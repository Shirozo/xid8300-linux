/*
 * rastertoxid8300 - CUPS filter: CUPS raster -> Matica XID8300 spool records.
 *
 * The output is the record stream the Windows rendering DLL (PDR23GDx64.DLL)
 * hands to its port monitor; the xid8300 backend turns it into the TCP
 * protocol.  See docs/PROTOCOL.md §1 for the reverse-engineered format.
 *
 * Usage (as called by CUPS):
 *   rastertoxid8300 job user title copies options [file]
 *
 * Page sequence: each card side consumes one page (colour/K layer) plus one
 * extra page when the side's panel set includes UV (that page is the UV
 * layer: every non-white pixel is printed with UV).  With duplex enabled the
 * sides alternate front, back, front, back...
 */

#include "xid8300.h"
#include "xid8300_areas.h"

#include <cups/cups.h>
#include <cups/ppd.h>
#include <cups/raster.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define LUT_HEADER_LEN  16
#define LUT_BODY_LEN    0x3000000u          /* 256^3 entries * 3 bytes */

typedef enum
{
  BLACK_KONLY,         /* pure black -> K only (driver K-mode 0x10) */
  BLACK_KPLUSCOLOR,    /* pure black -> K, CMY kept (driver K-mode 0x11) */
  BLACK_COMPOSITE      /* no K extraction, black printed with CMY */
} black_mode_t;

typedef struct
{
  int panels;          /* XPANEL_* */
  int rotate180;       /* user option FrontRotate180 / BackRotate180 */
} side_cfg_t;

typedef struct
{
  uint8_t *y, *m, *c, *k, *uv;
  uint8_t *p86, *scratch;
  int     have_color;  /* colour/K layer page seen */
  int     reversed;    /* plane rotated 180 degrees (also selects UV mode) */
  int     portrait;    /* colour page was portrait */
} side_planes_t;

static ppd_file_t    *ppd;
static int           num_options;
static cups_option_t *options;

static black_mode_t  black_mode = BLACK_KONLY;
static int           black_threshold;          /* channel <= threshold counts as black */
static int           mono_dither;              /* K-only sides: Floyd-Steinberg */
static const uint8_t *lut;                      /* 256^3*3 CMY cube or NULL */
static int           fit_actual;               /* CardFit=Actual: no scaling to the print area */
static int           shift_x, shift_y;         /* ImageShiftX/Y in dots, plane (landscape) axes */
static uint8_t       settings[4 * 32];         /* 0xC0 record: {id, 02, 01, value} items (§1.4) */
static int           settings_len;

/* Colour adjustment (Windows "Color" dialog, GD 0x71905280), DM+0x22ac.. */
static int           adj_on, adj_bri, adj_con, adj_hue[3], adj_gamma[3];
static int           uv_mark = 1;              /* UVMarkPosition: 1 upper right, 2 lower left */
static int           uv_panels = 2;            /* UVFilmPanels: 0 auto, 1, 2 */


/*
 * 'get_option()' - job option, else marked PPD choice, else default.
 */

static const char *
get_option(const char *name, const char *def)
{
  const char   *val;
  ppd_choice_t *choice;

  if ((val = cupsGetOption(name, num_options, options)) != NULL)
    return (val);

  if (ppd && (choice = ppdFindMarkedChoice(ppd, name)) != NULL)
    return (choice->choice);

  return (def);
}

static int
get_bool(const char *name)
{
  const char *val = get_option(name, "False");

  return (!strcasecmp(val, "true") || !strcasecmp(val, "yes") ||
          !strcasecmp(val, "on") || !strcmp(val, "1"));
}

/*
 * Printer settings (Windows "Printer settings" and Encode/Laminate pages).
 * Each option is a choice or "Default"; only set values are sent, as the
 * driver skips bytes left at 0xFF.  Ids and encodings: PDR23UIx64 0x6a919340
 * and GD 0x7190a7f0 (docs/PROTOCOL.md §1.4).
 */

static void
add_setting(uint8_t id, int value)
{
  if (settings_len + 4 > (int)sizeof(settings))
    return;

  settings[settings_len ++] = id;
  settings[settings_len ++] = 0x02;
  settings[settings_len ++] = 0x01;
  settings[settings_len ++] = (uint8_t)value;
}

/* Integer option in [lo, hi], or INT_MIN for "Default"/unset/invalid. */
static int
int_option(const char *name, int lo, int hi)
{
  const char *val = get_option(name, "Default");
  char       *end;
  long       v;

  if (!strcasecmp(val, "Default"))
    return (INT_MIN);

  v = strtol(val[0] == '+' ? val + 1 : val, &end, 10);
  if (end == val || *end || v < lo || v > hi)
  {
    fprintf(stderr, "WARNING: %s=%s is not between %d and %d; using the printer's setting.\n",
            name, val, lo, hi);
    return (INT_MIN);
  }

  return ((int)v);
}

/* Choice option mapped to a value, or INT_MIN for "Default". */
static int
choice_option(const char *name, const char *const *names, const int *values, int n)
{
  const char *val = get_option(name, "Default");

  if (!strcasecmp(val, "Default"))
    return (INT_MIN);

  for (int i = 0; i < n; i ++)
    if (!strcasecmp(val, names[i]))
      return (values[i]);

  fprintf(stderr, "WARNING: Unknown %s value \"%s\"; using the printer's setting.\n", name, val);
  return (INT_MIN);
}

