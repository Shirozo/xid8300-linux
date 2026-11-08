#!/usr/bin/env python3
"""Write the PostScript test cards used for the first prints on real hardware.

  make-testcards.py OUTDIR

Every card is CR80 landscape (248.64 x 159.36 pt = 1036 x 664 dots at 300 dpi)
and carries a black "TOP-LEFT" corner marker so orientation can be checked.
"""

import os
import sys

W, H = 248.64, 159.36

HEAD = """%!PS-Adobe-3.0
%%BoundingBox: 0 0 249 160
%%Title: {title}
%%Pages: {pages}
%%DocumentMedia: CR80 248.64 159.36 0 () ()
%%EndComments
/F {{ /Helvetica-Bold findfont exch scalefont setfont }} def
/T {{ 3 1 roll moveto show }} def
/marker {{ 0 setgray 0 147.36 12 12 rectfill 7 F 15 150 (TOP-LEFT) T }} def
"""


def page(n, body):
    return "%%Page: {0} {0}\n<< /PageSize [{1} {2}] >> setpagedevice\n{3}\nshowpage\n".format(n, W, H, body)


def card(name, title, pages):
    text = HEAD.format(title=title, pages=len(pages))
    text += "".join(page(i + 1, b) for i, b in enumerate(pages)) + "%%EOF\n"
    open(os.path.join(sys.argv[1], name), "w").write(text)


bars = []
for i, (label, rgb) in enumerate([("C", "0 1 1"), ("M", "1 0 1"), ("Y", "1 1 0"), ("R", "1 0 0"),
                                   ("G", "0 1 0"), ("B", "0 0 1"), ("K", "0 0 0")]):
    x = 12 + i * 32
    bars.append("{rgb} setrgbcolor {x} 70 28 40 rectfill 0 setgray 9 F {tx} 60 ({l}) T".format(
        rgb=rgb, x=x, tx=x + 10, l=label))
ramp = "".join("{g} setgray {x} 30 {w} 18 rectfill ".format(g=1 - i / 15.0, x=12 + i * 14.3, w=14.3)
               for i in range(16))

card("01-orientation-colours.ps", "XID8300 test 1: orientation and colours", [
    "marker\n" + "\n".join(bars) + "\n" + ramp +
    "\n0 setgray 8 F 12 112 (1: marker must be top-left; bars C M Y R G B K) T"
    "\n7 F 12 18 (grey ramp: white to black, left to right) T"])

card("02-yellow-only.ps", "XID8300 test 2: yellow only", [
    "1 1 0 setrgbcolor 20 20 208 110 rectfill"
    "\n0 setgray 8 F 20 140 (2: big rectangle must be YELLOW, not cyan) T"])

card("03-duplex-front-back.ps", "XID8300 test 3: duplex", [
    "marker\n0 setgray 40 F 50 60 (FRONT) T\n8 F 12 20 (3: print with -o Duplex=DuplexNoTumble) T",
    "marker\n0 setgray 40 F 60 60 (BACK) T\n8 F 12 20 (marker top-left when card is turned over) T"])

card("04-black-text.ps", "XID8300 test 4: resin black text", [
    "marker\n0 setgray"
    "\n12 F 12 120 (Resin K text 12 pt) T 8 F 12 105 (8 pt: The quick brown fox jumps) T"
    "\n6 F 12 94 (6 pt: over the lazy dog 0123456789) T"
    + "".join("\n{x} 30 {w} 50 rectfill".format(x=12 + i * 4.5, w=0.24 * (1 + i % 4)) for i in range(40))
    + "\n7 F 12 18 (4: text and bars must be sharp K, not CMY black) T"])

card("05-full-bleed.ps", "XID8300 test 5: full bleed", [
    "0.2 0.4 0.8 setrgbcolor 0 0 {w} {h} rectfill"
    "\n1 setgray 0.5 setlinewidth"
    "\n2.835 2.835 {w2} {h2} rectstroke 5.67 5.67 {w3} {h3} rectstroke"
    "\nmarker\n1 setgray 8 F 30 75 (5: blue to every edge; white lines 1 mm and 2 mm in) T".format(
        w=W, h=H, w2=W - 5.67, h2=H - 5.67, w3=W - 11.34, h3=H - 11.34)])
