#!/usr/bin/env python3
"""Mock Matica XID8300 printer for testing the xid8300 CUPS backend.

Implements the TCP framing from docs/PROTOCOL.md section 3 closely enough to
drive the backend through a whole job.  It is NOT a model of the real
printer: replies to status/property queries carry made-up data.

  mock_xid8300.py [--port N] [--out DIR] [--sessions N] [--busy] [--mag-error]
                  [--fail-settings STATUS:COUNT] [--asleep]

Every frame is logged as one line to stdout; ImageOut planes are saved as
PGM files in DIR (card<N>_<pass>_<plane>.pgm, 1036x664).
"""

import argparse
import os
import socket
import struct
import sys

W, H = 1036, 664
PLANE_NAMES = {0x01: "Y", 0x02: "M", 0x04: "C", 0x08: "K", 0x20: "P86", 0x40: "UV"}
SUBCMDS = {1: "TestUnitReady", 2: "ReadPosition", 3: "RezeroUnit", 4: "LoadCard",
           5: "MoveCard", 6: "PrintOnFilm", 7: "PrintOnCard", 9: "WriteMG",
           10: "ICControl", 11: "SecurityPrint"}
BUSY = {2: 0x2D300, 4: 0x2D400, 5: 0x2D500}


def frame(kind, code, status=0, tag=0, body=b""):
    body = body + b"\0" * (-len(body) % 4)
    words = (8 + len(body)) // 4
    return struct.pack(">BHBIII", kind, code, 0, words, status, tag) + body


def recv_exact(sock, n):
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise EOFError
        buf += chunk
    return buf


def recv_frame(sock):
    hdr = recv_exact(sock, 8)
    words = struct.unpack(">I", hdr[4:8])[0]
    return hdr + recv_exact(sock, words * 4)


