/*
 * xid8300 - CUPS backend for the Matica XID8300 retransfer card printer.
 *
 * Reads the spool-record stream produced by rastertoxid8300 (or by the
 * Windows driver) and speaks the TCP protocol of PCP24CT64.dll, following the
 * call sequence of the Windows port monitor PDR24PM64.dll (docs/PROTOCOL.md §2-3).
 *
 * Device URI:  xid8300://host[:port]      (default port 50710)
 *
 * Magnetic stripe encoding (records 0x40..0x47) follows §6.  Smart-card
 * encoding (records 0x20/0x21, §7) positions the card and engages the station
 * like the Windows monitor, then runs an external encoding program in place of
 * the vendor plug-ins PDR24IC0/IC1.DLL.
 * Not implemented: K-secure (0x88), interactive magnetic plug-in (0x70).
 */

#include "xid8300.h"

#include <cups/backend.h>
#include <cups/cups.h>
#include <cups/http.h>

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define REPLY_TIMEOUT_MS    60000     /* P_CheckReply timeout used by the monitor */
#define GREETING_TIMEOUT_MS 30000     /* P_Connect waits 30 s for the greeting */
#define MAX_FRAME           0x10000
#define RETRY_LIMIT         3000      /* busy retries at 100 ms (~5 min) */
#define CONNECT_RETRY_SEC   10

/* Busy states returned for an error reply carrying these statuses (§2.4). */
#define XST_BUSY_2          0x2D300
#define XST_BUSY_4          0x2D400
#define XST_BUSY_5          0x2D500
#define XERR_COMM           (-0x3000000)
#define XERR_PROTO          (-0x4100021)
#define XERR_RIBBON         (-0x10001102)   /* Windows 0xEFFFEFFE "ribbon specification" */
#define XERR_IC_NOPROG      (-0x10000001)   /* "IC encoding program is not found" */
#define XERR_IC_FAILED      (-0x10000003)   /* program failed / timed out */
#define XERR_IC_REJECT      (-0x10001001)   /* program exit 10 = plug-in 0x1001 */
#define XERR_IC_CANCEL      (-0x10001002)   /* program exit 11 = plug-in 0x1002 */

#ifndef XID8300_LIBDIR
#  define XID8300_LIBDIR    "/usr/lib/cups/xid8300"
#endif

/* Plane slots, in the order the monitor sends them with ImageOut. */
enum { PL_Y, PL_M, PL_C, PL_K, PL_KS, PL_UV, PL_86, PL_COUNT };
/* K-secure (0x88) is sent with the K plane id (0x50405cf0). */
static const uint8_t plane_id[PL_COUNT] = { 0x01, 0x02, 0x04, 0x08, 0x08, 0x40, 0x20 };

/* ImageOut / PrintOnFilm selection masks (as in PDR24PM64). */
#define M_YMC  0x01
#define M_K    0x02
#define M_UV   0x04
#define M_86   0x08
#define M_KS   0x200

typedef struct
{
  uint8_t  *plane[PL_COUNT];
  uint32_t plane_len[PL_COUNT];
  uint32_t uv_param;
  uint8_t  *settings;
  uint32_t settings_len;
  uint8_t  *ic[2];          /* record 0x20 = contact, 0x21 = contactless */
  uint32_t ic_len[2];
  uint8_t  *track[8];       /* records 0x40..0x47: characters, no sentinels */
  uint32_t track_len[8];
  int      has_tracks;
  int      has_data;
} side_t;

/* Card positions used by LoadCard/MoveCard (§6.3). */
#define POS_PRINT        0
#define POS_CONTACT      1
#define POS_CONTACTLESS  2
#define POS_MAG          3
#define POS_EJECT    4

typedef struct
{
  int      fd;
  uint32_t tag;
} xid_t;

typedef struct
{
  uint8_t  kind;            /* F1 / F3 */
  uint16_t code;
  uint32_t status;
  uint8_t  body[MAX_FRAME];
  size_t   body_len;
} reply_t;

typedef enum { CARD_FINAL, CARD_FIRST, CARD_MID } card_mode_t;

static int loadflag;
static int mag_option;          /* job header byte 6 (DM+0xee) */
static int ic_option;           /* job header byte 7 (DM+0xef) */
static const char *printer_host;
static int card_number;
static int verbose_frames;

/* Encoding requested for the card being printed and not yet done. */
static side_t *pending_encode;

static int run_encoders(xid_t *x, side_t *s);


/*
 * Utilities.
 */

static void
msleep(int ms)
{
  struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };

  nanosleep(&ts, NULL);
}

static void
hexdump(const char *label, const uint8_t *p, size_t len)
{
  char   line[3 * 32 + 1];
  size_t i, n;

  for (i = 0; i < len; i += 32)
  {
    for (n = 0; n < 32 && i + n < len; n ++)
      snprintf(line + 3 * n, 4, "%02x ", p[i + n]);
    fprintf(stderr, "DEBUG: %s +%04zx: %s\n", label, i, line);
  }
}

static int
wait_readable(int fd, int ms)
{
  fd_set         rfds;
  struct timeval tv;
  int            rc;

  do
  {
    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);
    tv.tv_sec  = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    rc = select(fd + 1, &rfds, NULL, NULL, &tv);
  }
  while (rc < 0 && errno == EINTR);

  return (rc);
}

static int
send_all(int fd, const uint8_t *p, size_t len)
{
  while (len)
  {
    ssize_t n = send(fd, p, len, 0);

    if (n < 0)
    {
      if (errno == EINTR)
        continue;
      return (-1);
    }

    p   += n;
    len -= (size_t)n;
  }

  return (0);
}

static int
recv_exact(int fd, uint8_t *p, size_t len, int ms)
{
  while (len)
  {
    ssize_t n;

    if (wait_readable(fd, ms) <= 0)
      return (-1);

    if ((n = recv(fd, p, len, 0)) <= 0)
    {
      if (n < 0 && errno == EINTR)
        continue;
      return (-1);
    }

    p   += n;
    len -= (size_t)n;
  }

  return (0);
}


/*
 * 'recv_frame()' - read one frame: 8-byte header whose bytes 4..7 give the
 * number of 32-bit words that follow (§3.2).  Returns 1 = frame, 0 = no data
 * within 'ms', -1 = error.
 */

static int
recv_frame(xid_t *x, uint8_t *buf, size_t *len, int ms)
{
  uint32_t words;
  size_t   total;
  int      rc;

  if ((rc = wait_readable(x->fd, ms)) == 0)
    return (0);
  if (rc < 0 || recv_exact(x->fd, buf, 8, REPLY_TIMEOUT_MS))
    return (-1);

  words = xid_get_be32(buf + 4);
  total = 8 + (size_t)words * 4;

  if (words > (MAX_FRAME - 8) / 4)
  {
    fprintf(stderr, "ERROR: Printer sent an oversized frame (%u words).\n", words);
    return (-1);
  }

  if (total > 8 && recv_exact(x->fd, buf + 8, total - 8, REPLY_TIMEOUT_MS))
    return (-1);

  *len = total;

  if (verbose_frames)
    hexdump("recv", buf, total < 64 ? total : 64);

  return (1);
}


/*
 * Frame builders (§3.2-3.4).  All fields big-endian.
 */

static void
frame_header(uint8_t *f, uint8_t kind, uint16_t code, size_t total, uint8_t flags)
{
  memset(f, 0, 16);
  f[0]  = kind;
  f[1]  = (uint8_t)(code >> 8);
  f[2]  = (uint8_t)code;
  xid_put_be32(f + 4, (uint32_t)(total / 4 - 2));
  f[11] = flags;
}

/* Card/print control command (code 1): 20 bytes, sub-command + 3 bytes. */
static size_t
build_ctl(uint8_t *f, uint8_t flags, uint8_t sub, uint8_t p1, uint8_t p2)
{
  frame_header(f, 0xF0, 1, 20, flags);
  f[16] = sub;
  f[17] = 2;
  f[18] = p1;
  f[19] = p2;
  return (20);
}

