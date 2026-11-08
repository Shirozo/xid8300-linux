/*
 * Print areas and the peel-off plane (record 0x86), implemented from the
 * format description in docs/PROTOCOL.md section 8.2.
 *
 * An area is a rectangle in plane coordinates (after rotation):
 *   type 2 (no-print)  - the exact rectangle is cleared in Y/M/C/K/UV;
 *   other types        - the rectangle plus 4 dots left/right and 6 dots
 *                        above/below (clipped to the card) is cleared;
 *   type 1 (peel-off)  - additionally filled in the 0x86 plane: the
 *                        rectangle moved up 8 dots, its first 5 rows at the
 *                        edge level, the rest a falling ramp (integer
 *                        division, never below the end level).
 * side is 1 for the front and 2 for the back.
 */

#ifndef XID8300_AREAS_H
#define XID8300_AREAS_H

#include "xid8300.h"

typedef struct
{
  int enabled, x, y, w, h, type, side;
} xid_area_t;

#define XID_MAX_AREAS 8

/*
 * Set rows [y0,y1) x columns [x0,x1) to v.  Positions are linear offsets
 * row * width + column, so a rectangle reaching past the right edge
 * continues on the next row, as the printer format expects; offsets outside
 * the plane are skipped.
 */

static void
xid_area_fill(uint8_t *plane, int x0, int x1, int y0, int y1, uint8_t v)
{
  for (long row = y0; row < y1; row ++)
    for (long col = x0; col < x1; col ++)
    {
      long pos = row * XID_WIDTH + col;

      if (pos >= 0 && pos < (long)XID_PLANE)
        plane[pos] = v;
    }
}

static int
xid_min(int a, int b)
{
  return (a < b ? a : b);
}

/* Clear the areas of one side in a colour, K or UV plane. */
static void
xid_clear_areas(uint8_t *plane, const xid_area_t *areas, int n, int side)
{
  for (const xid_area_t *a = areas; a < areas + n; a ++)
  {
    int left, top;

    if (a->enabled != 1 || a->side != side)
      continue;

    if (a->type == 2)
    {
      xid_area_fill(plane, a->x, a->x + a->w, a->y, a->y + a->h, 0);
      continue;
    }

    left = a->x > 4 ? a->x - 4 : 0;
    top  = a->y > 6 ? a->y - 6 : 0;
    xid_area_fill(plane, left, xid_min(left + a->w + 8, XID_WIDTH),
                  top, xid_min(top + a->h + 12, XID_HEIGHT), 0);
  }
}

/* Draw the peel-off areas of one side into the 0x86 plane. */
static void
xid_draw_peeloff(uint8_t *plane, const xid_area_t *areas, int n, int side,
                 uint8_t edge, int start, int end)
{
  for (const xid_area_t *a = areas; a < areas + n; a ++)
  {
    int top, right, bottom, ramp_rows;

    if (a->enabled != 1 || a->side != side || a->type != 1)
      continue;

    top       = a->y > 8 ? a->y - 8 : 0;
    right     = xid_min(a->x + a->w, XID_WIDTH);
    bottom    = xid_min(top + a->h, XID_HEIGHT);
    ramp_rows = bottom - top - 5;

    /* The 5 edge rows are drawn even when the area is shorter than that. */
    xid_area_fill(plane, a->x, right, top, top + 5, edge);

    for (int i = 0; i < ramp_rows; i ++)
    {
      int level = start - (start - end) * i / ramp_rows;

      xid_area_fill(plane, a->x, right, top + 5 + i, top + 6 + i,
                    (uint8_t)(level < end ? end : level));
    }
  }
}

#endif /* XID8300_AREAS_H */
