# Linux driver for Matica XID8300 / XID8100 card printers (experimental)

A CUPS filter, network backend and PPDs for the Matica XID8300 and XID8100 retransfer card printers over
TCP/IP, written from the printer's protocol and file formats ([`docs/PROTOCOL.md`](docs/PROTOCOL.md)).

> **Not affiliated with or endorsed by Matica Technologies or JVC KENWOOD.** Product names are used only to
> say which printers this works with. The project contains no files or code from the manufacturer's Windows
> driver. **Experimental:** it has printed cards on an XID8100, but many options are untested on real
> hardware. Use blank cards and expect to waste a few while testing. No warranty (see `LICENSE`).

| Piece | File | Role |
|---|---|---|
| Filter | `src/rastertoxid8300.c` | CUPS raster → the printer's spool records (job header, Y/M/C/K/UV planes, settings) |
| Backend | `src/xid8300.c` | spool records → printer TCP protocol on port 50710 |
| PPDs | `ppd/xid8300.ppd` (+ generated `xid8100.ppd`) | card size, options, `*cupsFilter2` hook |
| Tools | `packaging/xid8300-setup`, `packaging/xid8300-import` | create a printer; import the optional colour table |
| Tests | `tests/` | raster generator, mock printer, end-to-end checks |

## Install from the .deb (Debian 12+, Ubuntu 22.04+)

```sh
sudo apt install ./printer-driver-xid8300_*_amd64.deb
sudo xid8300-setup --model XID8100 192.168.1.50        # or --model XID8300; add --id-card for portrait, two-sided
```

Then follow `/usr/share/printer-driver-xid8300/testcards/TESTING.md`.

To build the package (any machine with Docker, from the project root):

```sh
docker run --rm --platform linux/amd64 -v "$PWD":/src:ro -v "$PWD/dist":/out -e OUT=/out ubuntu:22.04 sh -c \
  'apt-get update && apt-get install -y build-essential libcups2-dev libcupsimage2-dev dpkg-dev python3 &&
   sh /src/packaging/build-deb.sh'
```

## Optional: the Matica colour table

The Windows driver can apply a colour look-up table (`LUT24_*.LUT`, "Enable the look-up table"). It is off by
default there and here, and it is **not included** in this project. To use it, copy it from your own Windows
driver download:

```sh
sudo xid8300-import /path/to/windows-driver-folder      # or the LUT24_xxx.LUT file itself
lp -d XID8100 -o ColorCorrection=LUT card.pdf
```

## Build and install from source (Linux)

You need the CUPS development headers (`libcups2-dev` on Debian/Ubuntu, `cups-devel` on Fedora).

```sh
make
sudo make install           # filter, backend, PPDs, xid8300-setup, xid8300-import
sudo systemctl restart cups
sudo lpadmin -p XID8300 -E -v xid8300://192.168.1.50 -m xid8300.ppd
```

**Tested builds** (`make && make check`, no compiler warnings):
- Debian 12 arm64 (CUPS 2.4.2, GCC 12)
- Ubuntu 24.04 x86-64 (CUPS 2.4.7, GCC 13)

On Ubuntu the full chain also ran under a real `cupsd`: `make install`, a queue created with `lpadmin`, and
PostScript and raster jobs printed with `lp` to the mock printer, with the filter and backend running as
`lp`. All tests used the mock printer, not real hardware.

- `lpadmin` warns "Printer drivers are deprecated". PPD drivers still work in CUPS 2.x. CUPS 3.x drops them,
  so this driver would then need to become a Printer Application.
- "Failed to connect to system bus" in a container comes from cups-filters' colour management and is harmless.

The device URI is `xid8300://host[:port]`. The port defaults to 50710. IPv6 literal addresses go in brackets,
e.g. `xid8300://[fe80::1%25eth0]`.

The optional LUT is read from `/var/lib/printer-driver-xid8300/LUT24.LUT` (PPD `*xidLUTFile`). If it is
missing the filter prints a warning and continues without colour correction.

## Printer models