/* Printer-info request list (codes 5 and 6): 4-byte items {id,0,0,0}. */
static size_t
build_info(uint8_t *f, uint16_t code, const uint8_t *ids, int n)
{
  size_t total = 16 + 4 * (size_t)n;

  frame_header(f, 0xF0, code, total, 4);
  for (int i = 0; i < n; i ++)
  {
    f[16 + 4 * i]     = ids[i];
    f[16 + 4 * i + 1] = 0;
    f[16 + 4 * i + 2] = 0;
    f[16 + 4 * i + 3] = 0;
  }
  return (total);
}


/*
 * 'xid_command()' - send a command and wait for its F1 reply (same tag).
 * Unsolicited F3 frames are logged and skipped.
 */

static int
xid_command(xid_t *x, uint8_t *f, size_t len, reply_t *r)
{
  uint8_t *buf = malloc(MAX_FRAME);
  size_t  rlen;
  int     rc = XERR_COMM;

  if (!buf)
    return (XERR_COMM);

  x->tag ++;
  xid_put_be32(f + 12, x->tag);

  if (verbose_frames)
    hexdump("send", f, len < 32 ? len : 32);

  if (send_all(x->fd, f, len))
  {
    fprintf(stderr, "ERROR: Unable to send to printer: %s\n", strerror(errno));
    goto done;
  }

  for (;;)
  {
    int got = recv_frame(x, buf, &rlen, REPLY_TIMEOUT_MS);

    if (got <= 0)
    {
      fputs("ERROR: No reply from printer.\n", stderr);
      goto done;
    }

    if (rlen < 16)
    {
      fprintf(stderr, "DEBUG: Ignoring short frame (%zu bytes).\n", rlen);
      continue;
    }

    if (buf[0] == 0xF3)
    {
      fprintf(stderr, "DEBUG: Printer notification F3 code %u.\n", xid_get_be16(buf + 1));
      continue;
    }

    if (buf[0] != 0xF1)
    {
      fprintf(stderr, "ERROR: Unexpected frame kind 0x%02X from printer.\n", buf[0]);
      goto done;
    }

    if (xid_get_be32(buf + 12) != x->tag)
    {
      fprintf(stderr, "DEBUG: Reply for tag %u while waiting for %u, skipped.\n",
              xid_get_be32(buf + 12), x->tag);
      continue;
    }

    r->kind     = buf[0];
    r->code     = xid_get_be16(buf + 1);
    r->status   = xid_get_be32(buf + 8);
    r->body_len = rlen - 16;
    memcpy(r->body, buf + 16, r->body_len);
    rc = 0;
    break;
  }

done:
  free(buf);
  return (rc);
}


/*
 * 'classify()' - map a reply to the port monitor's result convention:
 * 0 = OK, 2/4/5 = busy (0x2D300/0x2D400/0x2D500), negative = error.
 */

static int
classify(int rc, const reply_t *r)
{
  if (rc < 0)
    return (rc);

  if (r->code == 2)
  {
    switch (r->status)
    {
      case XST_BUSY_2 : return (2);
      case XST_BUSY_4 : return (4);
      case XST_BUSY_5 : return (5);
      default :         return (-(int)((r->status & 0xffffff) | 0x1000000));
    }
  }

  if (r->status)
    return (-(int)(0x4000000 | 1));

  return (0);
}

/*
 * Printer status texts from the Windows monitor's message table (PDR24PM64
 * 0x50410420, docs/PROTOCOL.md §2.4).  'wait' = 1: the monitor offers "Retry", so
 * wait here; 2: power saving, wake the printer with RezeroUnit first
 * (0x504059a0); 0: an error that needs the operator.
 */

typedef struct
{
  uint32_t   status;
  int        wait;
  const char *text;
} xid_status_t;

static const xid_status_t xid_statuses[] =
{
  { 0x02D000, 1, "No card. (Cards have run out.)" },
  { 0x02D100, 1, "The printer door is open, or the cleaning unit is detached." },
  { 0x02DA00, 1, "Preheating is in progress." },
  { 0x02DB00, 1, "The printer is initializing." },
  { 0x02DC00, 1, "Offline testing or cleaning is in progress." },
  { 0x02DD00, 1, "Operation panel is currently in use, or the printer is in the transport mode." },
  { 0x02FD00, 2, "The printer is in the Power Saving mode." },
  { 0x02FE00, 1, "Password authentication is not complete." },
  { 0x062800, 1, "The printer is initializing." },
  { 0x062900, 1, "The printer is initializing." },
  { 0x42A200, 1, "Retransfer film has run out." },
  { 0x42B200, 1, "Ink ribbon has run out." },
  { 0x039000, 0, "Failure occurred during card loading." },
  { 0x039100, 0, "Card jam near the card turnover unit." },
  { 0x039200, 0, "Card jam near the magnetic encoder." },
  { 0x039300, 0, "Card jam near the card feed roller." },
  { 0x039400, 0, "Card jam near the card outlet." },
  { 0x039500, 0, "Card jam near the retransfer heating roller." },
  { 0x03A100, 0, "Retransfer film is broken or is not detected." },
  { 0x03AD00, 0, "An error has occurred during magnetic encoding data writing." },
  { 0x03AE00, 0, "An error has occurred during magnetic encoding data reading." },
  { 0x03B000, 0, "An incorrect ink ribbon is installed." },
  { 0x03B100, 0, "Ink ribbon is broken or is not detected." },
  { 0x052700, 0, "Ink ribbon specification is incorrect." },
  { 0x052B00, 0, "Magnetic encoding data is incorrect." },
  { 0x052C00, 0, "IC encoder is not installed." },
  { 0x052D00, 0, "Magnetic encoder is not installed." },
  { 0x052E00, 0, "Optional devices are not installed." }
};

#define WAIT_LIMIT_MS   (30 * 60 * 1000)      /* give up waiting after 30 minutes */

static uint32_t wait_status;                  /* status currently waited on, 0 = none */
static int      wait_reported_ms;

/* Printer status carried by an error result of classify(), if any. */
static int
printer_status(int rc, uint32_t *st)
{
  if (rc >= 0 || ((uint32_t)(-rc) & 0xff000000) != 0x01000000)
    return (0);

  *st = (uint32_t)(-rc) & 0xffffff;
  return (1);
}

static const xid_status_t *
find_status(uint32_t st)
{
  for (size_t i = 0; i < sizeof(xid_statuses) / sizeof(xid_statuses[0]); i ++)
    if (xid_statuses[i].status == st)
      return (xid_statuses + i);

  return (NULL);
}

/* Text for a status; unknown codes fall back to the sense-key class (0x504122e0). */
static const char *
status_text(uint32_t st)
{
  const xid_status_t *s = find_status(st);

  if (s)
    return (s->text);

  switch ((st >> 16) & 0x0f)
  {
    case 2 :  return ("The printer is not ready.");
    case 3 :  return ("A media error has occurred.");
    case 4 :  return ("A hardware error has occurred.");
    case 5 :  return ("A command error has occurred.");
    case 6 :  return ("An Unit Attention has occurred.");
    default : return ("Unknown printer status.");
  }
}

/*
 * 'wait_on_status()' - if 'rc' is a status the Windows monitor waits on, show
 * its message, wake the printer if needed, sleep a second and return 1 so the
 * caller repeats the command.  Returns 0 for anything else.
 */

static int
wait_on_status(xid_t *x, int rc, int *waited_ms)
{
  uint32_t           st;
  const xid_status_t *s;

  if (!printer_status(rc, &st) || (s = find_status(st)) == NULL || !s->wait)
    return (0);

  if (*waited_ms >= WAIT_LIMIT_MS)
  {
    fprintf(stderr, "ERROR: Gave up after waiting 30 minutes: %s\n", s->text);
    return (0);
  }

  if (st != wait_status || *waited_ms - wait_reported_ms >= 30000)
  {
    fprintf(stderr, "INFO: Waiting for the printer: %s (printer status 0x%06X)\n", s->text, st);
    wait_status      = st;
    wait_reported_ms = *waited_ms;
  }

  if (s->wait == 2)
  {
    uint8_t f[20];
    reply_t *r = malloc(sizeof(reply_t));

    /* RezeroUnit(1), as the monitor does for power saving (0x5040ad70). */
    build_ctl(f, 0, 0x03, 0x01, 0x00);
    for (int n = 0; n < 10 && r; n ++)
      if (classify(xid_command(x, f, sizeof(f), r), r) <= 0)
        break;
      else
        msleep(100);
    free(r);
    fputs("DEBUG: Sent RezeroUnit to wake the printer.\n", stderr);
  }

  msleep(1000);
  *waited_ms += 1000;
  return (1);
}