static void
parse_printer_settings(int mag_on, int ic_on)
{
  static const char *const std_fine[] = { "Standard", "Fine" };
  static const char *const std_mg[]   = { "Standard", "MGStripe" };
  static const char *const standby[]  = { "FrontWait", "BackWait" };
  static const char *const cooling[]  = { "Enable", "Disable" };
  static const char *const coerc[]    = { "LoCo", "HiCo" };
  static const char *const lamin[]    = { "Laminate", "PassThrough" };
  static const int         v01[]      = { 0, 1 };
  static const int         v10[]      = { 1, 0 };
  int v, t, sf, sb;

  /* Ink print levels: slider -3..+3 is sent as 0..6. */
  if ((v = int_option("YMCLevel", -3, 3)) != INT_MIN)      add_setting('H', v + 3);
  if ((v = int_option("KLevel", -3, 3)) != INT_MIN)        add_setting('F', v + 3);
  if ((v = int_option("UVLevel", -3, 3)) != INT_MIN)       add_setting('J', v + 3);
  if ((v = int_option("PeelOffLevel", -3, 3)) != INT_MIN)  add_setting('K', v + 3);

  if ((v = choice_option("KPrintMode", std_fine, v01, 2)) != INT_MIN)    add_setting('G', v);
  if ((v = choice_option("MagPeelMode", std_mg, v01, 2)) != INT_MIN)     add_setting('U', v);
  if ((v = choice_option("StandbyMode", standby, v01, 2)) != INT_MIN)    add_setting('V', v);
  if ((v = choice_option("BackCooling", cooling, v10, 2)) != INT_MIN)    add_setting('_', v);

  /* Retransfer: one grid cell sets temperature (row, shared by both sides)
   * and the speed of each side, so all three go together; an unset part
   * takes the grid's "0".  Speed column c is sent as 5-c, i.e. 2-speed. */
  t  = int_option("RetransferTemp", -2, 2);
  sf = int_option("RetransferSpeedFront", -3, 2);
  sb = int_option("RetransferSpeedBack", -3, 2);
  if (t != INT_MIN || sf != INT_MIN || sb != INT_MIN)
  {
    add_setting('L', (t == INT_MIN ? 0 : t) + 2);
    add_setting('M', 2 - (sf == INT_MIN ? 0 : sf));
    add_setting('N', 2 - (sb == INT_MIN ? 0 : sb));
  }

  if ((v = int_option("UVSpeedFront", 0, 5)) != INT_MIN)   add_setting(']', v);
  if ((v = int_option("UVSpeedBack", 0, 5)) != INT_MIN)    add_setting('^', v);

  /* Bend remedy: temperature -5..0 -> O 0..5, speed -2..+2 -> P 0..4;
   * the "Disable" radio sends O=10, P=4. */
  if (!strcasecmp(get_option("BendRemedyTemp", "Default"), "Disable"))
  {
    add_setting('O', 10);
    add_setting('P', 4);
  }
  else if ((t = int_option("BendRemedyTemp", -5, 0)) != INT_MIN)
  {
    v = int_option("BendRemedySpeed", -2, 2);
    add_setting('O', t + 5);
    add_setting('P', (v == INT_MIN ? 0 : v) + 2);
  }

  /* Encode page: only sent with magnetic encoding on (F2 also for IC). */
  if (mag_on)
  {
    if ((v = int_option("MagWriteRetry", 0, 3)) != INT_MIN)
      add_setting('(', v);
    if ((v = choice_option("MagCoercivity", coerc, v01, 2)) != INT_MIN)
      add_setting(strcasecmp(get_option("MagEncoderType", "ISO"), "JIS") ? '*' : ')', v);
  }
  if ((mag_on || ic_on) && get_bool("EncodeBeforePrint"))
    add_setting(0xF2, 1);

  if ((v = choice_option("LaminateMode", lamin, v01, 2)) != INT_MIN)
    add_setting(0xC8, v);

  if (settings_len)
  {
    fputs("DEBUG: Printer settings:", stderr);
    for (int i = 0; i < settings_len; i += 4)
      fprintf(stderr, " %c=%d", settings[i] >= 0x20 && settings[i] < 0x7f ? settings[i] : '?', settings[i + 3]);
    fputc('\n', stderr);
  }
}

/* Image position correction in mm (e.g. "-1.25"), limited to +-5 mm. */
static int
mm_to_dots(const char *val, const char *name)
{
  char   *end;
  double mm = strtod(val, &end);

  if (end == val || *end || mm < -5.0 || mm > 5.0)
  {
    fprintf(stderr, "WARNING: %s=%s is not a number of mm between -5 and 5; using 0.\n", name, val);
    return (0);
  }

  return ((int)(mm * XID_DPI / 25.4 + (mm < 0 ? -0.5 : 0.5)));
}

static int
parse_panels(const char *val)
{
  if (!strcasecmp(val, "YMC"))    return (XPANEL_YMC);
  if (!strcasecmp(val, "K"))      return (XPANEL_K);
  if (!strcasecmp(val, "YMCK"))   return (XPANEL_YMCK);
  if (!strcasecmp(val, "YMCUV"))  return (XPANEL_YMCUV);
  if (!strcasecmp(val, "KUV"))    return (XPANEL_KUV);
  if (!strcasecmp(val, "YMCKUV")) return (XPANEL_YMCKUV);

  fprintf(stderr, "WARNING: Unknown panel set \"%s\", using YMCK.\n", val);
  return (XPANEL_YMCK);
}

/*
 * Magnetic stripe data (docs/PROTOCOL.md §6).  Track contents come from job
 * options because CUPS raster carries no text; the same data is encoded on
 * every card of the job.  Records 0x40..0x47 hold the characters without
 * start/end sentinels, exactly as the Windows driver writes them.
 */

typedef struct
{
  uint32_t type;                 /* XREC_TRACK + 0..7 */
  char     data[128];
  size_t   len;
} mag_track_t;

static mag_track_t mag[4];
static int         num_mag;

static int
parse_track(const char *name, const char *value, const char *format, int track)
{
  mag_track_t *t = mag + num_mag;
  char        buf[256];
  size_t      len, max;
  int         lo, hi;
  char        start;

  snprintf(buf, sizeof(buf), "%s", value);
  len   = strlen(buf);
  start = track == 1 ? '%' : (track == 2 || track == 3) ? ';' : 0;

  /* Accept data with or without sentinels; the printer adds them. */
  if (start && len && buf[0] == start)
  {
    memmove(buf, buf + 1, len);
    len --;
  }
  if (start && len && buf[len - 1] == '?')
    buf[-- len] = '\0';

  if (track == 0)                                    /* JIS-II */
  {
    t->type = XREC_TRACK + 0;  max = 69;  lo = 0x20; hi = 0x7e;
  }
  else if (track == 1 && !strcasecmp(format, "7bit"))
  {
    t->type = XREC_TRACK + 4;  max = 69;  lo = 0x20; hi = 0x7e;
  }
  else if (track == 1)
  {
    /* ISO 6-bit; 77..79 characters use the long format like the driver. */
    t->type = len > 76 ? XREC_TRACK + 6 : XREC_TRACK + 1;
    max = 79;  lo = 0x20; hi = 0x5f;
  }
  else if (track == 2)
  {
    t->type = XREC_TRACK + 2;  max = 37;  lo = 0x30; hi = 0x3f;
  }
  else if (!strcasecmp(format, "6bit"))
  {
    t->type = XREC_TRACK + 7;  max = 79;  lo = 0x20; hi = 0x5f;
  }
  else if (!strcasecmp(format, "7bit"))
  {
    t->type = XREC_TRACK + 5;  max = 69;  lo = 0x20; hi = 0x7e;
  }
  else
  {
    t->type = XREC_TRACK + 3;  max = 104; lo = 0x30; hi = 0x3f;
  }

  if (len == 0)
  {
    fprintf(stderr, "ERROR: %s is empty.\n", name);
    return (-1);
  }

  if (len > max)
  {
    fprintf(stderr, "ERROR: %s has %zu characters, the maximum for this track format is %zu.\n",
            name, len, max);
    return (-1);
  }

  /* Never echo card data into the log, only the position of the problem. */
  for (size_t i = 0; i < len; i ++)
  {
    unsigned char c = (unsigned char)buf[i];

    if (c < lo || c > hi)
    {
      fprintf(stderr, "ERROR: %s: character %zu is not allowed on this track "
              "(allowed 0x%02X-0x%02X%s).\n", name, i + 1, lo, hi,
              hi == 0x5f ? ", upper case only" : hi == 0x3f ? ", digits and :;<=>" : "");
      return (-1);
    }
  }

  memcpy(t->data, buf, len);
  t->len = len;
  num_mag ++;

  fprintf(stderr, "DEBUG: %s: %zu characters, record 0x%02X.\n", name, len, t->type);
  return (0);
}

