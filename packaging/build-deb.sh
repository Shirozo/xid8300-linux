#!/bin/sh
# Build printer-driver-xid8300_VERSION_ARCH.deb for the machine it runs on.
# Run on the oldest distribution you want to support (Ubuntu 22.04 covers
# Ubuntu 22.04+ and Debian 12+), e.g. from the project root:
#
#   docker run --rm -v "$PWD":/src:ro -v "$PWD/dist":/out -e OUT=/out ubuntu:22.04 \
#     sh -c 'apt-get update && apt-get install -y build-essential libcups2-dev \
#            libcupsimage2-dev dpkg-dev python3 && sh /src/packaging/build-deb.sh'
#
# The package contains no files from the Windows driver.  Users who want the
# Matica colour table import it from their own copy with xid8300-import.

set -e

VERSION=${VERSION:-0.2.0}
REVISION=${REVISION:-1}
MAINTAINER=${MAINTAINER:-"Shirozo <109609284+Shirozo@users.noreply.github.com>"}
PKG=printer-driver-xid8300
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${OUT:-$ROOT/dist}
ARCH=$(dpkg --print-architecture)
WORK=$(mktemp -d)
STAGE=$WORK/stage

mkdir -p "$WORK/src"
tar -C "$ROOT" --exclude=./tests/out --exclude=./dist --exclude=./.git --exclude='*.dSYM' -cf - . |
    tar -C "$WORK/src" -xf -
cd "$WORK/src"
make clean >/dev/null
make SERVERBIN=/usr/lib/cups DATADIR=/usr/share/cups
make install DESTDIR="$STAGE" SERVERBIN=/usr/lib/cups DATADIR=/usr/share/cups
strip --strip-unneeded "$STAGE/usr/lib/cups/filter/rastertoxid8300" "$STAGE/usr/lib/cups/backend/xid8300"

SHARE=$STAGE/usr/share/$PKG
DOC=$STAGE/usr/share/doc/$PKG
install -d "$SHARE/testcards" "$SHARE/tools" "$DOC" "$STAGE/var/lib/$PKG"
python3 packaging/make-testcards.py "$SHARE/testcards"
install -m 0644 packaging/TESTING.md "$SHARE/testcards/"
install -m 0644 packaging/TESTING.md README.md docs/PROTOCOL.md "$DOC/"
install -m 0755 tests/mock_xid8300.py "$SHARE/tools/"

cat > "$DOC/copyright" <<EOF
Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
Upstream-Name: xid8300 Linux driver

Files: *
Copyright: the xid8300 Linux driver contributors
License: GPL-2.0-or-later
 This program is free software; you can redistribute it and/or modify it under
 the terms of the GNU General Public License as published by the Free Software
 Foundation; either version 2 of the License, or (at your option) any later
 version.
 .
 This program is distributed in the hope that it will be useful, but WITHOUT
 ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more details.
 .
 On Debian systems, the full text of the GNU General Public License version 2
 can be found in /usr/share/common-licenses/GPL-2.

Comment: Not affiliated with or endorsed by Matica Technologies or JVC KENWOOD.
 Product names are used only to identify the supported printers. The package
 contains no files from the manufacturer's Windows driver.
EOF

gzip -9n -c > "$DOC/changelog.gz" <<EOF
$PKG ($VERSION-$REVISION) unstable; urgency=low

  * First public release.

 -- $MAINTAINER  $(date -R)
EOF

# Shared-library dependencies, computed against this build system.
mkdir -p "$WORK/shlib/debian"
printf 'Source: %s\n\nPackage: %s\nArchitecture: any\n' "$PKG" "$PKG" > "$WORK/shlib/debian/control"
DEPS=$(cd "$WORK/shlib" && dpkg-shlibdeps -O \
        -e"$STAGE/usr/lib/cups/filter/rastertoxid8300" -e"$STAGE/usr/lib/cups/backend/xid8300" 2>/dev/null \
        | sed -n 's/^shlibs:Depends=//p')

install -d "$STAGE/DEBIAN"
SIZE=$(du -sk --exclude=DEBIAN "$STAGE" | cut -f1)
cat > "$STAGE/DEBIAN/control" <<EOF
Package: $PKG
Version: $VERSION-$REVISION
Architecture: $ARCH
Maintainer: $MAINTAINER
Installed-Size: $SIZE
Depends: $DEPS, cups-daemon, cups-client, cups-filters, ghostscript, python3
Recommends: cups
Suggests: opensc, pcscd
Section: misc
Priority: optional
Homepage: https://github.com/Shirozo/xid8300-linux
Description: CUPS driver for Matica XID8300/XID8100 retransfer card printers (TCP/IP)
 Raster filter, xid8300:// network backend and PPDs for the Matica XID8300
 and XID8100 card printers, written from the printer protocol.
 Supports YMCK/UV panels, two-sided cards, the Windows driver's printer
 settings and colour adjustment, magnetic stripe encoding, a smart-card
 encoding hook and peel-off areas.
 .
 Not affiliated with Matica or JVC KENWOOD. Experimental: test with blank
 cards first. Includes test cards and a mock printer.
EOF

cat > "$STAGE/DEBIAN/postinst" <<'EOF'
#!/bin/sh
set -e
if [ "$1" = configure ]; then
    # Give existing XID printers the new PPD (CUPS keeps their default options).
    if command -v lpstat >/dev/null 2>&1 && lpstat -r >/dev/null 2>&1; then
        lpstat -v 2>/dev/null | sed -n 's/^device for \([^:]*\): xid8300:.*/\1/p' | while read -r q; do
            model=xid8300
            grep -q '^\*ModelName: "Matica XID8100"' "/etc/cups/ppd/$q.ppd" 2>/dev/null && model=xid8100
            if lpadmin -p "$q" -P "/usr/share/cups/model/$model.ppd" -o print-scaling-default=fill >/dev/null 2>&1; then
                echo "Updated printer $q to the new driver options."
            fi
        done
    fi
    echo "XID8300 driver installed. Create a printer with:  sudo xid8300-setup PRINTER_IP"
    echo "Testing guide: /usr/share/printer-driver-xid8300/testcards/TESTING.md"
fi
EOF
chmod 0755 "$STAGE/DEBIAN/postinst"

cat > "$STAGE/DEBIAN/postrm" <<'EOF'
#!/bin/sh
set -e
# The imported colour table is the user's own copy; remove it only on purge.
if [ "$1" = purge ]; then
    rm -rf /var/lib/printer-driver-xid8300
fi
EOF
chmod 0755 "$STAGE/DEBIAN/postrm"

# Ownership root:root, sane permissions.
find "$STAGE" -type d -exec chmod 0755 {} +
mkdir -p "$OUT"
dpkg-deb --root-owner-group --build "$STAGE" "$OUT/${PKG}_${VERSION}-${REVISION}_${ARCH}.deb"
rm -rf "$WORK"