static void
wait_done(void)
{
  if (wait_status)
  {
    fputs("INFO: Printer ready, continuing.\n", stderr);
    wait_status = 0;
  }
}

/* Send until the printer stops answering "busy" (monitor: sleep 100 ms). */
static int
command_retry(xid_t *x, uint8_t *f, size_t len, reply_t *r, int max_tries)
{
  int waited = 0;

  for (int n = 1;; n ++)
  {
    int rc = classify(xid_command(x, f, len, r), r);

    if (wait_on_status(x, rc, &waited))
    {
      n = 0;
      continue;
    }

    if (rc == 0)
      wait_done();

    if (rc <= 0 || n >= max_tries)
      return (rc);

    msleep(100);
  }
}

static void
report_error(const char *what, int rc)
{
  if (rc == XERR_COMM)
    fprintf(stderr, "ERROR: %s: communication error.\n", what);
  else if (rc == XERR_PROTO)
    fprintf(stderr, "ERROR: %s: unexpected reply from printer.\n", what);
  else
  {
    uint32_t st;

    if (printer_status(rc, &st))
      fprintf(stderr, "ERROR: %s failed: %s (printer status 0x%06X)\n", what, status_text(st), st);
    else
      fprintf(stderr, "ERROR: %s failed (code 0x%08X).\n", what, (unsigned)(-rc));
  }
}


/*
 * Individual commands (§3.4).
 */

static int
cmd_tur(xid_t *x)
{
  uint8_t f[20];
  reply_t *r = malloc(sizeof(reply_t));
  int     rc;

  build_ctl(f, 0, 0x01, 0, 0);
  rc = classify(xid_command(x, f, sizeof(f), r), r);
  free(r);
  return (rc);
}

/* TestUnitReady polling; 'accept' is the busy state that also counts as done. */
static int
tur_wait(xid_t *x, int accept)
{
  int waited = 0;

  for (int n = 0; n < 10 * 60 * 10; n ++)       /* ~10 minutes */
  {
    int rc = cmd_tur(x);

    if (wait_on_status(x, rc, &waited))
      continue;
    if (rc == 0 || (accept && rc == accept))
    {
      wait_done();
      return (0);
    }
    if (rc < 0)
      return (rc);

    if (n == 50)
      fputs("INFO: Waiting for printer...\n", stderr);

    msleep(rc == 1 ? 20 : 100);
  }

  return (XERR_PROTO);
}

/* Returns position >= 0, -1/-2 = no card, -3 = error (W_ReadPosition). */
static int
cmd_read_position(xid_t *x)
{
  uint8_t f[20];
  reply_t *r = malloc(sizeof(reply_t));
  int     rc = -3;

  build_ctl(f, 0, 0x02, 0, 0);

  for (int n = 0; n < 10; n ++)
  {
    int c = classify(xid_command(x, f, sizeof(f), r), r);

    if (c > 0)
    {
      msleep(100);
      continue;
    }

    if (c == 0 && r->code == 3 && r->body_len >= 4 && r->body[0] == 1)
    {
      uint8_t flags = r->body[2];

      if (flags & 4)
        rc = (flags & 1) ? -2 : -1;
      else
        rc = r->body[3];
    }
    else if (c < 0)
      report_error("ReadPosition", c);
    else
      fprintf(stderr, "ERROR: ReadPosition: unexpected reply code %u.\n", r->code);
    break;
  }

  free(r);
  fprintf(stderr, "DEBUG: Card position %d.\n", rc);
  return (rc);
}

static int
cmd_load_card(xid_t *x, int pos, int flag)
{
  uint8_t f[20];
  reply_t *r = malloc(sizeof(reply_t));
  int     rc;

  build_ctl(f, 2, 0x04, flag ? 0x10 : 0x00, (uint8_t)pos);
  rc = command_retry(x, f, sizeof(f), r, 10);
  free(r);
  return (rc);
}

static int
cmd_move_card(xid_t *x, int pos, int p5, int p6)
{
  uint8_t f[20];
  reply_t *r = malloc(sizeof(reply_t));
  uint8_t flags = p5 ? (p6 ? 0x90 : 0x10) : (p6 ? 0x80 : 0x00);
  int     rc;

  build_ctl(f, 2, 0x05, flags, (uint8_t)pos);
  rc = command_retry(x, f, sizeof(f), r, 10);
  free(r);
  return (rc);
}

static int
cmd_print_on_film(xid_t *x, unsigned mask, int second)
{
  uint8_t f[20];
  uint8_t p1 = second ? 0x20 : 0x00, p2 = 0;
  reply_t *r = malloc(sizeof(reply_t));
  int     rc;

  if ((mask & 0x30) == 0x10)
    p1 |= 1;
  else if ((mask & 0x30) == 0x20)
    p1 |= 2;

  if (mask & M_YMC) p2 |= 0x07;
  if (mask & M_K)   p2 |= 0x08;
  if (mask & M_UV)  p2 |= 0x40;
  if (mask & M_86)  p2 |= 0x20;

  build_ctl(f, 2, 0x06, p1, p2);
  rc = command_retry(x, f, sizeof(f), r, RETRY_LIMIT);
  free(r);
  return (rc);
}

/* P_SecurityPrint: sub-command 0x0B, P1 = second side, P2 = 8 (K panel). */
static int
cmd_security_print(xid_t *x, int second)
{
  uint8_t f[20];
  reply_t *r = malloc(sizeof(reply_t));
  int     rc;

  build_ctl(f, 2, 0x0b, second ? 0x20 : 0x00, 0x08);
  rc = command_retry(x, f, sizeof(f), r, RETRY_LIMIT);
  free(r);
  return (rc);
}

static int
cmd_print_on_card(xid_t *x, card_mode_t mode)
{
  uint8_t f[24];
  reply_t *r = malloc(sizeof(reply_t));
  int     rc;

  frame_header(f, 0xF0, 1, 24, 2);
  f[16] = 0x07;
  f[17] = 6;
  f[18] = mode == CARD_FIRST ? 0x10 : 0x00;
  f[19] = mode == CARD_FINAL ? 5 : 0;
  memset(f + 20, 0, 4);

  rc = command_retry(x, f, sizeof(f), r, RETRY_LIMIT);
  free(r);
  return (rc);
}

static int
cmd_image_out(xid_t *x, uint8_t id, const uint8_t *data, uint32_t len, int second)
{
  size_t  total = ((size_t)len + 0x1f) & ~(size_t)3;
  uint8_t *f    = calloc(1, total);
  reply_t *r    = malloc(sizeof(reply_t));
  int     rc;

  if (!f || !r)
  {
    free(f);
    free(r);
    return (XERR_COMM);
  }

  frame_header(f, 0xF0, 2, total, 0);
  f[16] = id;
  f[17] = second ? 0x20 : 0x00;
  xid_put_be32(f + 20, (uint32_t)(total - 0x18));
  xid_put_be32(f + 24, len);
  memcpy(f + 28, data, len);

  rc = command_retry(x, f, total, r, RETRY_LIMIT);
  free(f);
  free(r);
  return (rc);
}

/* Status (code 6) / property (code 5) queries; reply body is logged. */
static int
cmd_info(xid_t *x, uint16_t code, const uint8_t *ids, int n, uint16_t want, size_t want_len)
{
  uint8_t f[64];
  reply_t *r = malloc(sizeof(reply_t));
  size_t  len = build_info(f, code, ids, n);
  int     rc;

  rc = command_retry(x, f, len, r, 10);

  if (rc == 0)
  {
    if (r->code != want || r->body_len == 0)
    {
      fprintf(stderr, "DEBUG: Info request %u: reply code %u, %zu bytes.\n",
              code, r->code, r->body_len);
      rc = XERR_PROTO;
    }
    else
    {
      /* The monitor insists on 12 bytes for the initial request; the real
       * reply size is unconfirmed, so only warn. */
      if (want_len && r->body_len != want_len)
        fprintf(stderr, "WARNING: Info request %u returned %zu bytes (expected %zu).\n",
                code, r->body_len, want_len);
      hexdump(code == 6 ? "status" : "property", r->body, r->body_len);
    }
  }

  free(r);
  return (rc);
}