static int
parse_mag_options(void)
{
  static const struct { const char *name; const char *fmt; int track; } opts[] =
  {
    { "mag-jis2",   NULL,                  0 },
    { "mag-track1", "mag-track1-format",   1 },
    { "mag-track2", NULL,                  2 },
    { "mag-track3", "mag-track3-format",   3 }
  };

  for (size_t i = 0; i < sizeof(opts) / sizeof(opts[0]); i ++)
  {
    const char *val = cupsGetOption(opts[i].name, num_options, options);
    const char *fmt = opts[i].fmt ? cupsGetOption(opts[i].fmt, num_options, options) : NULL;

    if (fmt && strcasecmp(fmt, "iso") && strcasecmp(fmt, "6bit") && strcasecmp(fmt, "7bit"))
    {
      fprintf(stderr, "ERROR: %s must be iso, 6bit or 7bit.\n", opts[i].fmt);
      return (-1);
    }
    if (fmt && opts[i].track == 1 && !strcasecmp(fmt, "6bit"))
      fmt = "iso";

    if (val && parse_track(opts[i].name, val, fmt ? fmt : "iso", opts[i].track))
      return (-1);
  }

  return (0);
}

/*
 * Smart-card data (docs/PROTOCOL.md §7).  The printer never sees it: the backend
 * hands it to an external encoding program while the card is held at the
 * contact (record 0x20) or contactless (record 0x21) station.  The value is
 * passed through unchanged; a "hex:" prefix gives binary data.
 */

#define IC_MAX_DATA  4096

typedef struct
{
  uint32_t type;
  uint8_t  data[IC_MAX_DATA];
  size_t   len;
} ic_data_t;

static ic_data_t ic[2];
static int       num_ic;

static int
parse_ic_options(void)
{
  static const struct { const char *name; uint32_t type; } opts[] =
  {
    { "ic-contact",     0x20 },
    { "ic-contactless", 0x21 }
  };

  for (size_t i = 0; i < 2; i ++)
  {
    const char *val = cupsGetOption(opts[i].name, num_options, options);
    ic_data_t  *d   = ic + num_ic;

    if (!val)
      continue;

    d->type = opts[i].type;
    d->len  = 0;

    if (!strncasecmp(val, "hex:", 4))
    {
      for (const char *p = val + 4; *p; p ++)
      {
        int hi, lo;

        if (*p == ' ' || *p == ':')
          continue;
        if (!isxdigit((unsigned char)p[0]) || !isxdigit((unsigned char)p[1]) || d->len >= IC_MAX_DATA)
        {
          fprintf(stderr, "ERROR: %s: invalid or too long hex data.\n", opts[i].name);
          return (-1);
        }
        hi = isdigit((unsigned char)p[0]) ? p[0] - '0' : (tolower((unsigned char)p[0]) - 'a' + 10);
        lo = isdigit((unsigned char)p[1]) ? p[1] - '0' : (tolower((unsigned char)p[1]) - 'a' + 10);
        d->data[d->len ++] = (uint8_t)(hi << 4 | lo);
        p ++;
      }
    }
    else
    {
      d->len = strlen(val);
      if (d->len > IC_MAX_DATA)
      {
        fprintf(stderr, "ERROR: %s: more than %d bytes.\n", opts[i].name, IC_MAX_DATA);
        return (-1);
      }
      memcpy(d->data, val, d->len);
    }

    if (d->len == 0)
    {
      fprintf(stderr, "ERROR: %s is empty.\n", opts[i].name);
      return (-1);
    }

    fprintf(stderr, "DEBUG: %s: %zu bytes, record 0x%02X.\n", opts[i].name, d->len, d->type);
    num_ic ++;
  }

  return (0);
}

/*
 * Print areas (docs/PROTOCOL.md §8): job options area1..area8 =
 * "front|back:peeloff|noprint:X,Y,W,H" in dots (300 dpi) or with an "mm"
 * suffix, measured on the side as it is designed (before any rotation).
 */

typedef struct
{
  int side;            /* 0 = front, 1 = back */
  int type;            /* 1 = peel-off, 2 = no-print (driver values) */
  int x, y, w, h;
} view_area_t;

static view_area_t   areas[XID_MAX_AREAS];
static int           num_areas;
static int           peel_edge = 0xE1, peel_start = 0xA0, peel_end = 0xA0;   /* DM+0x229c.. defaults */

static int
parse_area_options(void)
{
  const char *val;

  for (int i = 1; i <= XID_MAX_AREAS; i ++)
  {
    char        name[16], sidestr[16], typestr[16], unit[8] = "";
    double      v[4];
    view_area_t *a = areas + num_areas;

    snprintf(name, sizeof(name), "area%d", i);
    if ((val = cupsGetOption(name, num_options, options)) == NULL)
      continue;

    if (sscanf(val, "%15[^:]:%15[^:]:%lf,%lf,%lf,%lf%7s", sidestr, typestr,
               v, v + 1, v + 2, v + 3, unit) < 6 ||
        (unit[0] && strcasecmp(unit, "mm")) ||
        (strcasecmp(sidestr, "front") && strcasecmp(sidestr, "back")) ||
        (strcasecmp(typestr, "peeloff") && strcasecmp(typestr, "noprint")))
    {
      fprintf(stderr, "ERROR: %s must look like front:peeloff:X,Y,W,H or back:noprint:X,Y,W,Hmm.\n", name);
      return (-1);
    }

    for (int j = 0; j < 4; j ++)
      if (unit[0])
        v[j] = v[j] * XID_DPI / 25.4 + 0.5;

    a->side = strcasecmp(sidestr, "front") ? 1 : 0;
    a->type = strcasecmp(typestr, "peeloff") ? 2 : 1;
    a->x    = (int)v[0];
    a->y    = (int)v[1];
    a->w    = (int)v[2];
    a->h    = (int)v[3];

    if (a->x < 0 || a->y < 0 || a->w <= 0 || a->h <= 0 ||
        a->x + a->w > XID_WIDTH || a->y + a->h > XID_WIDTH)
    {
      fprintf(stderr, "ERROR: %s is outside the card.\n", name);
      return (-1);
    }

    fprintf(stderr, "DEBUG: %s: %s %s %d,%d %dx%d dots\n", name, sidestr, typestr, a->x, a->y, a->w, a->h);
    num_areas ++;
  }

  if ((val = cupsGetOption("peeloff-levels", num_options, options)) != NULL &&
      (sscanf(val, "%d,%d,%d", &peel_edge, &peel_start, &peel_end) != 3 ||
       peel_edge < 0 || peel_edge > 255 || peel_start < 0 || peel_start > 255 ||
       peel_end < 0 || peel_end > 255))
  {
    fputs("ERROR: peeloff-levels must be EDGE,START,END (0-255 each).\n", stderr);
    return (-1);
  }

  return (0);
}

