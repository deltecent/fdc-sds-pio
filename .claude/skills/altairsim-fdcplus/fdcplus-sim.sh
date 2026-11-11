#!/bin/sh
# usage: fdcplus-sim.sh <baud>  -- run a CPU polling a simulated FDC+ on the 9KFH link
exec altairsim default -x "boards remove dsk0" -x "boards add fdcplus fdc0" \
  -x "set fdc0 baud=${1:-403200}" \
  -x "connect fdc0:line serial:/dev/cu.usbserial-AL009KFH" -x "connect sio0:a null" \
  -x "deposit 0 db 08 c3 00 00" -x "run 0"