/*
 * Settings (code 8): TLVs {id, padded_len-2, len, data} copied from the
 * 0xC0 record in the order the port monitor uses (0x5040c014).
 */

static int
cmd_settings(xid_t *x, const uint8_t *items, uint32_t items_len)
{
  static const uint8_t order[] =
  {
    0x46, 0x47, 0x48, 0x49, 0x4a, 0x4b, 0x4c, 0x4d, 0x4e, 0x4f, 0x50, 0x55,
    0x56, 0x57, 0x5b, 0x28, 0x29, 0x2a, 0xc8, 0x5d, 0x5e, 0x5f
  };
  uint8_t f[16 + 4 * 0x7c];
  size_t  off = 16;
  reply_t *r;
  int     rc;

  for (size_t o = 0; o < sizeof(order); o ++)
  {
    for (uint32_t i = 0; i + 3 <= items_len; i += 2u + items[i + 1])
    {
      uint8_t dlen = items[i + 2];
      size_t  ent  = (3 + (size_t)dlen + 3) & ~(size_t)3;

      if (items[i] != order[o] || i + 3 + dlen > items_len)
        continue;
      if (off + ent > sizeof(f))
        break;

      memset(f + off, 0, ent);
      f[off]     = items[i];
      f[off + 1] = (uint8_t)(ent - 2);
      f[off + 2] = dlen;
      memcpy(f + off + 3, items + i + 3, dlen);
      off += ent;
      break;
    }
  }

  frame_header(f, 0xF0, 8, off, 4);

  r  = malloc(sizeof(reply_t));
  rc = command_retry(x, f, off, r, RETRY_LIMIT);
  free(r);
  return (rc);
}


/*
 * Session (§3.1, §3.3).
 */

static void
put_utf16le(uint8_t *dst, size_t max_chars, const char *s)
{
  const unsigned char *p = (const unsigned char *)s;
  size_t              n  = 0;

  memset(dst, 0, max_chars * 2);

  while (*p && n < max_chars)
  {
    uint32_t cp;

    if (*p < 0x80)
      cp = *p ++;
    else if ((*p & 0xe0) == 0xc0 && p[1])
    {
      cp = ((uint32_t)(p[0] & 0x1f) << 6) | (p[1] & 0x3f);
      p += 2;
    }
    else if ((*p & 0xf0) == 0xe0 && p[1] && p[2])
    {
      cp = ((uint32_t)(p[0] & 0x0f) << 12) | ((uint32_t)(p[1] & 0x3f) << 6) | (p[2] & 0x3f);
      p += 3;
    }
    else
    {
      cp = '?';
      p ++;
      while ((*p & 0xc0) == 0x80)
        p ++;
    }

    dst[2 * n]     = (uint8_t)cp;
    dst[2 * n + 1] = (uint8_t)(cp >> 8);
    n ++;
  }
}

static int
open_socket(const char *host, int port)
{
  struct addrinfo hints, *res, *ai;
  char            portstr[16];
  int             fd = -1, err;

  memset(&hints, 0, sizeof(hints));
  hints.ai_family   = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  snprintf(portstr, sizeof(portstr), "%d", port);

  if ((err = getaddrinfo(host, portstr, &hints, &res)) != 0)
  {
    fprintf(stderr, "ERROR: Unable to resolve \"%s\": %s\n", host, gai_strerror(err));
    return (-1);
  }

  for (ai = res; ai; ai = ai->ai_next)
  {
    if ((fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol)) < 0)
      continue;
    if (!connect(fd, ai->ai_addr, ai->ai_addrlen))
      break;
    close(fd);
    fd = -1;
  }

  freeaddrinfo(res);
  return (fd);
}

/* Returns 0 = session ready, 1 = retry later, -1 = fatal. */
static int
xid_session_start(xid_t *x, const char *host, int port, const char *user, const char *title)
{
  uint8_t   *buf = malloc(MAX_FRAME);
  uint8_t   job[0x7c];
  size_t    len;
  time_t    now;
  struct tm tm;
  int       waiting_msg = 0;
  int       rc = 1;
  static const uint8_t prop_ids[] = { 0x0d };

  x->tag = 0;

  if ((x->fd = open_socket(host, port)) < 0)
  {
    fprintf(stderr, "INFO: Unable to connect to %s:%d (%s), will retry.\n", host, port, strerror(errno));
    free(buf);
    return (1);
  }

  fputs("STATE: -connecting-to-device\n", stderr);

  /* Greeting: 0x78 bytes, F3 00 02 = accepted, F3 00 01 = refused. */
  if (recv_frame(x, buf, &len, GREETING_TIMEOUT_MS) <= 0 || len != 0x78 || buf[0] != 0xF3)
  {
    fputs("INFO: No valid greeting from printer, will retry.\n", stderr);
    goto fail;
  }

  hexdump("greeting", buf, len);

  if (xid_get_be16(buf + 1) != 2)
  {
    fprintf(stderr, "INFO: Printer refused the session (code %u), will retry.\n", xid_get_be16(buf + 1));
    goto fail;
  }

  /* Start job (P_SendPrintJob). */
  memset(job, 0, sizeof(job));
  job[0]  = 0xF2;
  job[2]  = 3;
  xid_put_be32(job + 4, (sizeof(job) - 8) / 4);
  job[11] = 2;
  memset(job + 12, 0x99, 4);
  now = time(NULL);
  localtime_r(&now, &tm);
  job[0x14] = (uint8_t)tm.tm_year;
  job[0x15] = (uint8_t)tm.tm_mon;
  job[0x16] = (uint8_t)tm.tm_mday;
  job[0x17] = (uint8_t)tm.tm_hour;
  job[0x18] = (uint8_t)tm.tm_min;
  job[0x19] = (uint8_t)tm.tm_sec;
  put_utf16le(job + 0x1c, 16, user);
  put_utf16le(job + 0x3c, 32, title);

  if (send_all(x->fd, job, sizeof(job)))
    goto fail;

  /* Wait for go-ahead; F3 00 03 = still waiting -> answer with keep-alive. */
  for (;;)
  {
    int got = recv_frame(x, buf, &len, 1000);

    if (got < 0)
      goto fail;
    if (got == 0)
      continue;

    if (buf[0] == 0xF3 && xid_get_be16(buf + 1) == 4)
      break;

    if (buf[0] == 0xF3 && xid_get_be16(buf + 1) == 3)
    {
      static const uint8_t keepalive[8] = { 0xF2, 0x00, 0x01, 0x00, 0, 0, 0, 0 };

      if (!waiting_msg)
      {
        fputs("INFO: Printer is busy, waiting for go-ahead...\n", stderr);
        waiting_msg = 1;
      }

      if (send_all(x->fd, keepalive, sizeof(keepalive)))
        goto fail;
      continue;
    }

    fprintf(stderr, "ERROR: Unexpected frame 0x%02X/%u while waiting for go-ahead.\n",
            buf[0], xid_get_be16(buf + 1));
    rc = -1;
    goto fail;
  }

  /* Initial property request [0x0D]; the monitor expects a 12-byte body. */
  if ((rc = cmd_info(x, 5, prop_ids, 1, 5, 0x0c)) != 0)
  {
    report_error("Initial property request", rc);
    rc = -1;
    goto fail;
  }

  free(buf);
  return (0);

fail:
  close(x->fd);
  x->fd = -1;
  free(buf);
  return (rc);
}


/*
 * Card printing (0x504047f8, simplified to the cases the filter produces).
 */

static unsigned
side_mask(const side_t *s)
{
  unsigned m = 0;

  if (s->plane[PL_Y] || s->plane[PL_M] || s->plane[PL_C])
    m |= M_YMC;
  if (s->plane[PL_K])
    m |= M_K;
  if (s->plane[PL_UV])
    m |= M_UV | ((s->uv_param >> 4) & 0x30);
  if (s->plane[PL_86])
    m |= M_86;
  if (s->plane[PL_KS])
    m |= M_KS;

  return (m);
}

