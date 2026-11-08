/*
 * Shared definitions for the Matica XID8300 CUPS filter and backend.
 *
 * Everything here comes from static analysis of the Windows driver
 * (see docs/PROTOCOL.md); section numbers refer to that file.
 */

#ifndef XID8300_H
#define XID8300_H

#include <stddef.h>
#include <stdint.h>

/* Plane geometry (§1.5): landscape, row-major, 8 bits per pixel. */
#define XID_WIDTH     1036
#define XID_HEIGHT    664
#define XID_PLANE     (XID_WIDTH * XID_HEIGHT)      /* 687,904 bytes */
#define XID_DPI       300

#define XID_TCP_PORT  50710                         /* §3.1, 0xC616 */

/*
 * Spool records written by the filter and read by the backend (§1.2).
 * Header is 20 bytes, little-endian: type, length, 0, 0, param.
 */
#define XREC_HDR_LEN   20

#define XREC_JOB       0x01   /* 20-byte job header (§1.3) */
#define XREC_TRACK     0x40   /* 0x40..0x47 magnetic stripe tracks */
#define XREC_Y         0x81
#define XREC_M         0x82
#define XREC_C         0x83
#define XREC_K         0x84
#define XREC_P86       0x86   /* area-fill plane, purpose unknown */
#define XREC_UV        0x87
#define XREC_KSECURE   0x88
#define XREC_SETTINGS  0xC0   /* {id,02,01,value} items (§1.4) */
#define XREC_ERROR     0xD0   /* uint32 code + WCHAR path[260] */
#define XREC_EOS       0xF3   /* end of card side */
#define XREC_EOJ       0xF5   /* end of job */

/* Job header payload (§1.3). */
#define XJOB_DUPLEX_SIMPLEX  0x10
#define XJOB_DUPLEX_DUPLEX   0x20
#define XJOB_FLAG_LOADFLAG   0x00000001   /* DM+0xed bit1 -> LoadCard flag */
#define XJOB_FLAG_BACKFIRST  0x00001000   /* DM+0xed bit4 -> back side first */

/* Ribbon panel choices per side (driver values, §1.6). */
#define XPANEL_YMC     0x01
#define XPANEL_K       0x02
#define XPANEL_YMCK    0x03
#define XPANEL_YMCUV   0x11
#define XPANEL_KUV     0x12
#define XPANEL_YMCKUV  0x13

static inline void xid_put_le32(uint8_t *p, uint32_t v)
{
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

static inline uint32_t xid_get_le32(const uint8_t *p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static inline void xid_put_be32(uint8_t *p, uint32_t v)
{
  p[0] = (uint8_t)(v >> 24);
  p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8);
  p[3] = (uint8_t)v;
}

static inline uint32_t xid_get_be32(const uint8_t *p)
{
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static inline uint16_t xid_get_be16(const uint8_t *p)
{
  return (uint16_t)(((unsigned)p[0] << 8) | p[1]);
}

#endif /* XID8300_H */
