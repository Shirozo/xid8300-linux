#!/usr/bin/env python3
"""Checks the mock-printer logs and saved planes written by tests/run.sh."""

import os
import re
import sys

W, H = 1036, 664
OUT = sys.argv[1]
failures = []


def check(cond, msg):
    print(("ok   " if cond else "FAIL ") + msg)
    if not cond:
        failures.append(msg)


def log(case):
    with open(os.path.join(OUT, case, "mock.log")) as f:
        return [l.strip() for l in f if l.startswith(("CMD", "SESSION"))]


def cmds(case, name):
    return [l for l in log(case) if l.startswith("CMD " + name)]


def plane(case, fname):
    path = os.path.join(OUT, case, fname)
    if not os.path.exists(path):
        return None
    with open(path, "rb") as f:
        data = f.read()
    return data[len(b"P5\n1036 664\n255\n"):]


def region(p, r0, r1, c0, c1):
    return [p[r * W + c] for r in range(r0, r1) for c in range(c0, c1)]


def kinds(case):
    seq = []
    for l in log(case):
        m = re.match(r"(CMD|SESSION) (\w+)", l)
        seq.append(m.group(2))
    return seq


def subsequence(seq, want):
    it = iter(seq)
    return all(any(x == w for x in it) for w in want)


# --- simplex: command order and orientation -------------------------------
seq = kinds("simplex")
check(subsequence(seq, ["StartJob", "PropertyReq", "StatusReq", "PropertyReq", "SettingValue",
                        "ReadPosition", "LoadCard", "ImageOut", "ImageOut", "ImageOut", "ImageOut",
                        "PrintOnFilm", "TestUnitReady", "PrintOnCard", "ReadPosition", "MoveCard"]),
      "simplex: command sequence follows PDR24PM64")
check([re.search(r"plane=(\w+)", l).group(1) for l in cmds("simplex", "ImageOut")] == ["Y", "M", "C", "K"],
      "simplex: planes sent Y, M, C, K")
check(all("ok" in l for l in cmds("simplex", "ImageOut")), "simplex: ImageOut frame lengths consistent")
check("p2=0f" in cmds("simplex", "PrintOnFilm")[0], "simplex: PrintOnFilm panels 0x0f (YMC+K)")
check("p2=05" in cmds("simplex", "PrintOnCard")[0], "simplex: PrintOnCard mode 5")
check("p2=04" in cmds("simplex", "MoveCard")[-1], "simplex: card ejected with MoveCard(4)")
k = plane("simplex", "card1_pass1_K.pgm")
y = plane("simplex", "card1_pass1_Y.pgm")
c = plane("simplex", "card1_pass1_C.pgm")
check(k is not None and min(region(k, 0, 40, 0, 40)) == 255, "simplex: K marker at top-left")
check(k is not None and max(region(k, 0, 40, 100, 140)) == 0, "simplex: no K outside black areas")
check(y is not None and min(region(y, 0, 40, 0, 40)) == 255,
      "simplex: default black keeps YMC under the K (Windows \"With YMC ink\")")
y = plane("konlyext", "card1_pass1_Y.pgm")
check(y is not None and max(region(y, 0, 40, 0, 40)) == 0, "konlyext: BlackMode=KOnly removes pure black from YMC")
if os.path.isdir(os.path.join(OUT, "lut")):
    y = plane("lut", "card1_pass1_Y.pgm")
    y0 = plane("simplex", "card1_pass1_Y.pgm")
    check(y is not None and y0 is not None and y != y0, "lut: ColorCorrection=LUT changes the colours (off by default)")
