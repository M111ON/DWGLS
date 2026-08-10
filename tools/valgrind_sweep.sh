#!/bin/bash
# Valgrind memcheck sweep — DWGSL core on WSL Linux (gcc 11.4)
# Note: builds WITHOUT sanitizer flags (ASAN and valgrind are mutually exclusive)
set -u
cd /mnt/i/DWGLS || exit 2
mkdir -p build/vg_wsl
FAIL=0
run_vg() {
  local name="$1"; shift
  gcc -O1 -g -I. -Icore -o build/vg_wsl/$name "$@" -lm 2>build/vg_wsl/$name.build.log
  if [ ! -x build/vg_wsl/$name ]; then echo "== $name: BUILD FAIL"; head -3 build/vg_wsl/$name.build.log; FAIL=1; return; fi
  valgrind --error-exitcode=99 --leak-check=full --show-leak-kinds=all \
           --track-origins=yes ./build/vg_wsl/$name >build/vg_wsl/$name.out 2>&1
  rc=$?
  if [ $rc -ne 0 ]; then
    echo "== $name: VALGRIND FAIL (rc=$rc)"
    grep -E "Invalid |uninitialised|definitely lost|indirectly lost|possibly lost|ERROR SUMMARY" build/vg_wsl/$name.out | head -10
    FAIL=1
  else
    echo "== $name: CLEAN — $(grep -E 'RESULT|PASS /|STABILITY' build/vg_wsl/$name.out | tail -1 | tr -d '\r') | $(grep 'ERROR SUMMARY' build/vg_wsl/$name.out | tr -d '\r')"
  fi
}
run_vg test_bfs_persist    tests/test_bfs_persist.c
run_vg test_bfs_stability  tests/test_bfs_stability.c
run_vg test_bfs_seek_anchor tests/test_bfs_seek_anchor.c
run_vg test_bfs_breath     tests/test_bfs_breath.c
run_vg test_geo_bfs_hub    tests/test_geo_bfs_hub.c
run_vg stress_monitor      tools/stress_monitor.c
echo "----"
[ $FAIL -eq 0 ] && echo "VALGRIND SWEEP: ALL CLEAN" || echo "VALGRIND SWEEP: FAILURES PRESENT"
exit $FAIL