/* One film pass (W_ImageOut + FUN_504065e4): the planes selected by 'sel',
 * then PrintOnFilm - or SecurityPrint for the K-secure plane alone. */
static int
film_pass(xid_t *x, side_t *s, unsigned sel, int second)
{
  static const unsigned slot_sel[PL_COUNT] = { M_YMC, M_YMC, M_YMC, M_K, M_KS, M_UV, M_86 };
  /* UV mode bits (0x30) only accompany the UV plane (0x5040901c). */
  unsigned mask = side_mask(s) & (sel | ((sel & M_UV) ? 0x30u : 0u));
  int      rc;

  for (int i = 0; i < PL_COUNT; i ++)
  {
    if (!(sel & slot_sel[i]) || !s->plane[i])
      continue;

    fprintf(stderr, "DEBUG: ImageOut plane 0x%02x%s (%u bytes)%s\n", plane_id[i],
            i == PL_KS ? " [K-secure]" : "", s->plane_len[i], second ? " [second]" : "");

    if ((rc = cmd_image_out(x, plane_id[i], s->plane[i], s->plane_len[i], second)) != 0)
    {
      report_error("ImageOut", rc);
      return (rc);
    }
  }

  if (mask == M_KS)
  {
    fprintf(stderr, "DEBUG: SecurityPrint%s\n", second ? " [second]" : "");
    if ((rc = cmd_security_print(x, second)) != 0)
      report_error("SecurityPrint", rc);
    return (rc);
  }

  if (mask & 0x0f)
  {
    fprintf(stderr, "DEBUG: PrintOnFilm mask 0x%02x%s\n", mask, second ? " [second]" : "");
    if ((rc = cmd_print_on_film(x, mask, second)) != 0)
    {
      report_error("PrintOnFilm", rc);
      return (rc);
    }
  }

  return (0);
}

/*
 * 'film_and_transfer()' - a group of film passes followed by one transfer to
 * the card.  The monitor waits for TestUnitReady (0x2D300) between passes,
 * and runs the encoders after the first group (0x504047f8).
 */

static int
film_and_transfer(xid_t *x, side_t *s, const unsigned *passes, int npasses, int second,
                  card_mode_t mode)
{
  int rc;

  for (int i = 0; i < npasses; i ++)
  {
    if (i && (rc = tur_wait(x, 2)) != 0)
    {
      report_error("TestUnitReady", rc);
      return (rc);
    }

    if ((rc = film_pass(x, s, passes[i], second)) != 0)
      return (rc);
  }

  if ((rc = tur_wait(x, 4)) != 0)
  {
    report_error("TestUnitReady", rc);
    return (rc);
  }

  /* The monitor encodes after the first film group, while the card waits in
   * the encoder, then brings it to the print position. */
  if (pending_encode)
  {
    side_t *e = pending_encode;

    pending_encode = NULL;
    if ((rc = run_encoders(x, e)) != 0 || (rc = tur_wait(x, 4)) != 0)
      return (rc);
  }

  if ((rc = tur_wait(x, 2)) != 0)
  {
    report_error("TestUnitReady", rc);
    return (rc);
  }

  fprintf(stderr, "DEBUG: PrintOnCard %s\n", mode == CARD_FINAL ? "final (mode 5)" :
          mode == CARD_FIRST ? "first side (mode 0, flag 0x10)" : "intermediate (mode 0)");
  if ((rc = cmd_print_on_card(x, mode)) != 0)
    report_error("PrintOnCard", rc);

  return (rc);
}

static int
print_side(xid_t *x, side_t *s, int second, int final)
{
  static const unsigned plain[]      = { M_YMC | M_K | M_UV | M_86 };
  static const unsigned plain_cmk[]  = { M_YMC | M_K | M_86 };
  static const unsigned secure[]     = { M_YMC | M_K, M_KS, M_UV };
  static const unsigned secure_cmk[] = { M_YMC | M_K, M_KS };
  static const unsigned uv_only[]    = { M_UV };
  unsigned    mask = side_mask(s);
  card_mode_t last = final ? CARD_FINAL : CARD_FIRST;
  int         ksec, uv_separate, rc;

  /* 0x50406aac: the peel-off plane cannot share a ribbon with UV or K-secure. */
  if ((mask & M_86) && (mask & (M_UV | M_KS)))
  {
    fputs("ERROR: Ink ribbon specification is incorrect (peel-off plane combined with UV or K-secure).\n",
          stderr);
    return (XERR_RIBBON);
  }

  /* A side without any plane gets a blank K plane (monitor 0x50405cf0). */
  if (!(mask & (M_YMC | M_K | M_UV | M_86)))
  {
    s->plane[PL_K]     = calloc(1, XID_PLANE);
    s->plane_len[PL_K] = XID_PLANE;
    if (!s->plane[PL_K])
      return (XERR_COMM);
    mask = side_mask(s);
  }

  /* K-secure is only used together with a K plane. */
  ksec        = (mask & M_K) && (mask & M_KS);
  /* UV together with other panels -> separate transfer (0x50406aac returns 2). */
  uv_separate = (mask & M_UV) && (mask & (M_YMC | M_K | M_86));

  if (!uv_separate)
    return (ksec ? film_and_transfer(x, s, secure, 3, second, last)
                 : film_and_transfer(x, s, plain, 1, second, last));

  rc = ksec ? film_and_transfer(x, s, secure_cmk, 2, second, CARD_MID)
            : film_and_transfer(x, s, plain_cmk, 1, second, CARD_MID);

  return (rc ? rc : film_and_transfer(x, s, uv_only, 1, second, last));
}

/*
 * 'move_to()' - bring the card to 'target' (FUN_504059a0): feed a new card if
 * none is present, otherwise move it.  'quick' skips the TestUnitReady wait
 * the monitor does after an explicit move.
 */

static int
move_to(xid_t *x, int target, int quick)
{
  int pos = cmd_read_position(x), rc;

  if (pos == -3)
    return (XERR_PROTO);

  if (pos == target && !loadflag)
    return (0);

  fprintf(stderr, "DEBUG: Card %d -> position %d%s\n", pos, target, loadflag ? " (flag 0x10)" : "");

  if (pos == -1 || pos == -2)
    rc = cmd_load_card(x, target, loadflag);
  else
    rc = cmd_move_card(x, target, loadflag, 0);

  if (rc == 0 && (!quick || pos == -2))
    rc = tur_wait(x, 4);

  if (rc == 0)
    loadflag = 0;

  return (rc);
}

static void
eject_if_present(xid_t *x)
{
  int pos = cmd_read_position(x), rc;

  if (pos < 0)
    return;

  if ((rc = cmd_move_card(x, 4, 0, 1)) != 0)
  {
    report_error("Eject", rc);
    return;
  }

  /* FUN_504068b8: these error statuses also end the wait. */
  for (int n = 0; n < 3000; n ++)
  {
    rc = cmd_tur(x);
    if (rc <= 0)
      break;
    msleep(rc == 1 ? 20 : 100);
  }

  if (rc < 0 && rc != -0x102d000 && rc != -0x142a200 && rc != -0x142b200)
    report_error("Eject", rc);
}

/*
 * Magnetic stripe (docs/PROTOCOL.md §6).
 */

static uint32_t
mg_max(uint8_t fmt)
{
  switch (fmt)
  {
    case 0x16 :             return (76);
    case 0x18 : case 0x36 : return (79);
    case 0x24 :             return (37);
    case 0x34 :             return (104);
    default :               return (69);    /* 0x07, 0x17, 0x37 */
  }
}