static int
side_has_peeloff(int side)
{
  for (int i = 0; i < num_areas; i ++)
    if (areas[i].side == side && areas[i].type == 1)
      return (1);
  return (0);
}

/*
 * Map the side's areas to plane coordinates: the driver applies them after
 * rotation (0x71907300), so follow the page through the same transforms.
 */

static int
plane_areas(int side, int portrait, int reversed, xid_area_t *out)
{
  int n = 0;

  for (int i = 0; i < num_areas; i ++)
  {
    view_area_t a = areas[i];
    int         x = a.x, y = a.y, w = a.w, h = a.h;

    if (a.side != side)
      continue;

    if (portrait)                          /* plane row = 663 - x, col = y */
    {
      x = a.y;
      y = XID_HEIGHT - a.x - a.w;
      w = a.h;
      h = a.w;
    }

    if (reversed)
    {
      x = XID_WIDTH - x - w;
      y = XID_HEIGHT - y - h;
    }

    out[n].enabled = 1;
    out[n].x       = x;
    out[n].y       = y;
    out[n].w       = w;
    out[n].h       = h;
    out[n].type    = a.type;
    out[n].side    = side + 1;
    n ++;
  }

  return (n);
}

static int panel_has_ymc(int p) { return (p == XPANEL_YMC || p == XPANEL_YMCK || p == XPANEL_YMCUV || p == XPANEL_YMCKUV); }
static int panel_has_k(int p)   { return (p == XPANEL_K || p == XPANEL_YMCK || p == XPANEL_KUV || p == XPANEL_YMCKUV); }
static int panel_has_uv(int p)  { return ((p & 0x10) != 0); }


/*
 * 'load_lut()' - map a LUT24 file (§1.7): 16-byte header "LUT24\0..." followed
 * by a Y-major CMY->CMY cube.  Returns a pointer to the cube or NULL.
 */

static const uint8_t *
load_lut(const char *path)
{
  int         fd;
  struct stat st;
  uint8_t     *map;

  if ((fd = open(path, O_RDONLY)) < 0)
  {
    fprintf(stderr, "WARNING: Unable to open LUT file \"%s\": %s - printing without colour correction "
            "(import it with xid8300-import).\n", path, strerror(errno));
    return (NULL);
  }

  if (fstat(fd, &st) || (st.st_size != (off_t)(LUT_HEADER_LEN + LUT_BODY_LEN) &&
                         st.st_size != (off_t)(LUT_HEADER_LEN + LUT_BODY_LEN + 0x100)))
  {
    fprintf(stderr, "WARNING: \"%s\" is not a LUT24 file (bad size) - printing without colour correction.\n", path);
    close(fd);
    return (NULL);
  }

  map = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_SHARED, fd, 0);
  close(fd);

  if (map == MAP_FAILED)
  {
    fprintf(stderr, "WARNING: Unable to map LUT file: %s\n", strerror(errno));
    return (NULL);
  }

  if (memcmp(map, "LUT24\0", 6))
  {
    fprintf(stderr, "WARNING: \"%s\" has no LUT24 signature - printing without colour correction.\n", path);
    munmap(map, (size_t)st.st_size);
    return (NULL);
  }

  fprintf(stderr, "DEBUG: Using LUT \"%s\" (%.10s).\n", path, (const char *)map + 6);
  return (map + LUT_HEADER_LEN);
}


/*
 * 'write_record()' - write one spool record (§1.2).
 */

static void
write_record(uint32_t type, const uint8_t *data, uint32_t len, uint32_t param)
{
  uint8_t hdr[XREC_HDR_LEN];

  memset(hdr, 0, sizeof(hdr));
  xid_put_le32(hdr, type);
  xid_put_le32(hdr + 4, len);
  xid_put_le32(hdr + 16, param);

  if (fwrite(hdr, 1, sizeof(hdr), stdout) != sizeof(hdr) ||
      (len && fwrite(data, 1, len, stdout) != len))
  {
    perror("ERROR: Unable to write print data");
    exit(1);
  }
}

static int
plane_used(const uint8_t *p)
{
  for (size_t i = 0; i < XID_PLANE; i ++)
    if (p[i])
      return (1);
  return (0);
}

static void
reverse_plane(uint8_t *p)
{
  for (size_t i = 0, j = XID_PLANE - 1; i < j; i ++, j --)
  {
    uint8_t t = p[i];
    p[i] = p[j];
    p[j] = t;
  }
}


/*
 * 'read_page()' - read one raster page into landscape R,G,B planes.
 *
 * The Windows driver forces the GDI page to the full 1036x664 print area
 * (DM+0xf1, §1.5), which is larger than the card (bleed), so applications
 * draw edge to edge.  A CUPS page usually has the document's own size, e.g.
 * a design made at the CR80 card size (54 x 85.6 mm = 638x1011 dots), and
 * placed as-is it leaves a white border that the printer's position tolerance
 * makes visible on one side.  So, like Windows, the page is scaled to the
 * print area: stretched when its shape is within 3% of the card's, otherwise
 * scaled to fit and centred.  CardFit=Actual keeps the old 1:1 placement.
 *
 * A portrait page is rotated exactly like the Windows driver:
 * dst[(664-1-x)*1036 + y] = src(x,y)  (§1.5).
 */

