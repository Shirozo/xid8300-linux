# XID8300 / XID8100 Linux driver: testing on a real printer

This driver is experimental. These steps are ordered so that each one tests one thing; use blank cards and
note what happens at every step, even when it works.

## Network or USB?

This driver talks to the printer **over the network only**.
- If the printer is on USB but has an Ethernet (LAN) port, connect that port to the network (or directly to
  the computer, with IP addresses in the same range) and use the printer's IP address below.
- **Do not pick a `usb://` address** for this driver; the printer would receive data it can't use.
- If USB is the only option, printing can't be tested yet. Please collect this instead (about 5 minutes), so
  that USB support can be added:

```sh
sudo apt install usbutils                # provides lsusb
lsusb                                   # run before and after plugging in; note the new line's ID vvvv:pppp
sudo lsusb -v -d vvvv:pppp > ~/xid8300-usb.txt 2>&1
sudo dmesg | tail -40   >> ~/xid8300-usb.txt
ls -l /dev/usb /dev/sg* >> ~/xid8300-usb.txt 2>&1
```

If a Windows PC prints to it over USB with the original driver, a USBPcap/Wireshark capture of one card being
printed would be the most useful thing of all.

## 0. Install and create the queue

```sh
sudo apt install ./printer-driver-xid8300_*.deb
sudo xid8300-setup 192.168.1.50                    # XID8300 at this IP -> queue "XID8300"
sudo xid8300-setup --model XID8100 192.168.1.50    # XID8100 (same protocol) -> queue "XID8100"
```

For an XID8100, use `XID8100` instead of `XID8300` as the queue name in all commands below.

Turn on detailed logging before the first print:

```sh
sudo cupsctl --debug-logging
echo "SetEnv XID8300_TRACE 1" | sudo tee -a /etc/cups/cupsd.conf    # hex dump of printer replies
sudo systemctl restart cups
```

If you can, record the network traffic in a second terminal while testing:

```sh
sudo tcpdump -i any -w ~/xid8300.pcap port 50710
```

## 1. Talk to the printer without printing (optional, but safest)

Insert one card, then print test 1 and watch the queue:

```sh
lp -d XID8300 /usr/share/printer-driver-xid8300/testcards/01-orientation-colours.ps
lpstat -p XID8300 -l
```

If the job stops or the queue is paused, run `sudo grep -E "xid8300|\[Job" /var/log/cups/error_log | tail -80`
and keep the output.

## 2. The test cards, in order

| # | Command | What to check |
|---|---|---|
| 1 | `lp -d XID8300 .../01-orientation-colours.ps` | The marker is **top-left**. The bars read C M Y R G B K in the right colours, and the grey ramp is smooth. |
| 2 | `lp -d XID8300 .../02-yellow-only.ps` | The rectangle is **yellow**. If it is **cyan**, the colour planes are swapped: tell me and I will fix it. |
| 3 | `lp -d XID8300 -o Duplex=DuplexNoTumble .../03-duplex-front-back.ps` | One card, FRONT on one side and BACK on the other. When turned over along the long edge, the BACK marker is top-left. |
| 4 | `lp -d XID8300 .../04-black-text.ps` | The text is sharp black resin (K panel), with no colour fringes. |
| 5 | `lp -d XID8300 .../05-full-bleed.ps` | Blue reaches every edge, and the white lines sit evenly 1 mm and 2 mm from the edges. |

The test cards are in `/usr/share/printer-driver-xid8300/testcards/`.

### If the picture is off-centre

Pages are stretched to the full print area, which is slightly larger than the card, as the Windows driver does.
If test 5 still shows a white strip along one edge, move the image **towards that edge** by the strip's width (held
landscape, marker top-left):

```sh
sudo lpadmin -p XID8300 -o ImageShiftY=-1.25   # strip at the top    (use 1.25 for the bottom)
sudo lpadmin -p XID8300 -o ImageShiftX=-1.25   # strip at the left   (use 1.25 for the right)
```

Values go in 0.25 mm steps up to 3 mm. On a portrait card, its right edge is "top", left is "bottom", top is
"left" and bottom is "right". Please report the values that worked.

### Printer properties

The Windows driver's preferences are under the printer's **Printer Options** (or `http://localhost:631` →
Printers → XID8300 → Set Default Options, or `lpoptions -p XID8300 -l`). "Printer setting" keeps the printer's own
value. To compare with Windows, try one at a time, e.g. `lp -d XID8300 -o KPrintMode=Fine .../04-black-text.ps`.

## 3. Optional features (only after 1–5 work)

```sh
# Magnetic stripe (use a test card, not real data). Read it back with a card reader.
lp -d XID8300 -o mag-track2=1234567890 .../01-orientation-colours.ps
# Peel-off area (needs a YMCK-PO ribbon)
lp -d XID8300 -o area1=front:peeloff:20,8,12,10mm .../05-full-bleed.ps
```

## 4. What to send back

- For each test: OK, or what came out (a phone photo of the card is ideal).
- `/var/log/cups/error_log` from the session (or only the lines containing `[Job`).
- `~/xid8300.pcap`, if you recorded it.
- Anything shown on the printer's panel.

When you are done, turn debugging off again with `sudo cupsctl --no-debug-logging`, remove the `SetEnv` line,
and restart cups.

## Without the printer

The package includes the mock printer used by the test suite. You can use it to check the queue setup:

```sh
python3 /usr/share/printer-driver-xid8300/tools/mock_xid8300.py --port 50710 --out /tmp/xid --sessions 10 &
sudo xid8300-setup 127.0.0.1 XIDTEST
lp -d XIDTEST /usr/share/printer-driver-xid8300/testcards/01-orientation-colours.ps
ls /tmp/xid      # the planes the "printer" received, as PGM images
```