/* P_WriteMG: sub-command 9 with up to three {format,len,data} blocks. */
static int
cmd_write_mg(xid_t *x, const uint8_t fmt[3], uint8_t *const data[3], const uint32_t len[3])
{
  uint32_t n = 0, sum = 0, l[3];
  size_t   alloc, off = 0x14;
  uint8_t  *f;
  reply_t  *r;
  int      rc;

  for (int i = 0; i < 3; i ++)
  {
    l[i] = 0;
    if (!fmt[i])
      continue;
    l[i] = len[i] > mg_max(fmt[i]) ? mg_max(fmt[i]) : len[i];
    sum += l[i];
    n ++;
  }

  if (!n)
    return (0);

  alloc = (0x14 + 2 * n + sum + 3) & ~(size_t)3;
  f     = calloc(1, alloc);
  r     = malloc(sizeof(reply_t));
  if (!f || !r)
  {
    free(f);
    free(r);
    return (XERR_COMM);
  }

  frame_header(f, 0xF0, 1, alloc, 0);
  f[16] = 0x09;
  f[17] = (uint8_t)(alloc - 0x12);
  f[19] = (uint8_t)n;

  for (int i = 0; i < 3; i ++)
  {
    if (!fmt[i])
      continue;

    f[off ++] = fmt[i];
    f[off ++] = (uint8_t)l[i];

    for (uint32_t j = 0; j < l[i]; j ++)
    {
      uint8_t c = data[i][j];

      if (fmt[i] == 0x07 || fmt[i] == 0x17 || fmt[i] == 0x37)
        f[off ++] = c & 0x7f;
      else if (fmt[i] == 0x24 || fmt[i] == 0x34)
        f[off ++] = c & 0x0f;
      else
        f[off ++] = (uint8_t)(c - 0x20) & 0x3f;
    }
  }

  fprintf(stderr, "DEBUG: WriteMG formats %02x/%02x/%02x lengths %u/%u/%u\n",
          fmt[0], fmt[1], fmt[2], l[0], l[1], l[2]);

  rc = command_retry(x, f, alloc, r, RETRY_LIMIT);
  free(f);
  free(r);
  return (rc);
}

static int
encode_mag(xid_t *x, side_t *s)
{
  /* Record 0x40+i -> format; later records override earlier (0x5040b968). */
  static const uint8_t t1_rec[] = { 1, 4, 6 }, t1_fmt[] = { 0x16, 0x17, 0x18 };
  static const uint8_t t3_rec[] = { 3, 5, 7 }, t3_fmt[] = { 0x34, 0x37, 0x36 };
  uint8_t  fmt[3]  = { 0, 0, 0 };
  uint8_t  *data[3] = { NULL, NULL, NULL };
  uint32_t len[3]  = { 0, 0, 0 };
  int      rc;

  fputs("INFO: Encoding magnetic stripe...\n", stderr);

  if ((rc = move_to(x, POS_MAG, 0)) != 0)
  {
    report_error("Card feed to magnetic encoder", rc);
    return (rc);
  }

  if (s->track[0])
  {
    fmt[0]  = 0x07;
    data[0] = s->track[0];
    len[0]  = s->track_len[0];

    if ((rc = cmd_write_mg(x, fmt, data, len)) != 0)
    {
      report_error("Magnetic encoding (JIS-II)", rc);
      return (rc);
    }

    fmt[0] = 0;
  }

  for (int i = 0; i < 3; i ++)
  {
    if (s->track[t1_rec[i]])
    {
      fmt[0]  = t1_fmt[i];
      data[0] = s->track[t1_rec[i]];
      len[0]  = s->track_len[t1_rec[i]];
    }

    if (s->track[t3_rec[i]])
    {
      fmt[2]  = t3_fmt[i];
      data[2] = s->track[t3_rec[i]];
      len[2]  = s->track_len[t3_rec[i]];
    }
  }

  if (s->track[2])
  {
    fmt[1]  = 0x24;
    data[1] = s->track[2];
    len[1]  = s->track_len[2];
  }

  if ((fmt[0] || fmt[1] || fmt[2]) && (rc = cmd_write_mg(x, fmt, data, len)) != 0)
  {
    report_error("Magnetic encoding", rc);
    return (rc);
  }

  return (0);
}


/*
 * Smart card (docs/PROTOCOL.md §7).
 */

/* P_ICControl: unit 0 = contact, 1 = contactless; release 0 = engage, 1 = release. */
static int
cmd_ic_control(xid_t *x, int unit, int release)
{
  uint8_t f[20];
  reply_t *r = malloc(sizeof(reply_t));
  int     rc;

  build_ctl(f, 0, 0x0a, (uint8_t)((((release & 1) << 2) | (unit & 1)) << 4), 0);
  rc = command_retry(x, f, sizeof(f), r, RETRY_LIMIT);
  free(r);
  return (rc);
}

/* P_FlushSettingReq item 1: 10 bytes handed to the Windows plug-in.  Failure is
 * ignored like in the monitor (the buffer stays zero). */
static size_t
cmd_encoder_info(xid_t *x, uint8_t info[10])
{
  static const uint8_t ids[] = { 0x01 };
  uint8_t f[32];
  reply_t *r = malloc(sizeof(reply_t));
  size_t  n = 0;

  memset(info, 0, 10);

  if (command_retry(x, f, build_info(f, 0x0a, ids, 1), r, 10) == 0 && r->code == 4)
  {
    for (size_t i = 0; i + 3 <= r->body_len; i += 2u + r->body[i + 1])
    {
      if (r->body[i] != 0x01)
        continue;
      n = r->body_len - (i + 3);
      if (n > 10)
        n = 10;
      memcpy(info, r->body + i + 3, n);
      break;
    }
  }

  free(r);
  return (n);
}

static const char *
ic_program(int contactless)
{
  static char path[1024];
  const char  *env = getenv(contactless ? "XID8300_IC_CONTACTLESS" : "XID8300_IC_CONTACT");

  if (env && *env)
    return (env);

  snprintf(path, sizeof(path), "%s/%s", XID8300_LIBDIR, contactless ? "ic-contactless" : "ic-contact");
  return (path);
}

/*
 * 'run_ic_program()' - stand-in for PDR24IC0/IC1.DLL!Encode.
 *
 *   program contact|contactless   (stdin = record data)
 *
 * Environment: XID8300_UNIT, XID8300_ENCODER_INFO (hex), XID8300_DATA_LENGTH,
 * XID8300_PRINTER, XID8300_CARD.  stdout/stderr go to the CUPS log, so the
 * program may print "INFO:" lines.  Exit 0 = OK, 10 = reject card (0x1001),
 * 11 = cancel job (0x1002), anything else = encoding error.
 */

static int
run_ic_program(const char *prog, int contactless, const uint8_t *data, uint32_t len,
               const uint8_t *info, size_t info_len)
{
  const char *unit = contactless ? "contactless" : "contact";
  char       infohex[21], lenstr[16], cardstr[16];
  int        fds[2], status, timeout;
  pid_t      pid;
  const char *t = getenv("XID8300_IC_TIMEOUT");

  timeout = t ? atoi(t) : 300;
  if (timeout <= 0)
    timeout = 300;

  for (size_t i = 0; i < info_len; i ++)
    snprintf(infohex + 2 * i, 3, "%02X", info[i]);
  infohex[2 * info_len] = '\0';
  snprintf(lenstr, sizeof(lenstr), "%u", len);
  snprintf(cardstr, sizeof(cardstr), "%d", card_number);

  if (pipe(fds))
    return (XERR_IC_FAILED);

  if ((pid = fork()) < 0)
  {
    close(fds[0]);
    close(fds[1]);
    return (XERR_IC_FAILED);
  }

  if (pid == 0)
  {
    dup2(fds[0], 0);
    dup2(2, 1);
    close(fds[0]);
    close(fds[1]);
    signal(SIGPIPE, SIG_DFL);
    setenv("XID8300_UNIT", unit, 1);
    setenv("XID8300_ENCODER_INFO", infohex, 1);
    setenv("XID8300_DATA_LENGTH", lenstr, 1);
    setenv("XID8300_PRINTER", printer_host ? printer_host : "", 1);
    setenv("XID8300_CARD", cardstr, 1);
    execl(prog, prog, unit, (char *)NULL);
    _exit(127);
  }

  close(fds[0]);
  for (uint32_t off = 0; off < len;)
  {
    ssize_t n = write(fds[1], data + off, len - off);

    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      break;                              /* program did not read its input */
    off += (uint32_t)n;
  }
  close(fds[1]);

  for (int waited = 0;; waited += 100)
  {
    pid_t got = waitpid(pid, &status, WNOHANG);

    if (got == pid)
      break;
    if (got < 0 && errno != EINTR)
      return (XERR_IC_FAILED);

    if (waited >= timeout * 1000)
    {
      fprintf(stderr, "ERROR: %s IC encoding program timed out after %d s.\n",
              contactless ? "Contactless" : "Contact", timeout);
      kill(pid, SIGKILL);
      waitpid(pid, &status, 0);
      return (XERR_IC_FAILED);
    }

    msleep(100);
  }

  if (WIFEXITED(status))
  {
    switch (WEXITSTATUS(status))
    {
      case 0 :
        return (0);
      case 10 :
        fprintf(stderr, "ERROR: %s IC encoding program rejected the card.\n",
                contactless ? "Contactless" : "Contact");
        return (XERR_IC_REJECT);
      case 11 :
        fprintf(stderr, "ERROR: %s IC encoding program cancelled the job.\n",
                contactless ? "Contactless" : "Contact");
        return (XERR_IC_CANCEL);
      case 127 :
        fprintf(stderr, "ERROR: Unable to run %s IC encoding program \"%s\".\n", unit, prog);
        return (XERR_IC_NOPROG);
      default :
        fprintf(stderr, "ERROR: An error has occurred during %s IC encoding (program exit %d).\n",
                unit, WEXITSTATUS(status));
        return (XERR_IC_FAILED);
    }
  }

  fprintf(stderr, "ERROR: %s IC encoding program was killed by signal %d.\n",
          contactless ? "Contactless" : "Contact", WIFSIGNALED(status) ? WTERMSIG(status) : 0);
  return (XERR_IC_FAILED);
}