static int
read_page(cups_raster_t *ras, const cups_page_header2_t *h,
          uint8_t *r, uint8_t *g, uint8_t *b, int *portrait)
{
  int     gray, bpp;
  int     pw, ph, dw, dh, ox, oy, sw, sh;
  uint8_t *src;
  int     x, y;

  if (h->cupsBitsPerColor != 8 || h->cupsColorOrder != CUPS_ORDER_CHUNKED)
  {
    fputs("ERROR: Unsupported raster format (need 8 bits per colour, chunky).\n", stderr);
    return (-1);
  }

  if (h->cupsBitsPerPixel == 24 &&
      (h->cupsColorSpace == CUPS_CSPACE_RGB || h->cupsColorSpace == CUPS_CSPACE_SRGB ||
       h->cupsColorSpace == CUPS_CSPACE_ADOBERGB))
    gray = 0;
  else if (h->cupsBitsPerPixel == 8 &&
           (h->cupsColorSpace == CUPS_CSPACE_W || h->cupsColorSpace == CUPS_CSPACE_SW))
    gray = 1;
  else
  {
    fprintf(stderr, "ERROR: Unsupported colour space %d / %d bits per pixel.\n",
            h->cupsColorSpace, h->cupsBitsPerPixel);
    return (-1);
  }

  if (h->HWResolution[0] != XID_DPI || h->HWResolution[1] != XID_DPI)
    fprintf(stderr, "WARNING: Raster is %dx%d dpi, printer is %d dpi.\n",
            h->HWResolution[0], h->HWResolution[1], XID_DPI);

  bpp       = gray ? 1 : 3;
  sw        = (int)h->cupsWidth;
  sh        = (int)h->cupsHeight;
  *portrait = sh > sw;
  pw        = *portrait ? XID_HEIGHT : XID_WIDTH;
  ph        = *portrait ? XID_WIDTH : XID_HEIGHT;

  if (sw < 1 || sh < 1 || h->cupsBytesPerLine < (unsigned)(sw * bpp))
  {
    fputs("ERROR: Bad raster page size.\n", stderr);
    return (-1);
  }

  if (fit_actual || (sw == pw && sh == ph))
  {
    dw = sw;
    dh = sh;
  }
  else
  {
    double rx = (double)pw / sw, ry = (double)ph / sh;

    if (rx / ry <= 1.03 && ry / rx <= 1.03)
    {
      dw = pw;                                 /* card shape: fill, bleed included */
      dh = ph;
    }
    else
    {
      double s = rx < ry ? rx : ry;            /* other shape: fit and centre */

      dw = (int)(sw * s + 0.5);
      dh = (int)(sh * s + 0.5);
    }
  }

  ox = (pw - dw) / 2;
  oy = (ph - dh) / 2;

  if (sw != pw || sh != ph)
    fprintf(stderr, "DEBUG: Page is %dx%d, print area is %dx%d; placed as %dx%d at %d,%d.\n",
            sw, sh, pw, ph, dw, dh, ox, oy);

  memset(r, 255, XID_PLANE);
  memset(g, 255, XID_PLANE);
  memset(b, 255, XID_PLANE);

  if ((src = malloc((size_t)sh * h->cupsBytesPerLine)) == NULL)
  {
    fputs("ERROR: Out of memory.\n", stderr);
    return (-1);
  }

  for (y = 0; y < sh; y ++)
    if (cupsRasterReadPixels(ras, src + (size_t)y * h->cupsBytesPerLine, h->cupsBytesPerLine) !=
        h->cupsBytesPerLine)
    {
      fputs("ERROR: Short raster data.\n", stderr);
      free(src);
      return (-1);
    }

  for (y = 0; y < ph; y ++)
  {
    int    py = y - oy, y0 = 0, y1 = 0;
    double fy = 0.0;

    if (py < 0 || py >= dh)
      continue;

    if (dh != sh)                              /* bilinear source rows */
    {
      double sy = (py + 0.5) * sh / dh - 0.5;

      if (sy < 0.0)
        sy = 0.0;
      y0 = (int)sy;
      y1 = y0 + 1 < sh ? y0 + 1 : y0;
      fy = sy - y0;
    }
    else
      y0 = y1 = py;

    for (x = 0; x < pw; x ++)
    {
      int           px = x - ox, x0, x1;
      double        fx = 0.0;
      size_t        idx;
      const uint8_t *row0 = src + (size_t)y0 * h->cupsBytesPerLine,
                    *row1 = src + (size_t)y1 * h->cupsBytesPerLine;
      uint8_t       v[3];

      if (px < 0 || px >= dw)
        continue;

      if (dw != sw)
      {
        double sx = (px + 0.5) * sw / dw - 0.5;

        if (sx < 0.0)
          sx = 0.0;
        x0 = (int)sx;
        x1 = x0 + 1 < sw ? x0 + 1 : x0;
        fx = sx - x0;
      }
      else
        x0 = x1 = px;

      for (int c = 0; c < bpp; c ++)
      {
        double top = row0[x0 * bpp + c] + (row0[x1 * bpp + c] - row0[x0 * bpp + c]) * fx,
               bot = row1[x0 * bpp + c] + (row1[x1 * bpp + c] - row1[x0 * bpp + c]) * fx;

        v[c] = (uint8_t)(top + (bot - top) * fy + 0.5);
      }

      idx = *portrait ? (size_t)(XID_HEIGHT - 1 - x) * XID_WIDTH + (size_t)y
                      : (size_t)y * XID_WIDTH + (size_t)x;

      if (gray)
        r[idx] = g[idx] = b[idx] = v[0];
      else
      {
        r[idx] = v[0];
        g[idx] = v[1];
        b[idx] = v[2];
      }
    }
  }

  free(src);
  return (0);
}


/*
 * 'shift_plane()' - move a finished plane by (dx, dy) dots to compensate for
 * the printer's image position.  Colour planes repeat the pixels EDGE_INSET
 * dots in from the edge so the bleed stays filled (the outermost rows and
 * columns are often white from rounding in Ghostscript); the other planes are
 * padded with "no ink".
 */

#define EDGE_INSET 3

static void
shift_plane(uint8_t *p, uint8_t *tmp, int dx, int dy, int extend)
{
  if (!dx && !dy)
    return;

  for (int y = 0; y < XID_HEIGHT; y ++)
  {
    int sy = y - dy;

    if (sy < 0 || sy >= XID_HEIGHT)
    {
      if (!extend)
      {
        memset(tmp + (size_t)y * XID_WIDTH, 0, XID_WIDTH);
        continue;
      }
      sy = sy < 0 ? EDGE_INSET : XID_HEIGHT - 1 - EDGE_INSET;
    }

    for (int x = 0; x < XID_WIDTH; x ++)
    {
      int sx = x - dx;

      if (sx < 0 || sx >= XID_WIDTH)
      {
        if (!extend)
        {
          tmp[(size_t)y * XID_WIDTH + x] = 0;
          continue;
        }
        sx = sx < 0 ? EDGE_INSET : XID_WIDTH - 1 - EDGE_INSET;
      }
      tmp[(size_t)y * XID_WIDTH + x] = p[(size_t)sy * XID_WIDTH + sx];
    }
  }

  memcpy(p, tmp, XID_PLANE);
}


/*
 * 'parse_color_adjust()' - Brightness, Contrast, Hue*, Gamma* options.
 * Brightness/contrast/hue -100..100, gamma 1.0..10.0 (stored in tenths).
 */

