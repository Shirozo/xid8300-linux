#!/bin/sh
# Test stand-in for an IC encoding program: records what the backend passed
# and exits with $XID_TEST_IC_EXIT (default 0).
data=$(od -An -tx1 | tr -d ' \n')
echo "$1 card=$XID8300_CARD len=$XID8300_DATA_LENGTH info=$XID8300_ENCODER_INFO printer=$XID8300_PRINTER data=$data" >> "$XID_TEST_IC_LOG"
exit "${XID_TEST_IC_EXIT:-0}"
