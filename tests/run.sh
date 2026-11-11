#!/bin/sh
# End-to-end test: raster -> rastertoxid8300 -> xid8300 backend -> mock printer.
# Run from the project root via "make check".

set -e

ROOT=$(pwd)
OUT=$ROOT/tests/out
PORT=${XID_TEST_PORT:-55710}
# Optional: the Matica LUT24 file from the Windows driver (not part of this
# project).  Without it the LUT test is skipped.
LUT=${XID_LUT:-/var/lib/printer-driver-xid8300/LUT24.LUT}
FAIL=0

rm -rf "$OUT"
mkdir -p "$OUT"

# PPD copy pointing at the LUT, if there is one.
sed -e "s|^\*xidLUTFile:.*|*xidLUTFile: \"$LUT\"|" ppd/xid8300.ppd > "$OUT/xid8300.ppd"

run_case() {
    name=$1 pages=$2 opts=$3 dflag=$4 mockflags=$5 expect=${6:-0}
    icexit=${7:-0} iccontact=${8:-$ROOT/tests/ic_encoder.sh}
    dir=$OUT/$name
    mkdir -p "$dir"

    ./tests/mkraster $dflag "$dir/in.ras" "$pages"
    PPD="$OUT/xid8300.ppd" ./rastertoxid8300 1 tester "Test $name" 1 "$opts" "$dir/in.ras" \
        > "$dir/job.xid" 2> "$dir/filter.log"

    python3 -I "$ROOT/tests/mock_xid8300.py" --port "$PORT" --out "$dir" $mockflags \
        > "$dir/mock.log" 2>&1 &
    MOCK=$!
    sleep 1

    if env DEVICE_URI="xid8300://127.0.0.1:$PORT" XID_TEST_IC_LOG="$dir/ic.log" \
       XID_TEST_IC_EXIT="$icexit" XID8300_IC_CONTACT="$iccontact" \
       XID8300_IC_CONTACTLESS="$ROOT/tests/ic_encoder.sh" \
       ./xid8300 1 tester "Test $name" 1 "" "$dir/job.xid" 2> "$dir/backend.log"; then
        rc=0
    else
        rc=$?
    fi
    if [ "$rc" = "$expect" ]; then
        echo "ok   $name (backend exit $rc)"
    else
        echo "FAIL $name (backend exit $rc, expected $expect)"; FAIL=1
    fi
    wait $MOCK || true
}

run_case simplex     L   ""                       ""   ""
run_case portrait    P   ""                       ""   ""
run_case duplex      LL  ""                       -d   ""
run_case uv          LU  "FrontPanels=YMCKUV"     ""   ""
run_case konly       L   "FrontPanels=K MonoMethod=Dither" "" ""
run_case busy        L   "ColorCorrection=None"   ""   --busy
run_case twocards    LL  ""                       ""   "--sessions 1"
run_case mag         L   "mag-track1=%B4111111111111111^DOE/JOHN^2912101? mag-track2=;4111111111111111=2912101? mag-track3=0123456789" "" ""
run_case magflip     LL  "mag-track2=1234567890 MagCardFlip=True" -d ""
run_case magjis      L   "mag-jis2=JIS-TEST-0001 mag-track1=HELLO mag-track1-format=7bit" "" ""
run_case magerror    L   "mag-track2=1234" "" --mag-error 4
run_case iccontact   L   "ic-contact=hello-chip" "" ""
run_case icall       L   "ic-contactless=hex:00A4040007A0000000041010 ic-contact=X mag-track2=1234 ICCardFlip=True" "" ""
run_case iccancel    L   "ic-contact=cancel-me"   ""   ""   5 11
run_case icreject    L   "ic-contact=reject-me"   ""   ""   1 10
run_case icerror     L   "ic-contact=fail-me"     ""   ""   4 3
run_case icmissing   L   "ic-contact=x"           ""   ""   4 0 /nonexistent/ic-contact

run_case panelbusy   L   ""                       ""   "--fail-settings 0x2DD00:3"
run_case jobstartbusy L  ""                       ""   "--busy-start 2"
run_case sleeping    L   ""                       ""   "--asleep"
run_case jammed      L   ""                       ""   "--fail-settings 0x39300:1" 4
run_case duplexopt   LL  "Duplex=DuplexNoTumble"  ""   ""
run_case peeloff     L   "area1=front:peeloff:100,50,200,40 area2=front:noprint:600,300,100,100" "" ""
run_case peelback    LL  "area1=back:peeloff:100,50,200,40" -d ""
run_case peelpor     P   "area1=front:peeloff:100,50,200,40" "" ""
run_case fill        T   ""                       ""   ""
run_case actual      T   "CardFit=Actual"         ""   ""
run_case shift       L   "ImageShiftX=1.00 ImageShiftY=-1.00" "" ""
run_case konlyext    L   "BlackMode=KOnly"        ""   ""
if [ -f "$LUT" ]; then
    run_case lut     L   "ColorCorrection=LUT"    ""   ""
else
    echo "skip lut (no LUT24 file; set XID_LUT=/path/to/LUT24_xxx.LUT to run it)"
