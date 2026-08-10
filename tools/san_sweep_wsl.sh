#!/bin/bash
# ASAN+UBSAN sweep — DWGLS core on WSL Linux (gcc 11.4, full runtimes)
set -u
cd /mnt/i/DWGLS || exit 2
mkdir -p build/san_wsl
PASS=0; FAIL=0
for t in test_bfs_persist test_bfs_stability test_bfs_seek_anchor test_bfs_breath test_geo_bfs_hub; do
  gcc -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
      -I. -Icore -o build/san_wsl/$t tests/$t.c -lm 2>build/san_wsl/$t.build.log
  if [ ! -x build/san_wsl/$t ]; then echo "== $t: BUILD FAIL"; head -5 build/san_wsl/$t.build.log; FAIL=1; continue; fi
  ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
    ./build/san_wsl/$t >build/san_wsl/$t.out 2>&1
  rc=$?
  if [ $rc -ne 0 ]; then
    echo "== $t: SANITIZER FAIL (rc=$rc)"
    grep -E "ERROR|runtime error|SUMMARY|AddressSanitizer|LeakSanitizer" build/san_wsl/$t.out | head -8
    FAIL=1
  else
    echo "== $t: CLEAN — $(grep -E 'RESULT|PASS /' build/san_wsl/$t.out | tail -1 | tr -d '\r')"
    PASS=1
  fi
done
# stress monitor under ASAN (50 rounds, all invariants)
gcc -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I. -Icore -o build/san_wsl/stress_monitor tools/stress_monitor.c -lm 2>build/san_wsl/stress.build.log
if [ -x build/san_wsl/stress_monitor ]; then
  ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
    ./build/san_wsl/stress_monitor 50 >build/san_wsl/stress.out 2>&1
  rc=$?
  if [ $rc -ne 0 ]; then echo "== stress_monitor: SANITIZER FAIL (rc=$rc)"; grep -E "ERROR|runtime error|SUMMARY" build/san_wsl/stress.out | head -8; FAIL=1
  else echo "== stress_monitor 50r: CLEAN — $(tail -1 build/san_wsl/stress.out | tr -d '\r')"; fi
else echo "== stress_monitor: BUILD FAIL"; head -5 build/san_wsl/stress.build.log; FAIL=1; fi
echo "----"
[ $FAIL -eq 0 ] && echo "SANITIZER SWEEP: ALL CLEAN" || echo "SANITIZER SWEEP: FAILURES PRESENT"
exit $FAIL