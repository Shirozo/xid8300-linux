#!/bin/sh
# Example smart-card encoding program for the xid8300 backend (UNTESTED).
#
# The backend runs   <program> contact|contactless   after it has moved the card
# to the station and engaged it, with the job's ic-contact / ic-contactless data
# on stdin (docs/PROTOCOL.md §7).  This example treats that data as a list of APDUs,
# one hex string per line ("#" starts a comment), and sends them with
# opensc-tool (OpenSC) through the PC/SC reader that sits in the printer.
#
# Install:  copy to /usr/lib/cups/xid8300/ic-contact and/or ic-contactless, chmod 755.
# The reader can be chosen with XID8300_READER (opensc-tool reader number);
# set it with "SetEnv XID8300_READER 1" in cups-files.conf / cupsd.conf.
# The CUPS backend runs as user lp: make sure lp may use pcscd (polkit rules).
#
# Exit codes understood by the backend:
#   0 = encoded, 10 = reject card (eject, job fails), 11 = cancel job, other = error.

unit=$1
reader=${XID8300_READER:-0}

echo "INFO: $unit encoder: card $XID8300_CARD, $XID8300_DATA_LENGTH bytes of data" >&2

args=""
while IFS= read -r line || [ -n "$line" ]; do
    line=$(printf '%s' "$line" | sed 's/#.*//; s/[[:space:]]//g')
    [ -z "$line" ] && continue
    case $line in
        *[!0-9A-Fa-f:]*) echo "ERROR: invalid APDU line" >&2; exit 11 ;;
    esac
    args="$args -s $line"
done

[ -z "$args" ] && exit 0

# -w waits for the card to be presented to the reader.
out=$(opensc-tool -r "$reader" -w $args 2>&1) || { echo "ERROR: opensc-tool failed" >&2; exit 1; }

# Every response must end with SW 90 00.
bad=$(printf '%s\n' "$out" | grep 'Received (SW1=' | grep -vc 'SW1=0x90, SW2=0x00')
if [ "$bad" -ne 0 ]; then
    echo "ERROR: card returned an error status" >&2
    exit 10
fi
exit 0
