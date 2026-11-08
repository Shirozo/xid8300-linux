/*
 * mkraster - write a test CUPS raster (8-bit RGB, 300 dpi).
 *
 *   mkraster [-d] out.ras PAGES
 *
 * PAGES is a string of page kinds:
 *   L  landscape 1036x664 test card     P  portrait 664x1036 test card
 *   U  landscape UV layer (black frame + diagonal)
 *   T  portrait design at the CR80 trim size (638x1011): green, black 40x40
 *      marker at the top-left (tests scaling to the print area)
 * -d sets Duplex in every page header.
 *
 * Test card: black 40x40 corner marker at the page's top-left, a pure-black
 * text bar, an RGB gradient band and a red/green/blue swatch row.
 */

#include <cups/raster.h>

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void
pixel(unsigned char *p, int x, int y, int w, int h, char kind)
{
  unsigned char r = 255, g = 255, b = 255;

  if (kind == 'T')
  {
    if (x < 40 && y < 40)
      r = g = b = 0;
    else
    {
      r = 0;
      g = 128;
      b = 0;
    }
  }
  else if (kind == 'U')
  {
    if (x < 20 || y < 20 || x >= w - 20 || y >= h - 20 || abs(x * h / w - y) < 6)
      r = g = b = 0;
  }
  else if (x < 40 && y < 40)
    r = g = b = 0;                                   /* orientation marker */
  else if (y >= 60 && y < 110 && x >= 60 && x < w / 2)
    r = g = b = 0;                                   /* "text" bar */
  else if (y >= h / 3 && y < h / 2)
  {
    r = (unsigned char)(255 * x / (w - 1));          /* gradient band */
    g = (unsigned char)(255 - 255 * x / (w - 1));
    b = 128;
  }
  else if (y >= 2 * h / 3 && y < 2 * h / 3 + 80 && x >= 60 && x < 360)
  {
    int s = (x - 60) / 100;                          /* R, G, B swatches */
    r = s == 0 ? 255 : 0;
    g = s == 1 ? 255 : 0;
    b = s == 2 ? 255 : 0;
  }

  p[0] = r;
  p[1] = g;
  p[2] = b;
}

int
main(int argc, char *argv[])
{
  int                 duplex = 0, fd;
  cups_raster_t       *ras;
  cups_page_header2_t h;
  const char          *pages;

  if (argc > 1 && !strcmp(argv[1], "-d"))
  {
    duplex = 1;
    argv ++;
    argc --;
  }

  if (argc != 3)
  {
    fputs("Usage: mkraster [-d] out.ras PAGES\n", stderr);
    return (1);
  }

  if ((fd = open(argv[1], O_WRONLY | O_CREAT | O_TRUNC, 0644)) < 0)
  {
    perror(argv[1]);
    return (1);
  }

  ras   = cupsRasterOpen(fd, CUPS_RASTER_WRITE);
  pages = argv[2];

  for (; *pages; pages ++)
  {
    char          kind = *pages;
    int           w = kind == 'P' ? 664 : kind == 'T' ? 638 : 1036,
                  hgt = kind == 'P' ? 1036 : kind == 'T' ? 1011 : 664;
    unsigned char *line;

    memset(&h, 0, sizeof(h));
    h.HWResolution[0]  = h.HWResolution[1] = 300;
    h.PageSize[0]      = (unsigned)(w * 72 / 300);
    h.PageSize[1]      = (unsigned)(hgt * 72 / 300);
    h.cupsWidth        = (unsigned)w;
    h.cupsHeight       = (unsigned)hgt;
    h.cupsBitsPerColor = 8;
    h.cupsBitsPerPixel = 24;
    h.cupsBytesPerLine = (unsigned)w * 3;
    h.cupsColorOrder   = CUPS_ORDER_CHUNKED;
    h.cupsColorSpace   = CUPS_CSPACE_RGB;
    h.Duplex           = duplex;
    h.NumCopies        = 1;

    cupsRasterWriteHeader2(ras, &h);
    line = malloc(h.cupsBytesPerLine);

    for (int y = 0; y < hgt; y ++)
    {
      for (int x = 0; x < w; x ++)
        pixel(line + 3 * x, x, y, w, hgt, kind);
      cupsRasterWritePixels(ras, line, h.cupsBytesPerLine);
    }

    free(line);
  }

  cupsRasterClose(ras);
  close(fd);
  return (0);
}