- **XID8300:** the protocol and formats in `docs/PROTOCOL.md` are those of the XID8300 Windows driver.
- **XID8100:** uses the same protocol (it is printed from Windows with the XID8300 driver). Cards have been
  printed with this driver on an XID8100 (docs/PROTOCOL.md §10). Use `sudo xid8300-setup --model XID8100 <ip>`. The only difference is the model name CUPS shows, from
  `xid8100.ppd`.

## When the printer is not ready

When the printer answers with a "please wait" status, the backend shows the printer's own message in CUPS (for
example "Operation panel is currently in use, or the printer is in the transport mode.") and keeps retrying
for up to 30 minutes, as the Windows driver offers "Retry". This covers:
- operation panel in use or transport mode
- door open
- no cards
- film or ribbon run out
- preheating, initializing, cleaning
- password not entered on the panel

In power-saving mode it first wakes the printer with `RezeroUnit`, as Windows does. Real faults such as card
jams still stop the queue, now with the printer's message instead of only a number. The texts come from the
Windows driver's message table.

## USB

This driver works **only over the network** (TCP/IP, port 50710).
- If the printer has an Ethernet (LAN) port, use it. A direct cable between the computer and the printer also
  works if both have IP addresses in the same range.
- **Do not create a queue with a `usb://` address for this driver.** CUPS's USB backend would send the
  driver's internal data straight to the printer, which can't use it.

Why USB is missing: the Windows driver package you have contains only the network part ("XID8300N Port"). The
rendering DLL refers to a separate USB part ("XID8300U Port", addressed by a SCSI ID), but that part isn't in
the package, so nothing is known about the USB protocol. docs/PROTOCOL.md §9 has the evidence. The printer most
likely does not appear on Linux as an ordinary USB printer.

To make USB support possible later, collect, with the printer connected by USB and switched on:
- `lsusb` before and after plugging it in, to find its line (`ID vvvv:pppp`);
- `sudo lsusb -v -d vvvv:pppp`;
- `sudo dmesg | tail -40`.

The most useful item would be a USBPcap/Wireshark capture of the Windows driver printing one card over USB.

## ID cards on both sides (like Windows)

Windows: Setup → Orientation *Portrait*, Print → *Print on both sides*. On Linux the same is
`PageSize=CR80Portrait` and `Duplex=DuplexNoTumble`, with a two-page document (page 1 front, page 2 back):

```sh
lp -d XID8100 -o PageSize=CR80Portrait -o Duplex=DuplexNoTumble id-card.pdf
sudo lpadmin -p XID8100 -o PageSize=CR80Portrait -o Duplex=DuplexNoTumble   # or make it the default
sudo xid8300-setup --model XID8100 --id-card PRINTER_IP                       # same, when creating the queue
```

Use the portrait card size for portrait designs. With the landscape size CUPS turns the page sideways before the
driver sees it, and the back side then comes out upside down compared with Windows.

## Card layout and pages

- The card is 1036 × 664 dots at 300 dpi (87.7 × 56.2 mm, a little larger than CR80, so artwork bleeds off the edge).
- Pages are scaled to that full print area, like the Windows driver (which forces the page to 1036 × 664), so a
  design made at the card size (54 × 85.6 mm, or 638 × 1011 pixels) is printed edge to edge with bleed. A page
  whose shape is within 3 % of the card's is stretched to fill; any other shape is scaled to fit and centred.
  `CardFit=Actual` places the page 1:1 instead.
- PDF and PostScript pages are placed on the print area by CUPS before the filter sees them, so the queue also
  needs `print-scaling-default=fill` (`xid8300-setup` sets it; for a queue made another way run
  `sudo lpadmin -p XID8100 -o print-scaling-default=fill`).
- `PageSize=CR80` is landscape and `CR80Portrait` is portrait. Portrait pages are rotated the same way the
  Windows driver rotates them.
- Each card side takes one page. If a side's panel choice includes **UV**, the *next* page is that side's UV
  layer: every non-white pixel on it is printed with UV.
- With duplex on, pages go front, back, front, back. In landscape the back side is turned 180° by default, as
  the Windows driver does. `BackRotate180=True` cancels that, and `DuplexTumble` toggles it.

Example: 2-sided card, YMCK+UV front, K back → 3 pages per card (front colour, front UV, back).