static void
parse_color_adjust(void)
{
  static const char *const hue[3] = { "HueRed", "HueGreen", "HueBlue" };
  static const char *const gam[3] = { "GammaRed", "GammaGreen", "GammaBlue" };
  int v;

  adj_bri = (v = int_option("Brightness", -100, 100)) == INT_MIN ? 0 : v;
  adj_con = (v = int_option("Contrast", -100, 100)) == INT_MIN ? 0 : v;
  adj_on  = adj_bri || adj_con;

  for (int c = 0; c < 3; c ++)
  {
    const char *val = get_option(gam[c], "1.0");
    char       *end;
    double     g    = strtod(val, &end);

    adj_hue[c] = (v = int_option(hue[c], -100, 100)) == INT_MIN ? 0 : v;

    if (!strcasecmp(val, "Default"))
      g = 1.0;
    else if (end == val || *end || g < 1.0 || g > 10.0)
    {
      fprintf(stderr, "WARNING: %s=%s is not between 1.0 and 10.0; using 1.0.\n", gam[c], val);
      g = 1.0;
    }
    adj_gamma[c] = (int)(g * 10.0 + 0.5);
    adj_on      |= adj_hue[c] || adj_gamma[c] != 10;
  }

  if (adj_on)
    fprintf(stderr, "DEBUG: Colour adjustment: brightness %d contrast %d hue %d/%d/%d gamma %d/%d/%d (tenths)\n",
            adj_bri, adj_con, adj_hue[0], adj_hue[1], adj_hue[2],
            adj_gamma[0], adj_gamma[1], adj_gamma[2]);
}

static int
clampi(int v, int lo, int hi)
{
  return (v < lo ? lo : v > hi ? hi : v);
}

/*
 * 'color_adjust()' - the Windows driver's colour adjustment (GD 0x71905280),
 * applied to RGB before black extraction and the RGB->CMY step.  Pure white
 * and pure black pixels are left alone.
 */

static void
color_adjust(uint8_t *r, uint8_t *g, uint8_t *b)
{
  uint8_t  con[256], gam[3][256], up[3][256], dn[3][256];
  uint8_t  *p[3] = { r, g, b };
  int      bri = adj_bri ? clampi(adj_bri, -100, 100) * 255 / 100 : 0;

  for (int i = 0; i < 256; i ++)
  {
    if (adj_con <= -100)
      con[i] = i < 128 ? 0x7f : 0x80;
    else if (adj_con >= 100)
      con[i] = i < 128 ? 0 : 255;
    else
      con[i] = (uint8_t)clampi((int)((i - 127.5) * tan((adj_con * 0.45 + 45.0) * M_PI / 180.0) + 127.5), 0, 255);

    for (int c = 0; c < 3; c ++)
    {
      int d = clampi(i * adj_hue[c] / 100, -255, 255);

      gam[c][i] = i == 0 ? 0 : (uint8_t)clampi((int)(pow(i / 255.0, 10.0 / adj_gamma[c]) * 255.0 + 0.5), 0, 255);
      up[c][i]  = (uint8_t)clampi(i + d, 0, 255);
      dn[c][i]  = (uint8_t)clampi(i - d / 2, 0, 255);
    }
  }

  for (size_t i = 0; i < XID_PLANE; i ++)
  {
    int v[3] = { r[i], g[i], b[i] };

    if ((v[0] == 255 && v[1] == 255 && v[2] == 255) || (v[0] == 0 && v[1] == 0 && v[2] == 0))
      continue;

    for (int c = 0; c < 3; c ++)
    {
      if (adj_bri)
        v[c] = clampi(v[c] + bri, 0, 255);
      if (adj_con)
        v[c] = con[v[c]];
      if (adj_gamma[c] != 10)
        v[c] = gam[c][v[c]];
    }

    /* Hue: raise one channel, lower the other two by half as much. */
    for (int h = 0; h < 3; h ++)
      if (adj_hue[h])
        for (int c = 0; c < 3; c ++)
          v[c] = c == h ? up[h][v[c]] : dn[h][v[c]];

    for (int c = 0; c < 3; c ++)
      p[c][i] = (uint8_t)v[c];
  }
}


/*
 * 'mono_k()' - K-only sides: luminance (30R+59G+11B)/100 -> binary K.
 */

static void
mono_k(const uint8_t *r, const uint8_t *g, const uint8_t *b, uint8_t *k)
{
  if (!mono_dither)
  {
    for (size_t i = 0; i < XID_PLANE; i ++)
      k[i] = ((30 * r[i] + 59 * g[i] + 11 * b[i]) / 100 < 128) ? 255 : 0;
    return;
  }

  /* Floyd-Steinberg on luminance. */
  int *cur = calloc(XID_WIDTH + 2, sizeof(int));
  int *nxt = calloc(XID_WIDTH + 2, sizeof(int));

  if (!cur || !nxt)
  {
    fputs("ERROR: Out of memory.\n", stderr);
    exit(1);
  }

  for (int y = 0; y < XID_HEIGHT; y ++)
  {
    memset(nxt, 0, (XID_WIDTH + 2) * sizeof(int));

    for (int x = 0; x < XID_WIDTH; x ++)
    {
      size_t i   = (size_t)y * XID_WIDTH + (size_t)x;
      int    v   = (30 * r[i] + 59 * g[i] + 11 * b[i]) / 100 + cur[x + 1] / 16;
      int    out = v < 128 ? 0 : 255;
      int    err = v - out;

      k[i] = out ? 0 : 255;
      cur[x + 2] += err * 7;
      nxt[x]     += err * 3;
      nxt[x + 1] += err * 5;
      nxt[x + 2] += err;
    }

    int *t = cur;
    cur = nxt;
    nxt = t;
  }

  free(cur);
  free(nxt);
}


/*
 * 'make_color_layer()' - colour/K page -> Y, M, C, K planes (§1.6):
 * black extraction, RGB->CMY by inversion, then the LUT.
 */

static void
make_color_layer(int panels, uint8_t *r, uint8_t *g, uint8_t *b, side_planes_t *sp)
{
  int has_ymc = panel_has_ymc(panels), has_k = panel_has_k(panels);

  memset(sp->k, 0, XID_PLANE);

  if (has_k && !has_ymc)
    mono_k(r, g, b, sp->k);
  else if (has_k && black_mode != BLACK_COMPOSITE)
  {
    for (size_t i = 0; i < XID_PLANE; i ++)
    {
      if (r[i] <= black_threshold && g[i] <= black_threshold && b[i] <= black_threshold)
      {
        sp->k[i] = 255;
        if (black_mode == BLACK_KONLY)
          r[i] = g[i] = b[i] = 255;
      }
    }
  }

  if (!has_ymc)
  {
    memset(sp->y, 0, XID_PLANE);
    memset(sp->m, 0, XID_PLANE);
    memset(sp->c, 0, XID_PLANE);
    return;
  }

  for (size_t i = 0; i < XID_PLANE; i ++)
  {
    uint8_t yv = (uint8_t)(255 - b[i]);
    uint8_t mv = (uint8_t)(255 - g[i]);
    uint8_t cv = (uint8_t)(255 - r[i]);

    if (lut)
    {
      const uint8_t *e = lut + 3 * (((size_t)yv << 16) | ((size_t)mv << 8) | cv);

      yv = e[0];
      mv = e[1];
      cv = e[2];
    }

    sp->y[i] = yv;
    sp->m[i] = mv;
    sp->c[i] = cv;
  }
}