fi
run_case psettings   L   "YMCLevel=2 KLevel=-1 KPrintMode=Fine BackCooling=Disable RetransferTemp=1 RetransferSpeedFront=-3 UVSpeedBack=4 BendRemedyTemp=Disable LaminateMode=PassThrough StandbyMode=BackWait" "" ""
run_case magset      L   "mag-track2=1234 MagWriteRetry=2 MagCoercivity=HiCo EncodeBeforePrint=True" "" ""
run_case coloradj    L   "Brightness=50 GammaRed=2.0 HueBlue=-30" "" ""
run_case backfirst   LL  "BackSideFirst=True TurnOverAfterLoad=True" -d ""
run_case peelmm      L   "area1=front:noprint:10,5,20,10mm peeloff-levels=200,150,100 area2=front:peeloff:0,0,50,50" "" ""

# Peel-off with UV is a ribbon error: the filter refuses it ...
mkdir -p "$OUT/peeluv"
./tests/mkraster "$OUT/peeluv/in.ras" LU
if PPD="$OUT/xid8300.ppd" ./rastertoxid8300 1 tester bad 1 "FrontPanels=YMCKUV area1=front:peeloff:10,10,50,50" \
     "$OUT/peeluv/in.ras" > /dev/null 2> "$OUT/peeluv/filter.log"; then
    echo "FAIL peeluv (filter accepted peel-off + UV)"; FAIL=1
else
    echo "ok   peeluv (filter rejected peel-off + UV)"
fi

# ... and so does the backend if such a stream arrives from elsewhere.
mkdir -p "$OUT/ribbon"
python3 -I - "$OUT/ribbon/job.xid" <<'PY'
import struct, sys
N = 1036 * 664
def rec(t, data=b"", param=0):
    return struct.pack("<5I", t, len(data), 0, 0, param) + data
job = struct.pack("<I", 1) + bytes([0x10]) + bytes(15)
out = rec(0x01, job) + rec(0x84, b"\xff" * N) + rec(0x87, b"\xff" * N, 0x102) + rec(0x86, b"\xe1" * N) + rec(0xF3) + rec(0xF5)
open(sys.argv[1], "wb").write(out)
PY
python3 -I "$ROOT/tests/mock_xid8300.py" --port "$PORT" --out "$OUT/ribbon" > "$OUT/ribbon/mock.log" 2>&1 &
MOCK=$!
sleep 1
if DEVICE_URI="xid8300://127.0.0.1:$PORT" ./xid8300 1 tester ribbon 1 "" "$OUT/ribbon/job.xid" 2> "$OUT/ribbon/backend.log"; then
    rc=0
else
    rc=$?
fi
wait $MOCK || true
if [ $rc = 4 ] && grep -q "Ink ribbon specification" "$OUT/ribbon/backend.log"; then
    echo "ok   ribbon (backend rejected 0x86 + UV with the driver's error)"
else
    echo "FAIL ribbon (backend exit $rc)"; FAIL=1
fi

# A K-secure (0x88) record arriving from elsewhere is printed with SecurityPrint
# (this filter does not create one; the backend still handles the protocol).
mkdir -p "$OUT/ksflow"
python3 -I - "$OUT/ksflow/job.xid" <<'PY'
import struct, sys
N = 1036 * 664
def rec(t, data=b"", param=0):
    return struct.pack("<5I", t, len(data), 0, 0, param) + data
job = struct.pack("<I", 1) + bytes([0x10]) + bytes(15)
k = bytes(N // 2) + b"\xff" * (N - N // 2)
out = rec(0x01, job) + rec(0x84, k) + rec(0x88, k[::-1]) + rec(0xF3) + rec(0xF5)
open(sys.argv[1], "wb").write(out)
PY
python3 -I "$ROOT/tests/mock_xid8300.py" --port "$PORT" --out "$OUT/ksflow" > "$OUT/ksflow/mock.log" 2>&1 &
MOCK=$!
sleep 1
if DEVICE_URI="xid8300://127.0.0.1:$PORT" ./xid8300 1 tester ksflow 1 "" "$OUT/ksflow/job.xid" 2> "$OUT/ksflow/backend.log"; then
    echo "ok   ksflow (backend exit 0)"
else
    echo "FAIL ksflow (backend exit $?)"; FAIL=1
fi
wait $MOCK || true

python3 -I "$ROOT/tests/golden.py" ./tests/areacheck "$ROOT/tests/golden.sha256" || FAIL=1

# Invalid track data must fail in the filter without echoing the data.
mkdir -p "$OUT/magbad"
./tests/mkraster "$OUT/magbad/in.ras" L
if PPD="$OUT/xid8300.ppd" ./rastertoxid8300 1 tester bad 1 "mag-track1=lowercase-secret" \
     "$OUT/magbad/in.ras" > /dev/null 2> "$OUT/magbad/filter.log"; then
    echo "FAIL magbad (filter accepted invalid track 1)"; FAIL=1
elif grep -q secret "$OUT/magbad/filter.log"; then
    echo "FAIL magbad (track data leaked into log)"; FAIL=1
else
    echo "ok   magbad (rejected, data not logged)"
fi

python3 -I "$ROOT/tests/verify.py" "$OUT" || FAIL=1

if [ $FAIL -ne 0 ]; then
    echo "Some tests FAILED - see $OUT"
    exit 1
fi
echo "All tests passed."