```sh
lp -d XID8300 -o Duplex=DuplexNoTumble -o FrontPanels=YMCKUV -o BackPanels=K card.pdf
```

## Options

| Option | Values (default first) | Meaning |
|---|---|---|
| `FrontPanels`, `BackPanels` | `YMCK`, `YMC`, `K`, `YMCUV`, `KUV`, `YMCKUV` | ribbon panels used on that side |
| `BlackMode` | `KPlusColor`, `KOnly`, `Composite` | pure black gets K on top of CMY (Windows "With YMC ink"), K only, or stays CMY |
| `BlackThreshold` | `0`, `16`, `32` | how dark a pixel must be to count as "black" |
| `MonoMethod` | `Threshold`, `Dither` | how K-only sides turn grey into resin K |
| `ColorCorrection` | `None`, `LUT` | Windows "Enable the look-up table" (off by default, as in Windows): apply the Matica LUT24 cube |
| `FrontRotate180`, `BackRotate180` | `False`, `True` | turn the side 180° |
| `Duplex` | `None`, `DuplexNoTumble`, `DuplexTumble` | front only / both sides |
| `MagCardFlip` | `False`, `True` | see *Magnetic stripe* below |
| `ICCardFlip` | `False`, `True` | see *Smart card* below |
| `CardFit` | `Fill`, `Actual` | scale the page to the full print area (bleed), or print it 1:1 |
| `ImageShiftX`, `ImageShiftY` | `0`, `-3.00` … `3.00` (mm, 0.25 steps) | see *Print position* below |
| Printer settings, colour adjustment, encode, laminate | see below | see *Windows printing preferences on Linux* |

Copies are made by CUPS (`*cupsManualCopies: True`).

## Windows printing preferences on Linux

The options are grouped like the tabs of the Windows driver. Find them in the printer's **Printer Options**
(GNOME/KDE printer settings), at `http://localhost:631` → *Printers* → the printer → *Set Default Options*, in an
application's print dialog, or with `lpoptions -p XID8100 -l`. Set a default with
`sudo lpadmin -p XID8100 -o KPrintMode=Fine`, or per job with `lp -o KPrintMode=Fine`.

| Windows | Linux group | Options |
|---|---|---|
| Setup: orientation, copies | General | `PageSize` (CR80 / CR80Portrait), copies from the print dialog |
| Setup: turn over the card after loading | General | `TurnOverAfterLoad` |
| Print: both sides, back side first | General | `Duplex`, `BackSideFirst` |
| Print: print mode, Resin K, security erase, rotate 180 | Print | `FrontPanels`, `BackPanels`, `BlackMode`, `BlackThreshold`, `MonoMethod`, `FrontRotate180`, `BackRotate180` |
| Print → Color adjustment, Look-up table | Color Adjustment | `Brightness`, `Contrast`, `HueRed/Green/Blue`, `GammaRed/Green/Blue`, `ColorCorrection` |
| Print → Printer settings | Printer Settings | `YMCLevel`, `KLevel`, `UVLevel`, `PeelOffLevel` (−3…+3), `KPrintMode`, `MagPeelMode`, `StandbyMode`, `BackCooling`, `RetransferTemp`, `RetransferSpeedFront/Back`, `UVSpeedFront/Back`, `BendRemedyTemp`, `BendRemedySpeed` |
| Print → UV ink: MAC address position, film panels | Print | `UVMarkPosition`, `UVFilmPanels` |
| Print → Printing area | (command line) | `area1=…` options, see *Print areas* |
| Encode | Encode | `MagCardFlip`, `ICCardFlip`, `MagWriteRetry`, `MagCoercivity`, `EncodeBeforePrint`; track/chip data as job options |
| Laminate | Laminate | `LaminateMode` |
| Configuration | Installed Options | `MagEncoderType` (ISO/JIS; decides how coercivity is sent) |

"Printer setting" (`Default`) leaves the printer's own value unchanged, like an unticked box in Windows; only the
options you set are sent. Not available: Load/Save settings, the Version page, the dither darkness slider and the
"Only text"/"Page split" black modes (both need Windows text/page data), UV text/file extraction and UV image
files, "Starts from magnetic encoding", and the Encode "Security" box (the Windows port monitor never sends it).