/* UV page: any non-white pixel gets UV, like driver UV source 0x11. */
static void
make_uv_layer(const uint8_t *r, const uint8_t *g, const uint8_t *b, uint8_t *uv)
{
  for (size_t i = 0; i < XID_PLANE; i ++)
    uv[i] = (r[i] != 255 || g[i] != 255 || b[i] != 255) ? 255 : 0;
}


/*
 * 'emit_side()' - write the planes of one card side followed by 0xF3.
 */

static void
emit_side(side_planes_t *sp, int panels, int have_uv, int side)
{
  int        front = side == 0;
  xid_area_t pa[XID_MAX_AREAS];
  int        npa = plane_areas(side, sp->portrait, sp->reversed, pa);
  int        p86_used = 0;

  /* The driver writes the settings record on every page, before the planes. */
  if (settings_len)
    write_record(XREC_SETTINGS, settings, (uint32_t)settings_len, 0);

  /* Track data rides with the front side, before its planes (§6.3). */
  if (front)
  {
    for (int i = 0; i < num_ic; i ++)
      write_record(ic[i].type, ic[i].data, (uint32_t)ic[i].len, 0);
    for (int i = 0; i < num_mag; i ++)
      write_record(mag[i].type, (const uint8_t *)mag[i].data, (uint32_t)mag[i].len, 0);
  }

  if (sp->have_color)
  {
    if (sp->reversed)
    {
      reverse_plane(sp->y);
      reverse_plane(sp->m);
      reverse_plane(sp->c);
      reverse_plane(sp->k);
    }

    /* Areas are cut out after rotation. */
    if (npa)
    {
      xid_clear_areas(sp->y, pa, npa, side + 1);
      xid_clear_areas(sp->m, pa, npa, side + 1);
      xid_clear_areas(sp->c, pa, npa, side + 1);
      xid_clear_areas(sp->k, pa, npa, side + 1);
    }

    /* The 0x86 plane exists only on sides with a K panel (iVar5 in 0x71907300). */
    if (npa && panel_has_k(panels))
    {
      memset(sp->p86, 0, XID_PLANE);
      xid_draw_peeloff(sp->p86, pa, npa, side + 1, (uint8_t)peel_edge, peel_start, peel_end);
      p86_used = plane_used(sp->p86);
    }

    /* Position correction last, so every plane (and its areas) moves together. */
    shift_plane(sp->y, sp->scratch, shift_x, shift_y, 1);
    shift_plane(sp->m, sp->scratch, shift_x, shift_y, 1);
    shift_plane(sp->c, sp->scratch, shift_x, shift_y, 1);
    shift_plane(sp->k, sp->scratch, shift_x, shift_y, 0);
    if (p86_used)
      shift_plane(sp->p86, sp->scratch, shift_x, shift_y, 0);

    if (panel_has_ymc(panels) &&
        (plane_used(sp->y) || plane_used(sp->m) || plane_used(sp->c)))
    {
      write_record(XREC_Y, sp->y, XID_PLANE, 0);
      write_record(XREC_M, sp->m, XID_PLANE, 0);
      write_record(XREC_C, sp->c, XID_PLANE, 0);
    }

    if (panel_has_k(panels) && plane_used(sp->k))
      write_record(XREC_K, sp->k, XID_PLANE, 0);

  }

  if (have_uv)
  {
    if (sp->reversed)
      reverse_plane(sp->uv);
    if (npa)
      xid_clear_areas(sp->uv, pa, npa, side + 1);
    shift_plane(sp->uv, sp->scratch, shift_x, shift_y, 0);

    /* param = (position << 8) | panels: "MAC address position" 1 upper right /
     * 2 lower left, swapped when the side is rotated (GD 0x7190881a); film
     * panels 0 auto / 1 / 2 (DM+0x4778; driver default -> 2). */
    if (plane_used(sp->uv))
      write_record(XREC_UV, sp->uv, XID_PLANE,
                   ((uint32_t)(sp->reversed ? 3 - uv_mark : uv_mark) << 8) | (uint32_t)uv_panels);
  }

  if (p86_used)
    write_record(XREC_P86, sp->p86, XID_PLANE, 0);

  write_record(XREC_EOS, NULL, 0, 0);
  sp->have_color = 0;
}


