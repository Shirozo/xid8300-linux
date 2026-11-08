/*
 * areacheck - apply the print-area routines to a raw 1036x664 plane.
 *
 *   areacheck PEELOFF SIDE EDGE START END AREAS in out
 *
 * PEELOFF 1 draws the 0x86 peel-off plane, 0 clears the areas as in the
 * colour/K/UV planes.  AREAS is a text file with one
 * "enabled x y w h type side" line per area.
 */

#include "../src/xid8300_areas.h"

#include <stdio.h>
#include <stdlib.h>

static uint8_t *
load(const char *path)
{
  FILE    *f = fopen(path, "rb");
  uint8_t *p = malloc(XID_PLANE);

  if (!f || !p || fread(p, 1, XID_PLANE, f) != XID_PLANE)
  {
    perror(path);
    exit(1);
  }
  fclose(f);
  return (p);
}

int
main(int argc, char *argv[])
{
  xid_area_t a[XID_MAX_AREAS];
  int        n = 0;
  FILE       *f;
  uint8_t    *p;

  if (argc != 9)
  {
    fputs("Usage: areacheck PEELOFF SIDE EDGE START END AREAS in out\n", stderr);
    return (1);
  }

  if ((f = fopen(argv[6], "r")) == NULL)
  {
    perror(argv[6]);
    return (1);
  }
  while (n < XID_MAX_AREAS &&
         fscanf(f, "%d %d %d %d %d %d %d", &a[n].enabled, &a[n].x, &a[n].y, &a[n].w,
                &a[n].h, &a[n].type, &a[n].side) == 7)
    n ++;
  fclose(f);

  p = load(argv[7]);
  if (atoi(argv[1]))
    xid_draw_peeloff(p, a, n, atoi(argv[2]), (uint8_t)atoi(argv[3]), atoi(argv[4]), atoi(argv[5]));
  else
    xid_clear_areas(p, a, n, atoi(argv[2]));

  if ((f = fopen(argv[8], "wb")) == NULL || fwrite(p, 1, XID_PLANE, f) != XID_PLANE)
  {
    perror(argv[8]);
    return (1);
  }
  fclose(f);
  return (0);
}