## Print position

If a printed card shows a white strip along one edge, the printer places the image off-centre. Measure the
strip and move the image **towards that edge** by the same amount. Both options act on both sides, and the
directions are as seen on a landscape card (front, design upright):

| White strip on a landscape card | Portrait card | Option |
|---|---|---|
| top edge | right edge | `ImageShiftY=-<mm>` (up) |
| bottom edge | left edge | `ImageShiftY=<mm>` (down) |
| left edge | top edge | `ImageShiftX=-<mm>` (left) |
| right edge | bottom edge | `ImageShiftX=<mm>` (right) |

Set it once on the queue: `sudo lpadmin -p XID8100 -o ImageShiftY=-1.25`. Any value from −5 to 5 mm works on
the command line (`-o ImageShiftY=-1.4`); the PPD lists 0.25 mm steps up to ±3 mm.

## Magnetic stripe

Track data is passed as job options, because a CUPS raster has no text to hide escape codes in. The same data
is encoded on **every card in the job**, so submit one job per card when the data differs.

```sh
lp -d XID8300 \
   -o mag-track1='%B4111111111111111^DOE/JOHN^2912101?' \
   -o mag-track2=';4111111111111111=2912101?' \
   -o mag-track3='0123456789' card.pdf
```

| Option | Content | Max | Characters |
|---|---|---|---|
| `mag-track1` | ISO track 1 | 76 (77–79 use the long 6-bit format automatically) | `0x20`–`0x5F` (upper case) |
| `mag-track1-format=7bit` | track 1 as 7-bit | 69 | printable ASCII |
| `mag-track2` | ISO track 2 | 37 | digits and `: ; < = >` |
| `mag-track3` | ISO track 3 | 104 | digits and `: ; < = >` |
| `mag-track3-format=6bit` / `7bit` | track 3 as 6-bit / 7-bit | 79 / 69 | as track 1 / printable ASCII |
| `mag-jis2` | JIS-II | 69 | printable ASCII (no katakana) |

- Start and end sentinels (`%`/`;` and `?`) are optional. They are removed before sending, because the
  Windows driver also sends the data without them.
- Invalid data stops the job in the filter. The error names the option and character position but never
  shows the data.
- The filter sets job-header byte 6 to 0x10, or 0x11 with `MagCardFlip=True`, and puts the track records on
  each card's front side.
- The backend then follows the Windows port monitor:
  1. Feed the card to the encoder (position 3).
  2. Image and print the front film.
  3. Send JIS-II in its own `WriteMG`, then all ISO tracks in one `WriteMG`.
  4. Move the card to position 0 and transfer the film.
  - If encoding fails, the card is ejected and the queue is stopped.
- `MagCardFlip=True` makes the move back to the print position use LoadCard/MoveCard flag 0x10, as the
  driver's value 0x11 does. What that does physically (probably turning the card over) is unconfirmed.
- **Coercivity (Lo-Co/Hi-Co) is not selectable.** It is not in any frame the port monitor builds. It may be one
  of the unexplained printer settings, or a setting on the printer itself.
- **Card data handling:** track data is a job option, so CUPS keeps it with the job (in the spool directory
  and job history) and may show it in job attributes, depending on `cupsd.conf`. Check `JobPrivateValues`,
  `PreserveJobHistory` and `PreserveJobFiles` before using real account data. `XID8300_TRACE=1` also
  hex-dumps the start of every frame sent, including the first track bytes.

## Smart card (contact / contactless)

The printer never receives chip data. As in the Windows driver, the backend:

1. Moves the card to the encoding station: contactless = position 2, contact = position 1.
2. Engages the station (`ICControl`).
3. Asks the printer for 10 bytes of "encoder info".
4. Runs an **encoding program** that talks to the chip itself, normally through the PC/SC reader built into
   the printer.
5. Releases the station.

On Windows that program is a vendor plug-in, `PDR24IC0.DLL` (contactless) or `PDR24IC1.DLL` (contact). It is
not part of the driver package, so on Linux you supply an executable:

| Station | Job option | Program (default path; override with env) |
|---|---|---|
| contact | `ic-contact=DATA` | `$(cups-config --serverbin)/xid8300/ic-contact` (`XID8300_IC_CONTACT`) |
| contactless | `ic-contactless=DATA` | `$(cups-config --serverbin)/xid8300/ic-contactless` (`XID8300_IC_CONTACTLESS`) |

```sh
lp -d XID8300 -o ic-contactless='hex:00A4040007A0000000041010' card.pdf
```

**Program contract** (this backend's design, modelled on the plug-in call
`Encode(handle, out, encoderInfo[10], data)`):

- Called as `PROGRAM contact|contactless`.
- The job option's value arrives on **stdin**: raw text, or bytes decoded from a `hex:` prefix (up to 4096
  bytes).
- Environment variables:
  - `XID8300_UNIT`
  - `XID8300_ENCODER_INFO`: the printer's 10 info bytes, in hex
  - `XID8300_DATA_LENGTH`
  - `XID8300_PRINTER`: host
  - `XID8300_CARD`: card number in the job
- stdout and stderr go to the CUPS log, so `INFO:` and `ERROR:` lines show up in the queue status.
- Exit codes:

  | Exit code | Meaning | Card | CUPS backend result |
  |---|---|---|---|
  | 0 | encoded | stays in, printing continues | — |
  | 10 | reject card (plug-in's 0x1001) | ejected | job fails |
  | 11 | cancel job (plug-in's 0x1002) | ejected | job cancelled |
  | anything else, or a timeout | encoding error | ejected | **queue stopped** |

  The timeout is `XID8300_IC_TIMEOUT`, default 300 s.
- A missing program is reported as "Contact/Contactless IC encoding program is not found" before the station
  is engaged.

**Order and timing** (as in the Windows monitor):

- Encoding happens after the front film has been printed.
- Stations run in the order contactless, contact, magnetic stripe.
- The card is then transferred.
- The card is fed straight to the first station it needs.
- `ICCardFlip=True` sets job-header bit0, which toggles the LoadCard/MoveCard flag after IC encoding. What that
  flag does physically is unconfirmed.

**Example program:** `examples/ic-encoder-example.sh` (untested) treats the data as APDU hex strings, one per
line, and sends them with OpenSC's `opensc-tool`. `make install` copies it into the `xid8300` directory
**without** enabling it; copy it to `ic-contact` / `ic-contactless` and make it executable.
The backend runs as `lp`, so check that `lp` may use `pcscd` (polkit). Environment such as `XID8300_READER`
can be passed with `SetEnv` in `cups-files.conf`.

Card data handling is the same as for magnetic stripes: the data is a CUPS job option and is stored with the
job.

## K-secure (not included)

The Windows driver's "Security erase" (K-secure) needs a pattern file from the Windows driver and is not part
of this project. The backend still prints a K-secure record (0x88) if a job contains one, as described in
`docs/PROTOCOL.md` §8.

## Print areas and peel-off (0x86 plane)

Up to 8 rectangles per job, as job options `area1` … `area8`:

```sh
lp -d XID8300 -o area1=front:peeloff:20,8,12,10mm -o area2=back:noprint:600,40,380,120 card.pdf
```

Format: `SIDE:TYPE:X,Y,W,H`, in dots (300 dpi), or in millimetres with an `mm` suffix. Coordinates are
measured on the side **as you design it**; the filter maps them through the portrait and back-side rotations,
because the driver applies areas after rotation.

| Type | Effect (same as the driver) |
|---|---|
| `noprint` | the rectangle is left blank on every panel |
| `peeloff` | the rectangle plus a 4/6-dot margin is left blank, and on sides with a K panel it is filled in the **0x86 peel-off plane** (5 rows at the edge level, then a ramp, 8 dots higher) |

- `peeloff-levels=EDGE,START,END` sets the 0x86 values. The driver's defaults are `225,160,160`.
- Peel-off needs a peel-off ribbon panel (the driver's "YMCK-PO"). It cannot be combined with UV on the same
  side. The Windows monitor rejects that as "Ink ribbon specification is incorrect", and the
  filter stops with a clearer message.
- The meaning of "peel-off" is inferred from the driver's ribbon names. Typical uses would be chip contacts or
  a signature panel.

## How it maps to the Windows driver

- **Filter:** RGB is inverted to CMY (255 − v), then the LUT is applied with
  `i = (Y<<16)|(M<<8)|C → (Y,M,C) = lut[16+3i..]`.
  Output records are 0x01 (job header), 0x81/0x82/0x83 (Y/M/C), 0x84 (K), 0x87 (UV), 0xF3 (end of side)
  and 0xF5 (end of job), all raw 8-bit planes with no compression.
- **Backend:** follows the port monitor's per-card sequence:
  - status and property queries
  - settings (empty unless the stream carries a 0xC0 record)
  - ReadPosition, then LoadCard
  - ImageOut for each plane, then PrintOnFilm
  - TestUnitReady polling, then PrintOnCard (mode 5 on the last side; mode 0 + flag 0x10 on the first side of
    a duplex card)
  - MoveCard(4) to eject if the card is still in the printer
  - When a side has both UV and colour/K panels, UV is printed in its own film pass, as the monitor does with
    its default UV setting.

## Not implemented

- The interactive magnetic read/write and "user program" plug-in paths (records 0x60/0x70 are skipped with a
  warning). Magnetic read-back verification (`P_ReadMG`) is not used, matching the Windows print path.
- The chip-side protocol: it lives in the encoding program you supply, not in this driver.
- K-secure / "Security erase" (see above), the "Page split" and "Only text" black modes, UV from text or image
  files, the dither slider, and "Starts from magnetic encoding".
- USB (see the *USB* section).

## Help wanted: things to verify on real printers

1. **Two-sided cards.** Check that the back is upright for portrait (`PageSize=CR80Portrait`) and landscape
   cards.
2. **Printer settings.** What each value of the "Printer Settings" group does (ink levels, retransfer and
   bend-remedy temperature/speed, UV speed direction).
3. **Magnetic stripe.** Encode a test card and read it back. Check which side the stripe is encoded on,
   whether `MagCardFlip` matters, and the coercivity.
4. **Smart card.** With the station engaged, check that the printer's reader shows up in `pcsc_scan` and sees
   the card.
5. **Peel-off.** Check that peel-off areas come out as expected on a "PO" ribbon.
6. **USB.** See the *USB* section.

Run with `XID8300_TRACE=1` in the backend environment to hex-dump every frame the printer sends. A packet
capture of the Windows driver printing one card (`tcp.port == 50710`) settles most questions quickly.

## Tests

```sh
make check
```

`make check` builds test rasters, runs them through the filter and backend into `tests/mock_xid8300.py`, and
checks:

- the command order
- frame lengths
- that the plane bytes are correct, including portrait rotation, duplex back-side rotation, the UV second
  pass, the K-only side, and busy/keep-alive handling
- magnetic encoding: card feed to the encoder, the order of encoding and transfer, the track contents decoded
  back by the mock, JIS-II as a separate write, the `MagCardFlip` flag, and ejecting on an encoding error
- that invalid track data is rejected without leaking into the log
- a K-secure record in the job: the SecurityPrint pass order
- peel-off and no-print areas: the 0x86 fill levels and ramp, margins, mapping through back and portrait
  rotation, mm units, and the ribbon-combination errors in both the filter and the backend
- the print-area routines against fixed test vectors (`tests/golden.sha256`)
- printer settings (0xC0 items and their order), colour adjustment, back side first, turn over after
  loading, scaling to the print area and the image-position correction
- smart card: station positions and `ICControl` engage/release codes, program arguments, stdin data and
  environment, contactless → contact → magnetic order, and each exit code (OK, reject, cancel, error,
  missing program) ejecting the card with the right CUPS result

The planes the mock receives are saved as PGM images in `tests/out/<case>/`.

The mock printer can also be run by hand to try a real CUPS queue without hardware:

```sh
python3 tests/mock_xid8300.py --port 50710 --out /tmp/xid --sessions 100
sudo lpadmin -p XIDTEST -E -v xid8300://127.0.0.1 -m xid8300.ppd
```

## Licence

GPL-2.0-or-later, see `LICENSE`. Matica and XID are trademarks of their owners.