class Printer:
    def __init__(self, out, busy, mag_error=False, fail_settings=(0, 0), asleep=False, busy_start=0):
        self.out = out
        self.busy = busy
        self.mag_error = mag_error
        self.fail_status, self.fail_left = fail_settings   # e.g. 0x2DD00 "panel in use"
        self.asleep = asleep                                # power saving until RezeroUnit
        self.busy_start = busy_start   # first N ReadPositions answer "initializing"
        self.card_pos = None      # None = no card in the transport
        self.card = 0
        self.passes = 0
        self.after_film = []      # TUR busy states to report after PrintOnFilm

    def log(self, msg):
        print(msg, flush=True)

    def save_plane(self, plane, side_flag, data):
        name = PLANE_NAMES.get(plane, "%02x" % plane)
        path = os.path.join(self.out, "card%d_pass%d_%s%s.pgm" % (
            self.card, self.passes + 1, name, "_second" if side_flag else ""))
        with open(path, "wb") as f:
            f.write(b"P5\n%d %d\n255\n" % (W, H))
            f.write(data)

    def handle_command(self, f):
        code = struct.unpack(">H", f[1:3])[0]
        flags = f[11]
        tag = struct.unpack(">I", f[12:16])[0]

        if code == 1:
            sub, ln, p1, p2 = f[16], f[17], f[18], f[19]
            if sub == 9:
                return self.write_mg(f, tag)
            name = SUBCMDS.get(sub, "sub%02x" % sub)
            extra = " arg=%02x" % f[20] if len(f) > 20 else ""
            if sub == 10:
                extra = " at=%s" % self.card_pos
            self.log("CMD %s flags=%02x p1=%02x p2=%02x%s tag=%d" % (name, flags, p1, p2, extra, tag))
            if sub == 1:
                if self.after_film:
                    state = self.after_film.pop(0)
                    return frame(0xF1, 2, BUSY[state], tag)
                return frame(0xF1, 4, 0, tag)
            if sub == 2:
                if self.busy_start > 0:                     # still finishing the last card
                    self.busy_start -= 1
                    return frame(0xF1, 2, 0x02DB00, tag)    # "the printer is initializing"
                if self.card_pos is None:
                    body = bytes([1, 0, 0x04, 0])
                else:
                    body = bytes([1, 0, 0x00, self.card_pos])
                return frame(0xF1, 3, 0, tag, body)
            if sub == 3:
                self.asleep = False
                return frame(0xF1, 4, 0, tag)
            if sub in (4, 5) and self.asleep:
                return frame(0xF1, 2, 0x2FD00, tag)
            if sub == 4:
                self.card += 1
                self.passes = 0
                self.card_pos = p2
            elif sub == 5:
                self.card_pos = None if p2 == 4 else p2
            elif sub in (6, 11):
                self.passes += 1
                if self.busy:
                    self.after_film = [5, 4, 2]
            return frame(0xF1, 4, 0, tag)

        if code == 2:
            plane, side = f[16], f[17]
            rest = struct.unpack(">I", f[20:24])[0]
            dlen = struct.unpack(">I", f[24:28])[0]
            ok = rest == len(f) - 0x18 and dlen <= len(f) - 28
            self.log("CMD ImageOut plane=%s second=%d len=%d frame=%d %s tag=%d" % (
                PLANE_NAMES.get(plane, hex(plane)), 1 if side else 0, dlen, len(f),
                "ok" if ok else "BAD-LENGTH", tag))
            if dlen == W * H:
                self.save_plane(plane, side, f[28:28 + dlen])
            return frame(0xF1, 4, 0 if ok else 1, tag)

        if code in (5, 6):
            ids = [f[i] for i in range(16, len(f), 4) if f[i]]
            self.log("CMD %s ids=%s flags=%02x tag=%d" % (
                "PropertyReq" if code == 5 else "StatusReq", ",".join("%02x" % i for i in ids), flags, tag))
            if ids == [0x0D] and code == 5:
                body = bytes([0x0D, 10, 8]) + b"MOCK8300" + b"\0"   # 12 bytes after padding
                return frame(0xF1, 5, 0, tag, body[:12])
            body = b"".join(bytes([i, 2, 1, 0]) for i in ids)
            return frame(0xF1, code, 0, tag, body)

        if code == 10:
            ids = [f[i] for i in range(16, len(f), 4) if f[i]]
            self.log("CMD SettingReq ids=%s flags=%02x tag=%d" % (",".join("%02x" % i for i in ids), flags, tag))
            info = b"ENC0123456"                      # 10 bytes, TLV {01, 14, 10, data, pad}
            return frame(0xF1, 4, 0, tag, bytes([0x01, 14, 10]) + info + b"\0\0\0")

        if code == 8:
            items = f[16:]
            vals = " ".join("%s=%d" % (chr(items[i]) if 0x20 < items[i] < 0x7f else "%02x" % items[i], items[i + 3])
                            for i in range(0, len(items) - 3, 4) if items[i])
            self.log("CMD SettingValue %d bytes flags=%02x tag=%d items=[%s]" % (len(items), flags, tag, vals))
            if self.fail_left > 0:
                self.fail_left -= 1
                return frame(0xF1, 2, self.fail_status, tag)
            return frame(0xF1, 4, 0, tag)

        self.log("CMD unknown code %d" % code)
        return frame(0xF1, 2, 0x123456, tag)

    def write_mg(self, f, tag):
        n, off, blocks = f[19], 0x14, []
        ok = f[17] == len(f) - 0x12
        for _ in range(n):
            fmt, ln = f[off], f[off + 1]
            raw = f[off + 2:off + 2 + ln]
            if fmt in (0x07, 0x17, 0x37):
                text = bytes(raw).decode("ascii", "replace")
            elif fmt in (0x24, 0x34):
                text = "".join(chr(0x30 + b) for b in raw)
            else:
                text = "".join(chr(0x20 + b) for b in raw)
            blocks.append("%02x:%s" % (fmt, text))
            off += 2 + ln
        self.log("CMD WriteMG at=%s blocks=%d %s %s tag=%d" % (
            self.card_pos, n, "|".join(blocks), "ok" if ok else "BAD-LENGTH", tag))
        if self.mag_error:
            return frame(0xF1, 2, 0x123400, tag)
        return frame(0xF1, 4, 0, tag)

    def session(self, conn):
        greeting = b"MOCK XID8300".ljust(0x68, b"\0")
        conn.sendall(frame(0xF3, 2, 0, 0x12345678, greeting))
        sent_wait = False
        while True:
            try:
                f = recv_frame(conn)
            except EOFError:
                self.log("CLOSE")
                return
            kind, code = f[0], struct.unpack(">H", f[1:3])[0]
            if kind == 0xF2 and code == 3:
                user = f[0x1C:0x3C].decode("utf-16le").rstrip("\0")
                doc = f[0x3C:0x7C].decode("utf-16le").rstrip("\0")
                self.log("SESSION StartJob len=%d user=%r doc=%r date=%s" % (
                    len(f), user, doc, list(f[0x14:0x1A])))
                if self.busy and not sent_wait:
                    conn.sendall(frame(0xF3, 3))
                    sent_wait = True
                else:
                    conn.sendall(frame(0xF3, 4))
            elif kind == 0xF2 and code == 1:
                self.log("SESSION KeepAlive")
                conn.sendall(frame(0xF3, 4))
            elif kind == 0xF0:
                conn.sendall(self.handle_command(f))
            else:
                self.log("UNKNOWN frame %02x/%d" % (kind, code))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=50710)
    ap.add_argument("--out", default=".")
    ap.add_argument("--sessions", type=int, default=1)
    ap.add_argument("--busy", action="store_true", help="exercise wait/busy paths")
    ap.add_argument("--mag-error", action="store_true", help="fail every WriteMG")
    ap.add_argument("--fail-settings", default="0:0",
                    help="STATUS:COUNT - answer the first COUNT SettingValue commands with STATUS")
    ap.add_argument("--asleep", action="store_true", help="start in power saving (needs RezeroUnit)")
    ap.add_argument("--busy-start", type=int, default=0,
                    help="first N ReadPositions answer 'initializing' (printer still finishing the last card)")
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", args.port))
    srv.listen(1)
    print("LISTEN %d" % args.port, flush=True)

    st, cnt = args.fail_settings.split(":")
    printer = Printer(args.out, args.busy, args.mag_error, (int(st, 0), int(cnt)), args.asleep, args.busy_start)
    for _ in range(args.sessions):
        conn, _ = srv.accept()
        with conn:
            printer.session(conn)
    srv.close()


if __name__ == "__main__":
    sys.exit(main())