check(c is not None and max(region(c, H // 3, H // 2, 0, 20)) > 0, "simplex: gradient band present in C")

# --- portrait: driver rotation dst[(663-x)*1036 + y] -----------------------
k = plane("portrait", "card1_pass1_K.pgm")
check(k is not None and min(region(k, H - 40, H, 0, 40)) == 255, "portrait: marker rotated to bottom-left")

# --- duplex: front then back, back rotated 180 ------------------------------
pc = cmds("duplex", "PrintOnCard")
check(len(pc) == 2 and "p1=10 p2=00" in pc[0] and "p2=05" in pc[1],
      "duplex: PrintOnCard first side mode 0/flag 0x10, then mode 5")
pf = cmds("duplex", "PrintOnFilm")
check(len(pf) == 2 and "p1=00" in pf[0] and "p1=20" in pf[1], "duplex: second side flagged 0x20")
kb = plane("duplex", "card1_pass2_K_second.pgm")
check(kb is not None and min(region(kb, H - 40, H, W - 40, W)) == 255,
      "duplex: back side rotated 180 (marker bottom-right)")

# --- UV: separate film pass -------------------------------------------------
pf = cmds("uv", "PrintOnFilm")
pc = cmds("uv", "PrintOnCard")
check(len(pf) == 2 and "p2=0f" in pf[0] and "p1=01 p2=40" in pf[1], "uv: colour pass then UV pass (mode 1)")
check(len(pc) == 2 and "p1=00 p2=00" in pc[0] and "p2=05" in pc[1], "uv: intermediate transfer then final")
uv = plane("uv", "card1_pass2_UV.pgm")
check(uv is not None and min(region(uv, 0, 10, 0, W)) == 255 and uv[(H // 2) * W + W // 4] == 0,
      "uv: UV plane = non-white pixels of the UV page")

# --- K only -------------------------------------------------------------------
check([re.search(r"plane=(\w+)", l).group(1) for l in cmds("konly", "ImageOut")] == ["K"], "konly: only K sent")
check("p2=08" in cmds("konly", "PrintOnFilm")[0], "konly: PrintOnFilm panels 0x08")

# --- busy / keep-alive ------------------------------------------------------------
check("KeepAlive" in kinds("busy"), "busy: keep-alive answered the wait message")
check(len(cmds("busy", "TestUnitReady")) >= 3, "busy: TestUnitReady polled through busy states")

# --- two cards in one job -----------------------------------------------------------
check(len(cmds("twocards", "LoadCard")) == 2 and len(cmds("twocards", "PrintOnCard")) == 2,
      "twocards: two cards in one session")

# --- magnetic stripe ------------------------------------------------------------
def idx(case, prefix):
    seq = log(case)
    return [i for i, l in enumerate(seq) if l.startswith(prefix)]

seq = kinds("mag")
check(subsequence(seq, ["ReadPosition", "LoadCard", "ImageOut", "PrintOnFilm", "TestUnitReady",
                        "WriteMG", "ReadPosition", "MoveCard", "TestUnitReady", "PrintOnCard", "MoveCard"]),
      "mag: card fed to encoder, film printed, then encoded, then moved to print position")
check("p2=03" in cmds("mag", "LoadCard")[0], "mag: initial feed to position 3 (encoder)")
wm = cmds("mag", "WriteMG")
check(len(wm) == 1 and "at=3" in wm[0] and "blocks=3" in wm[0] and " ok " in wm[0],
      "mag: one WriteMG with 3 tracks while the card is at the encoder")
check(len(wm) == 1 and "16:B4111111111111111^DOE/JOHN^2912101|24:4111111111111111=2912101|34:0123456789" in wm[0],
      "mag: track contents round-trip (sentinels stripped, 6-bit/4-bit encodings)")
check("p1=00 p2=00" in cmds("mag", "MoveCard")[0], "mag: card moved back to position 0 without flag")
check(idx("mag", "CMD WriteMG")[0] < idx("mag", "CMD PrintOnCard")[0], "mag: encoding before transfer to card")

wm = cmds("magflip", "WriteMG")
check(len(wm) == 1 and "24:1234567890" in wm[0], "magflip: duplex card encoded once (front side data)")
check("p1=10 p2=00" in cmds("magflip", "MoveCard")[0], "magflip: MagCardFlip sets the 0x10 card flag after encoding")

wm = cmds("magjis", "WriteMG")
check(len(wm) == 2 and "blocks=1 07:JIS-TEST-0001" in wm[0] and "17:HELLO" in wm[1],
      "magjis: JIS-II written alone first, then 7-bit track 1")

check(len(cmds("magerror", "WriteMG")) == 1 and not cmds("magerror", "PrintOnCard")
      and "p2=04" in cmds("magerror", "MoveCard")[-1], "magerror: encoding failure ejects the card, no transfer")
with open(os.path.join(OUT, "magerror", "backend.log")) as f:
    check("Magnetic encoding failed" in f.read(), "magerror: error reported to CUPS")

# --- smart card --------------------------------------------------------------------
def iclog(case):
    path = os.path.join(OUT, case, "ic.log")
    return open(path).read().splitlines() if os.path.exists(path) else []

seq = kinds("iccontact")
check(subsequence(seq, ["LoadCard", "ImageOut", "PrintOnFilm", "TestUnitReady", "ICControl", "SettingReq",
                        "ICControl", "ReadPosition", "MoveCard", "TestUnitReady", "PrintOnCard"]),
      "iccontact: station engaged after the film pass, released, card back to print position")
check("p2=01" in cmds("iccontact", "LoadCard")[0], "iccontact: card fed to the contact station (position 1)")
ic = cmds("iccontact", "ICControl")
check(len(ic) == 2 and "p1=00" in ic[0] and "at=1" in ic[0] and "p1=40" in ic[1],
      "iccontact: ICControl engage 0x00 / release 0x40 at position 1")
lines = iclog("iccontact")
check(len(lines) == 1 and lines[0].startswith("contact card=1 len=10 ")
      and "info=454E4330313233343536" in lines[0] and "data=68656c6c6f2d63686970" in lines[0]
      and "printer=127.0.0.1" in lines[0],
      "iccontact: program got unit, data on stdin, encoder info and printer host")

seq = kinds("icall")
check("p2=02" in cmds("icall", "LoadCard")[0], "icall: first station is contactless (position 2)")
ic = cmds("icall", "ICControl")
check(len(ic) == 4 and "p1=10" in ic[0] and "at=2" in ic[0] and "p1=50" in ic[1]
      and "p1=00" in ic[2] and "at=1" in ic[2] and "p1=40" in ic[3],
      "icall: contactless (0x10/0x50 at 2) then contact (0x00/0x40 at 1)")
lines = iclog("icall")
check(len(lines) == 2 and lines[0].startswith("contactless") and "data=00a4040007a0000000041010" in lines[0]
      and lines[1].startswith("contact ") and "data=58" in lines[1],
      "icall: hex data decoded, contactless program before contact program")
wm = cmds("icall", "WriteMG")
check(len(wm) == 1 and "at=3" in wm[0], "icall: magnetic stripe encoded last at position 3")
mv = cmds("icall", "MoveCard")
check(any("p1=10 p2=03" in l for l in mv), "icall: ICCardFlip toggles the card flag before the magnetic step")
check(subsequence(seq, ["ICControl", "ICControl", "ICControl", "ICControl", "WriteMG", "MoveCard", "PrintOnCard"]),
      "icall: all encoding done before transfer")

for case, what in (("iccancel", "cancel"), ("icreject", "reject"), ("icerror", "error")):
    ic = cmds(case, "ICControl")
    check(len(ic) == 2 and "p1=40" in ic[1] and not cmds(case, "PrintOnCard")
          and "p2=04" in cmds(case, "MoveCard")[-1],
          "%s: station released, card ejected, nothing transferred" % case)
check(not cmds("icmissing", "ICControl") and "p2=04" in cmds("icmissing", "MoveCard")[-1],
      "icmissing: missing program detected before engaging, card ejected")
with open(os.path.join(OUT, "icmissing", "backend.log")) as f:
    check("IC encoding program is not found" in f.read(), "icmissing: reported like the Windows driver")

# --- duplex from the job option when the raster header says simplex -----------
pc = cmds("duplexopt", "PrintOnCard")
check(len(cmds("duplexopt", "LoadCard")) == 1 and len(pc) == 2 and "p1=10 p2=00" in pc[0] and "p2=05" in pc[1],
      "duplexopt: Duplex job option makes one two-sided card even without the raster flag")

# --- printer states seen on real hardware (XID8100 field test) --------------------
blog = open(os.path.join(OUT, "panelbusy", "backend.log")).read()
check(len(cmds("panelbusy", "SettingValue")) == 4 and len(cmds("panelbusy", "PrintOnCard")) == 1
      and "Operation panel is currently in use" in blog and "Printer ready, continuing" in blog,
      "panelbusy: status 0x02DD00 shows the printer's message, waits, then prints")
check(subsequence(kinds("sleeping"), ["LoadCard", "RezeroUnit", "LoadCard", "PrintOnCard"])
      and "Power Saving" in open(os.path.join(OUT, "sleeping", "backend.log")).read(),
      "sleeping: power saving answered with RezeroUnit, then the card is fed")
blog = open(os.path.join(OUT, "jammed", "backend.log")).read()
check("Settings failed: Card jam near the card feed roller. (printer status 0x039300)" in blog
      and not cmds("jammed", "PrintOnCard"),
      "jammed: real errors still stop, with the printer's own message")

# --- K-secure record handling (backend) ------------------------------------------------
seq = kinds("ksflow")
check(subsequence(seq, ["ImageOut", "PrintOnFilm", "TestUnitReady", "ImageOut", "SecurityPrint",
                        "TestUnitReady", "PrintOnCard"]),
      "ksflow: K film pass, then the 0x88 plane with SecurityPrint, then transfer")
check(len(cmds("ksflow", "SecurityPrint")) == 1 and "p1=00 p2=08" in cmds("ksflow", "SecurityPrint")[0],
      "ksflow: SecurityPrint on panel 0x08")

# --- peel-off / no-print areas ---------------------------------------------------------
check([re.search(r"plane=(\w+)", l).group(1) for l in cmds("peeloff", "ImageOut")] == ["Y", "M", "C", "K", "P86"],
      "peeloff: 0x86 plane sent last with plane id 0x20")
check("p2=2f" in cmds("peeloff", "PrintOnFilm")[0], "peeloff: PrintOnFilm includes panel bit 0x20")
p86 = plane("peeloff", "card1_pass1_P86.pgm")
check(p86 is not None and min(region(p86, 42, 47, 100, 300)) == 225 and min(region(p86, 47, 82, 100, 300)) == 160
      and max(region(p86, 82, 90, 100, 300)) == 0 and max(region(p86, 0, 42, 0, W)) == 0
      and max(region(p86, 300, 400, 600, 700)) == 0,
      "peeloff: fill 8 dots up, 5 rows at 225 then 160; no fill for no-print areas")
y = plane("peeloff", "card1_pass1_Y.pgm")
check(y is not None and max(region(y, 44, 96, 96, 304)) == 0 and max(region(y, 300, 400, 600, 700)) == 0
      and max(region(y, H // 3, H // 2, 310, 590)) > 0,
      "peeloff: colours cut out (peel-off with 4/6-dot margin, no-print exact)")
p86 = plane("peelback", "card1_pass2_P86_second.pgm")
check(p86 is not None and min(region(p86, 566, 606, 736, 936)) >= 160 and max(region(p86, 0, 500, 0, W)) == 0,
      "peelback: back-side area mapped through the 180-degree back rotation")
p86 = plane("peelpor", "card1_pass1_P86.pgm")
check(p86 is not None and min(region(p86, 356, 556, 50, 90)) >= 160 and max(region(p86, 0, 350, 0, W)) == 0,
      "peelpor: portrait area mapped through the portrait rotation")
p86 = plane("peelmm", "card1_pass1_P86.pgm")
# ramp: start - (start-end)*k/(rows-5), integer division -> 150 at row 5, 102 at row 49
check(p86 is not None and min(region(p86, 0, 5, 0, 50)) == 200 and p86[5 * W] == 150 and p86[49 * W] == 102,
      "peelmm: peeloff-levels edge 200, ramp 150 -> 102 (driver integer ramp)")
k = plane("peelmm", "card1_pass1_K.pgm")
check(k is not None and max(region(k, 60, 110, 118, 354)) == 0 and min(region(k, 60, 110, 60, 118)) == 255,
      "peelmm: no-print area given in mm (10,5,20,10 mm = 118,59,236,118 dots) cut exactly")

# --- printer settings and other Windows preferences ---------------------------------------
sv = cmds("psettings", "SettingValue")
check(len(sv) == 1 and "items=[F=2 G=1 H=5 L=3 M=5 N=2 O=10 P=4 V=1 c8=1 ^=4 _=0]" in sv[0],
      "psettings: printer settings sent as 0xC0 items in the port monitor's order (%s)" % (sv[0].split("items=")[-1] if sv else "none"))
check(all("items=[]" in l for l in cmds("simplex", "SettingValue")), "simplex: no printer settings sent by default")
sv = cmds("magset", "SettingValue")
check(len(sv) == 1 and "items=[(=2 *=1]" in sv[0], "magset: write retry and Hi-Co coercivity (ISO encoder id '*') sent")
seq = kinds("magset")
check(subsequence(seq, ["WriteMG", "PrintOnFilm"]), "magset: EncodeBeforePrint encodes before the film is printed")
y0, y1 = plane("simplex", "card1_pass1_Y.pgm"), plane("coloradj", "card1_pass1_Y.pgm")
k0, k1 = plane("simplex", "card1_pass1_K.pgm"), plane("coloradj", "card1_pass1_K.pgm")
band = lambda p: sum(region(p, H // 3, H // 2, 0, W))
check(y0 and y1 and band(y1) != band(y0) and k0 == k1,
      "coloradj: colour adjustment changes the colour planes, pure black (K) untouched")
k = plane("backfirst", "card1_pass1_K.pgm")
check(k is not None and min(region(k, 624, 659, 996, 1031)) == 255,
      "backfirst: the back side (turned 180) is printed first")
check(any("flags=12" in l or "flags=10" in l for l in cmds("backfirst", "LoadCard")) or
      any(" p1=10" in l for l in cmds("backfirst", "LoadCard")),
      "backfirst: TurnOverAfterLoad loads the card with flag 0x10 (%s)" % cmds("backfirst", "LoadCard")[:1])

# --- image size and position ------------------------------------------------------------
y, k = plane("fill", "card1_pass1_Y.pgm"), plane("fill", "card1_pass1_K.pgm")
ink = [a or b for a, b in zip(y, k)] if y and k else None
check(ink is not None and min(ink) > 0,
      "fill: CR80-size portrait page stretched over the whole 1036x664 print area (no white edge)")
check(k is not None and min(region(k, H - 40, H, 0, 40)) == 255 and max(region(k, 0, H - 44, 0, W)) == 0,
      "fill: portrait top-left marker scaled and rotated to the plane's bottom-left")
y, k = plane("actual", "card1_pass1_Y.pgm"), plane("actual", "card1_pass1_K.pgm")
ink = [a or b for a, b in zip(y, k)] if y and k else None
check(ink is not None and max(region(ink, 0, 13, 0, W)) == 0 and max(region(ink, 0, H, 0, 12)) == 0
      and max(region(ink, H - 13, H, 0, W)) == 0 and min(region(ink, 13, H - 13, 12, W - 13)) > 0,
      "actual: CardFit=Actual keeps the page 1:1, centred, with a white border")
k, y = plane("shift", "card1_pass1_K.pgm"), plane("shift", "card1_pass1_Y.pgm")
check(k is not None and min(region(k, 0, 28, 12, 52)) == 255 and max(region(k, 0, H, 0, 12)) == 0
      and max(region(k, 30, 40, 12, 52)) == 0,
      "shift: ImageShiftX=1 ImageShiftY=-1 mm moves K 12 dots right and 12 up, padding with no ink")
y0 = plane("simplex", "card1_pass1_Y.pgm")
check(y is not None and y0 is not None and y[(H - 1) * W + 500] == y0[(H - 4) * W + 488]
      and y[300 * W + 5] == y0[312 * W + 3],
      "shift: colour planes shifted the same way, rows/columns 3 dots in repeated to keep the bleed filled")

sys.exit(1 if failures else 0)