int
main(int argc, char *argv[])
{
  int                 fd = 0;
  cups_raster_t       *ras;
  cups_page_header2_t header;
  side_cfg_t          cfg[2];
  side_planes_t       sp;
  uint8_t             *r, *g, *b;
  int                 page = 0, side = 0, awaiting_uv = 0, duplex = 0, tumble = 0;
  int                 started = 0;
  const char          *val;

  setbuf(stderr, NULL);

  if (argc < 6 || argc > 7)
  {
    fputs("Usage: rastertoxid8300 job user title copies options [file]\n", stderr);
    return (1);
  }

  if (argc == 7 && (fd = open(argv[6], O_RDONLY)) < 0)
  {
    perror("ERROR: Unable to open raster file");
    return (1);
  }

  num_options = cupsParseOptions(argv[5], 0, &options);

  if ((val = getenv("PPD")) != NULL && (ppd = ppdOpenFile(val)) != NULL)
  {
    ppdMarkDefaults(ppd);
    cupsMarkOptions(ppd, num_options, options);
  }

  cfg[0].panels    = parse_panels(get_option("FrontPanels", "YMCK"));
  cfg[1].panels    = parse_panels(get_option("BackPanels", "YMCK"));
  cfg[0].rotate180 = get_bool("FrontRotate180");
  cfg[1].rotate180 = get_bool("BackRotate180");

  val = get_option("BlackMode", "KPlusColor");
  if (!strcasecmp(val, "KPlusColor"))
    black_mode = BLACK_KPLUSCOLOR;
  else if (!strcasecmp(val, "Composite"))
    black_mode = BLACK_COMPOSITE;

  black_threshold = atoi(get_option("BlackThreshold", "0"));
  if (black_threshold < 0)
    black_threshold = 0;
  else if (black_threshold > 64)
    black_threshold = 64;

  mono_dither = !strcasecmp(get_option("MonoMethod", "Threshold"), "Dither");

  parse_color_adjust();
  uv_mark   = !strcasecmp(get_option("UVMarkPosition", "UpperRight"), "LowerLeft") ? 2 : 1;
  val       = get_option("UVFilmPanels", "Two");
  uv_panels = !strcasecmp(val, "Auto") ? 0 : !strcasecmp(val, "One") ? 1 : 2;

  fit_actual = !strcasecmp(get_option("CardFit", "Fill"), "Actual");
  shift_x    = mm_to_dots(get_option("ImageShiftX", "0"), "ImageShiftX");
  shift_y    = mm_to_dots(get_option("ImageShiftY", "0"), "ImageShiftY");
  if (shift_x || shift_y)
    fprintf(stderr, "DEBUG: Image position correction %d,%d dots.\n", shift_x, shift_y);

  if (parse_mag_options() || parse_ic_options() || parse_area_options())
    return (1);

  parse_printer_settings(num_mag > 0, num_ic > 0);

  for (int i = 0; i < 2; i ++)
  {
    const char *name = i ? "Back" : "Front";

    /* The monitor rejects these combinations with "Ink ribbon specification
     * is incorrect" (0x50406aac), so stop early with a clearer message. */
    if (side_has_peeloff(i) && panel_has_k(cfg[i].panels) && panel_has_uv(cfg[i].panels))
    {
      fprintf(stderr, "ERROR: %s side: peel-off areas cannot be combined with a UV panel.\n", name);
      return (1);
    }
  }

  if (!strcasecmp(get_option("ColorCorrection", "None"), "LUT"))
  {
    ppd_attr_t *attr = ppd ? ppdFindAttr(ppd, "xidLUTFile", NULL) : NULL;

    lut = load_lut(attr && attr->value ? attr->value
                                       : "/var/lib/printer-driver-xid8300/LUT24.LUT");
  }

  r      = malloc(XID_PLANE);
  g      = malloc(XID_PLANE);
  b      = malloc(XID_PLANE);
  sp.y   = malloc(XID_PLANE);
  sp.m   = malloc(XID_PLANE);
  sp.c   = malloc(XID_PLANE);
  sp.k   = malloc(XID_PLANE);
  sp.uv  = calloc(1, XID_PLANE);
  sp.p86     = malloc(XID_PLANE);
  sp.scratch = malloc(XID_PLANE);
  sp.have_color = 0;
  sp.reversed   = 0;
  sp.portrait   = 0;

  if (!r || !g || !b || !sp.y || !sp.m || !sp.c || !sp.k || !sp.uv || !sp.p86 || !sp.scratch)
  {
    fputs("ERROR: Out of memory.\n", stderr);
    return (1);
  }

  ras = cupsRasterOpen(fd, CUPS_RASTER_READ);

  while (cupsRasterReadHeader2(ras, &header))
  {
    int portrait, panels;

    page ++;

    if (!started)
    {
      uint8_t job[20];

      /* The raster header carries duplex for PDF/PostScript jobs; raster
       * submitted directly may only have the job option. */
      val    = get_option("Duplex", "None");
      duplex = header.Duplex != 0 || !strcasecmp(val, "DuplexNoTumble") || !strcasecmp(val, "DuplexTumble");
      tumble = duplex && (header.Tumble != 0 || !strcasecmp(val, "DuplexTumble"));

      /* Job header (§1.3).  Copies are produced by CUPS (cupsManualCopies). */
      memset(job, 0, sizeof(job));
      xid_put_le32(job, 1);
      job[4] = duplex ? XJOB_DUPLEX_DUPLEX : XJOB_DUPLEX_SIMPLEX;
      /* DM+0xee: magnetic encoding on; bit0 toggles the card flag after
       * encoding (MagCardFlip, physical meaning unconfirmed). */
      if (num_mag)
        job[6] = get_bool("MagCardFlip") ? 0x11 : 0x10;
      /* DM+0xef: 0x10 contact, 0x20 contactless, bit0 toggles the card flag. */
      for (int i = 0; i < num_ic; i ++)
        job[7] |= ic[i].type == 0x20 ? 0x10 : 0x20;
      if (job[7] && get_bool("ICCardFlip"))
        job[7] |= 0x01;
      /* Setup page "turns over the card after loading" (DM+0xed bit1) and
       * Print page "print the back side first" (DM+0xed bit4). */
      xid_put_le32(job + 8, (get_bool("TurnOverAfterLoad") ? XJOB_FLAG_LOADFLAG : 0u) |
                            (duplex && get_bool("BackSideFirst") ? XJOB_FLAG_BACKFIRST : 0u));
      write_record(XREC_JOB, job, sizeof(job), 0);
      started = 1;

      fprintf(stderr, "DEBUG: duplex=%d tumble=%d front=0x%02x back=0x%02x black=%d lut=%s\n",
              duplex, tumble, cfg[0].panels, cfg[1].panels, black_mode, lut ? "yes" : "no");
    }

    fprintf(stderr, "INFO: Processing page %d...\n", page);

    if (read_page(ras, &header, r, g, b, &portrait))
      return (1);

    panels = cfg[side].panels;

    if (!awaiting_uv)
    {
      /* Driver rotation rule (0x719083eb): the back side is turned 180
       * degrees by default for landscape cards; the user option inverts it. */
      if (side == 0)
        sp.reversed = cfg[0].rotate180;
      else
        sp.reversed = (portrait ? cfg[1].rotate180 : !cfg[1].rotate180) ^ tumble;

      if (adj_on)
        color_adjust(r, g, b);
      make_color_layer(panels, r, g, b, &sp);
      sp.have_color = 1;
      sp.portrait   = portrait;

      if (panel_has_uv(panels))
      {
        awaiting_uv = 1;
        fprintf(stderr, "DEBUG: Page %d is the colour layer, next page is the UV layer.\n", page);
      }
      else
        emit_side(&sp, panels, 0, side);
    }
    else
    {
      make_uv_layer(r, g, b, sp.uv);
      emit_side(&sp, panels, 1, side);
      awaiting_uv = 0;
    }

    if (!awaiting_uv)
      side = (duplex && side == 0) ? 1 : 0;

    fprintf(stderr, "PAGE: %d 1\n", page);
  }

  if (!started)
  {
    fputs("ERROR: No pages found.\n", stderr);
    return (1);
  }

  if (awaiting_uv)
  {
    fputs("WARNING: Document ended before the UV layer page; side printed without UV.\n", stderr);
    emit_side(&sp, cfg[side].panels, 0, side);
    side = (duplex && side == 0) ? 1 : 0;
  }

  if (duplex && side == 1)
  {
    /* Odd number of sides: send an empty back side; the backend prints it
     * with a blank K plane exactly as the Windows port monitor does. */
    write_record(XREC_EOS, NULL, 0, 0);
  }

  write_record(XREC_EOJ, NULL, 0, 0);
  fflush(stdout);

  cupsRasterClose(ras);
  if (ppd)
    ppdClose(ppd);
  cupsFreeOptions(num_options, options);

  return (page ? 0 : 1);
}