static int
encode_ic(xid_t *x, side_t *s, int contactless)
{
  const char *prog = ic_program(contactless);
  const char *name = contactless ? "Contactless" : "Contact";
  uint8_t    info[10];
  size_t     info_len;
  int        rc, prc;

  if (access(prog, X_OK))
  {
    fprintf(stderr, "ERROR: %s IC encoding program is not found (%s).\n", name, prog);
    return (XERR_IC_NOPROG);
  }

  fprintf(stderr, "INFO: Encoding %s smart card...\n", contactless ? "contactless" : "contact");

  if ((rc = move_to(x, contactless ? POS_CONTACTLESS : POS_CONTACT, 0)) != 0)
  {
    report_error(contactless ? "Card feed to contactless station" : "Card feed to contact station", rc);
    return (rc);
  }

  if ((rc = cmd_ic_control(x, contactless, 0)) != 0)
  {
    report_error(contactless ? "Engage contactless station" : "Engage contact station", rc);
    return (rc);
  }

  info_len = cmd_encoder_info(x, info);
  fprintf(stderr, "DEBUG: %s encoder info: %zu bytes.\n", name, info_len);

  prc = run_ic_program(prog, contactless, s->ic[contactless], s->ic_len[contactless],
                       info, sizeof(info));

  /* The station is released even after a failed encode (0x5040939c). */
  if ((rc = cmd_ic_control(x, contactless, 1)) != 0)
    report_error(contactless ? "Release contactless station" : "Release contact station", rc);

  return (prc ? prc : rc);
}

static int need_contactless(const side_t *s) { return ((ic_option & 0x20) && s->ic[1]); }
static int need_contact(const side_t *s)     { return ((ic_option & 0x10) && s->ic[0]); }
static int need_mag(const side_t *s)         { return (mag_option && s->has_tracks); }

/* First station the card has to visit (0x50409a74). */
static int
first_station(const side_t *s)
{
  if (need_contactless(s)) return (POS_CONTACTLESS);
  if (need_contact(s))     return (POS_CONTACT);
  if (need_mag(s))         return (POS_MAG);
  return (POS_PRINT);
}

/* Contactless, contact, magnetic - then back to the print position (0x5040939c). */
static int
run_encoders(xid_t *x, side_t *s)
{
  int rc;

  if (need_contactless(s) && (rc = encode_ic(x, s, 1)) != 0)
    return (rc);

  if (need_contact(s) && (rc = encode_ic(x, s, 0)) != 0)
    return (rc);

  /* Byte-7 / byte-6 bit0: toggle the card flag after that encoder. */
  if ((ic_option & 0x30) && (ic_option & 1))
    loadflag = !loadflag;

  if (need_mag(s))
  {
    if ((rc = encode_mag(x, s)) != 0)
      return (rc);
    if (mag_option & 1)
      loadflag = !loadflag;
  }

  if ((rc = move_to(x, POS_PRINT, 1)) != 0)
    report_error("Card feed to print position", rc);

  return (rc);
}

/* Value of a 1-byte setting TLV, or -1 (§3.4). */
static int
setting_value(const side_t *s, uint8_t id)
{
  for (uint32_t i = 0; s->settings && i + 3 < s->settings_len; i += 2u + s->settings[i + 1])
    if (s->settings[i] == id && s->settings[i + 2] >= 1)
      return (s->settings[i + 3]);

  return (-1);
}

static int
print_card(xid_t *x, side_t *first, side_t *second, side_t *enc)
{
  static const uint8_t status_ids[] = { 0x0d };
  static const uint8_t prop_ids[]   = { 0x32, 0x33, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x3b };
  int rc;

  if ((rc = cmd_info(x, 6, status_ids, 1, 6, 0)) != 0)
  {
    report_error("Status request", rc);
    return (rc);
  }

  if ((rc = cmd_info(x, 5, prop_ids, (int)sizeof(prop_ids), 5, 0)) != 0)
  {
    report_error("Property request", rc);
    return (rc);
  }

  if ((rc = cmd_settings(x, first->settings, first->settings_len)) != 0)
  {
    report_error("Settings", rc);
    return (rc);
  }

  int station = first_station(enc), encode = station != POS_PRINT;

  if (enc->has_tracks && !mag_option)
    fputs("WARNING: Track data present but magnetic encoding is off in the job header; not encoded.\n", stderr);
  if ((enc->ic[0] && !(ic_option & 0x10)) || (enc->ic[1] && !(ic_option & 0x20)))
    fputs("WARNING: Smart-card data present but that station is off in the job header; not encoded.\n", stderr);

  pending_encode = NULL;

  /* With encoding the card is fed straight to the first station (0x50409a74). */
  if ((rc = move_to(x, station, 1)) != 0)
  {
    report_error("Card feed", rc);
    return (rc);
  }

  if (encode)
  {
    /* Settings TLV 0xF2 != 0 makes the monitor encode before printing. */
    if (setting_value(first, 0xF2) > 0)
    {
      if ((rc = tur_wait(x, 4)) != 0 || (rc = run_encoders(x, enc)) != 0)
        goto fail;
    }
    else
      pending_encode = enc;
  }

  if ((rc = print_side(x, first, 0, second == NULL)) != 0)
    goto fail;

  if (second && (rc = print_side(x, second, 1, 1)) != 0)
    goto fail;

  eject_if_present(x);
  return (0);

fail:
  /* The monitor ejects the card after a failed encoding (0x5040939c). */
  pending_encode = NULL;
  if (rc != XERR_COMM)
    eject_if_present(x);
  return (rc);
}


/*
 * Spool-record input (§1.2).
 */

static int
read_full(int fd, uint8_t *p, size_t len)
{
  size_t got = 0;

  while (got < len)
  {
    ssize_t n = read(fd, p + got, len - got);

    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      return (got == 0 ? 0 : -1);
    got += (size_t)n;
  }

  return (1);
}

static void
free_side(side_t *s)
{
  for (int i = 0; i < PL_COUNT; i ++)
    free(s->plane[i]);
  for (int i = 0; i < 8; i ++)
    free(s->track[i]);
  free(s->ic[0]);
  free(s->ic[1]);
  free(s->settings);
  memset(s, 0, sizeof(*s));
}

