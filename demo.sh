#!/bin/sh
# demo.sh - scripted demonstration of the R7023E OBDH model.
#
# Starts PF and PL, then drives the ground console through a nominal
# run plus two deliberate rejections. Everything is printed to one
# terminal, prefixed by which process produced it, so the whole thing
# can be screen-recorded in a single window.

set -e
cd "$(dirname "$0")"

rm -f pl_images.log gnd_tm.log
pkill -f './pf' 2>/dev/null || true
pkill -f './pl' 2>/dev/null || true
sleep 0.3

./pf & PF_PID=$!
sleep 0.5
./pl & PL_PID=$!
sleep 1

{
  echo "# --- a time tag before the clock is set must be rejected"
  echo "image @23:59:00";           sleep 1
  echo "# --- set the onboard clock from the ground computer"
  echo "obt";                       sleep 1
  echo "# --- a payload command in SAFE must be rejected"
  echo "power ON";                  sleep 1
  echo "# --- go to TARGET, which takes 8 seconds"
  echo "mode TARGET";               sleep 11
  echo "# --- now the payload can be powered"
  echo "power ON";                  sleep 3
  echo "# --- an immediate capture, 5 seconds"
  echo "image";                     sleep 8
  echo "# --- a time tagged capture, scheduled 10 seconds ahead"
  echo "image @+10";                sleep 16
  echo "# --- a typo must be rejected, not ignored"
  echo "typo";                      sleep 2
  echo "# --- back to SAFE forces the payload off automatically"
  echo "mode SAFE";                 sleep 11
  echo "quit"
} | ./gnd_stub

kill $PF_PID $PL_PID 2>/dev/null || true
wait 2>/dev/null || true

echo
echo "================ payload image log ================"
cat pl_images.log 2>/dev/null || echo "(none)"
echo "==================================================="