/* Read records up to the next 0xF3.  Returns 0 ok, -1 fatal; *eoj set at 0xF5/EOF. */
static int
read_side(int fd, side_t *s, int *eoj)
{
  uint8_t hdr[XREC_HDR_LEN];

  memset(s, 0, sizeof(*s));

  for (;;)
  {
    uint32_t type, len, param;
    uint8_t  *data = NULL;
    int      rc, slot = -1;

    if ((rc = read_full(fd, hdr, sizeof(hdr))) <= 0)
    {
      if (rc < 0)
        fputs("WARNING: Truncated print data.\n", stderr);
      *eoj = 1;
      return (0);
    }

    type  = xid_get_le32(hdr);
    len   = xid_get_le32(hdr + 4);
    param = xid_get_le32(hdr + 16);

    if (len > 32 * 1024 * 1024)
    {
      fprintf(stderr, "ERROR: Bad record 0x%02X (length %u).\n", type, len);
      return (-1);
    }

    if (len)
    {
      if ((data = malloc(len)) == NULL || read_full(fd, data, len) != 1)
      {
        fputs("ERROR: Truncated print data.\n", stderr);
        free(data);
        return (-1);
      }
    }

    switch (type)
    {
      case XREC_Y :   slot = PL_Y;  break;
      case XREC_M :   slot = PL_M;  break;
      case XREC_C :   slot = PL_C;  break;
      case XREC_K :   slot = PL_K;  break;
      case XREC_UV :  slot = PL_UV; s->uv_param = param; break;
      case XREC_P86 : slot = PL_86; break;

      case XREC_SETTINGS :
        free(s->settings);
        s->settings     = data;
        s->settings_len = len;
        data            = NULL;
        break;

      case XREC_ERROR :
        fprintf(stderr, "ERROR: The driver could not open a LUT/UV file (code 0x%08X).\n",
                len >= 4 ? xid_get_le32(data) : 0);
        free(data);
        return (-1);

      case XREC_EOS :
        free(data);
        return (0);

      case XREC_EOJ :
        free(data);
        *eoj = 1;
        return (0);

      case XREC_KSECURE : slot = PL_KS; break;

      case 0x40 : case 0x41 : case 0x42 : case 0x43 :
      case 0x44 : case 0x45 : case 0x46 : case 0x47 :
        free(s->track[type - 0x40]);
        s->track[type - 0x40]     = data;
        s->track_len[type - 0x40] = len;
        s->has_tracks             = len != 0;
        s->has_data               = 1;
        data                      = NULL;
        break;

      case 0x20 : case 0x21 :
        free(s->ic[type - 0x20]);
        s->ic[type - 0x20]     = data;
        s->ic_len[type - 0x20] = len;
        s->has_data            = 1;
        data                   = NULL;
        break;

      case 0x60 : case 0x70 :
        fprintf(stderr, "WARNING: Record 0x%02X ignored (user / interactive encoding plug-ins not implemented).\n", type);
        break;

      default :
        fprintf(stderr, "DEBUG: Ignoring record 0x%02X (%u bytes).\n", type, len);
        break;
    }

    if (slot >= 0)
    {
      if (len != XID_PLANE)
        fprintf(stderr, "WARNING: Plane 0x%02X has %u bytes, expected %d.\n", type, len, XID_PLANE);

      free(s->plane[slot]);
      s->plane[slot]     = data;
      s->plane_len[slot] = len;
      s->has_data        = 1;
      data               = NULL;
    }

    free(data);
  }
}


int
main(int argc, char *argv[])
{
  const char *uri;
  char       scheme[64], userpass[256], host[256], resource[1024];
  int        port, fd = 0, eoj = 0, duplex, backfirst, cards = 0;
  uint32_t   copies;
  uint8_t    hdr[XREC_HDR_LEN], job[20];
  xid_t      x = { -1, 0 };

  setbuf(stderr, NULL);
  signal(SIGPIPE, SIG_IGN);

  if (argc == 1)
  {
    puts("network xid8300 \"Unknown\" \"Matica XID8300 (TCP/IP)\"");
    return (CUPS_BACKEND_OK);
  }

  if (argc < 6 || argc > 7)
  {
    fputs("Usage: xid8300 job user title copies options [file]\n", stderr);
    return (CUPS_BACKEND_FAILED);
  }

  verbose_frames = getenv("XID8300_TRACE") != NULL;

  if ((uri = getenv("DEVICE_URI")) == NULL)
    uri = argv[0];

  if (httpSeparateURI(HTTP_URI_CODING_ALL, uri, scheme, sizeof(scheme), userpass,
                      sizeof(userpass), host, sizeof(host), &port, resource,
                      sizeof(resource)) < HTTP_URI_STATUS_OK || !host[0])
  {
    fprintf(stderr, "ERROR: Bad device URI \"%s\".\n", uri);
    return (CUPS_BACKEND_STOP);
  }

  if (port <= 0)
    port = XID_TCP_PORT;

  if (argc == 7 && (fd = open(argv[6], O_RDONLY)) < 0)
  {
    perror("ERROR: Unable to open print file");
    return (CUPS_BACKEND_FAILED);
  }

  /* The stream must start with the job header (PDR24PM64 WritePort). */
  if (read_full(fd, hdr, sizeof(hdr)) != 1 || xid_get_le32(hdr) != XREC_JOB ||
      xid_get_le32(hdr + 4) != sizeof(job) || read_full(fd, job, sizeof(job)) != 1)
  {
    fputs("ERROR: Print data does not start with an XID8300 job header (the queue must use the "
          "xid8300 PPD, and cups-filters/ghostscript are needed to print PDF or PostScript).\n", stderr);
    return (CUPS_BACKEND_FAILED);
  }

  copies    = xid_get_le32(job);
  duplex    = (job[4] & 0xfe) == XJOB_DUPLEX_DUPLEX;
  loadflag  = (xid_get_le32(job + 8) & XJOB_FLAG_LOADFLAG) != 0;
  backfirst = (xid_get_le32(job + 8) & XJOB_FLAG_BACKFIRST) != 0;
  if (copies == 0 || copies > 999)
    copies = 1;

  mag_option   = job[6];
  ic_option    = job[7];
  printer_host = host;

  fprintf(stderr, "DEBUG: Job: copies=%u duplex=%d host=%s port=%d\n", copies, duplex, host, port);

  while (!eoj)
  {
    side_t front, back;
    int    rc;

    memset(&back, 0, sizeof(back));

    if (read_side(fd, &front, &eoj))
      goto fail_data;

    if (duplex && !eoj && read_side(fd, &back, &eoj))
    {
      free_side(&front);
      goto fail_data;
    }

    if (!front.has_data && !back.has_data)
    {
      free_side(&front);
      free_side(&back);
      continue;
    }

    /* Connect once the first card is buffered. */
    while (x.fd < 0)
    {
      fputs("STATE: +connecting-to-device\n", stderr);
      rc = xid_session_start(&x, host, port, argv[2], argv[3]);
      if (rc < 0)
      {
        free_side(&front);
        free_side(&back);
        return (CUPS_BACKEND_RETRY_CURRENT);
      }
      if (rc > 0)
        sleep(CONNECT_RETRY_SEC);
    }

    for (uint32_t c = 0; c < copies; c ++)
    {
      cards ++;
      card_number = cards;
      fprintf(stderr, "INFO: Printing card %d...\n", cards);

      /* Encoding data always comes from the front side (local_198 in 0x504047f8). */
      if (!duplex)
        rc = print_card(&x, &front, NULL, &front);
      else if (backfirst)
        rc = print_card(&x, &back, &front, &front);
      else
        rc = print_card(&x, &front, &back, &front);

      if (rc)
      {
        free_side(&front);
        free_side(&back);
        close(x.fd);
        /* Plug-in 0x1002 deletes the job, 0x1001 ends it quietly; other
         * errors need the operator, as the Windows error dialog does. */
        if (rc == XERR_IC_CANCEL)
          return (CUPS_BACKEND_CANCEL);
        if (rc == XERR_IC_REJECT)
          return (CUPS_BACKEND_FAILED);
        return (CUPS_BACKEND_STOP);
      }
    }

    free_side(&front);
    free_side(&back);
  }

  if (x.fd >= 0)
    close(x.fd);

  fprintf(stderr, "INFO: %d card(s) printed.\n", cards);
  return (CUPS_BACKEND_OK);

fail_data:
  if (x.fd >= 0)
    close(x.fd);
  return (CUPS_BACKEND_FAILED);
}
