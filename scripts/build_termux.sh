#!/bin/bash
# build_termux.sh — DWGLS ARM build for Termux
# Usage: bash build_termux.sh [test|bench|all]
set -e

MODE="${1:-all}"
CC="${CC:-gcc}"
CFLAGS="-O2 -Wall -Wextra -Wno-unused-parameter -Wno-format -I. -Icore -Icore/infra"
LDFLAGS="-lm"
BUILD="build_arm"

mkdir -p "$BUILD"

# ── Core tests (self-contained, no Windows deps) ──
CORE_TESTS="
  test_tess_sacred
  test_tess_index_frame
  test_tess_scale_log
  test_tess_frame_seek
  test_tess_magnify
  test_tess_hex_delta
  test_tess_tetra_axis
  test_tess_subdivide
  test_tess_full_cycle
  test_tess_belt
  test_tess_header
  test_tess_stream
  test_hex_quad_dual
  test_hex_quad_dual_upgrades
  test_cube_addr
  test_cell_classify
  test_geo_hyperbolic
  test_geo_fs
  test_geo_sync_bridge
  test_kis_cube_views
  test_fibo_walk
  test_fibo_dual_rail
  test_scale_bridge
  test_6ico_tesseract
  test_18tes_field
  test_twin_rebalance
  test_d4_linesum_bridge
"

# ── Bench tools (no Windows deps) ──
BENCH_TOOLS="
  tools/bench_unified.c
  tools/bench_cache.c
  tools/bench_geo_fast.c
"

# ── Pure-integer geometry probes ──
PROBE_TOOLS="
  tools/hyp_candidate_map.c
  tools/cube_cylinder_probe.c
  tools/bond_direct_resolve.c
  tools/bond_tetris_probe.c
"

compile_test() {
  local t="$1"
  if [ -f "tests/$t.c" ]; then
    echo -n "  $t ... "
    if $CC $CFLAGS -o "$BUILD/test-$t" "tests/$t.c" $LDFLAGS 2>/dev/null; then
      echo "OK"
      return 0
    else
      echo "BUILD FAIL"
      return 1
    fi
  fi
}

compile_tool() {
  local src="$1"
  local name=$(basename "$src" .c)
  echo -n "  $name ... "
  if $CC $CFLAGS -o "$BUILD/$name" "$src" $LDFLAGS 2>/dev/null; then
    echo "OK"
    return 0
  else
    echo "BUILD FAIL"
    return 1
  fi
}

run_test() {
  local t="$1"
  if [ -x "$BUILD/test-$t" ]; then
    echo -n "  $t ... "
    if "./$BUILD/test-$t" >/dev/null 2>&1; then
      echo "PASS"
      return 0
    else
      echo "FAIL"
      return 1
    fi
  fi
}

echo "══ DWGLS ARM Build ($MODE) ══"
echo "Compiler: $CC"
$CC --version 2>/dev/null | head -1 || true
echo ""

case "$MODE" in
  test)
    echo "▶ Compiling tests..."
    pass=0; fail=0
    for t in $CORE_TESTS; do
      if compile_test "$t"; then
        pass=$((pass+1))
      else
        fail=$((fail+1))
      fi
    done
    echo ""
    echo "▶ Running tests..."
    rp=0; rf=0
    for t in $CORE_TESTS; do
      if run_test "$t"; then
        rp=$((rp+1))
      else
        rf=$((rf+1))
      fi
    done
    echo ""
    echo "══ Build: $pass/$((pass+fail)) OK, Run: $rp/$((rp+rf)) PASS ══"
    ;;
  bench)
    echo "▶ Compiling bench tools..."
    for src in $BENCH_TOOLS; do
      compile_tool "$src" || true
    done
    echo ""
    echo "▶ Compiling probes..."
    for src in $PROBE_TOOLS; do
      compile_tool "$src" || true
    done
    ;;
  all)
    echo "▶ Compiling tests..."
    pass=0; fail=0
    for t in $CORE_TESTS; do
      if compile_test "$t"; then
        pass=$((pass+1))
      else
        fail=$((fail+1))
      fi
    done
    echo ""
    echo "▶ Compiling bench tools..."
    for src in $BENCH_TOOLS; do
      compile_tool "$src" || true
    done
    for src in $PROBE_TOOLS; do
      compile_tool "$src" || true
    done
    echo ""
    echo "▶ Running tests..."
    rp=0; rf=0
    for t in $CORE_TESTS; do
      if run_test "$t"; then
        rp=$((rp+1))
      else
        rf=$((rf+1))
      fi
    done
    echo ""
    echo "══ Build: $pass/$((pass+fail)) OK, Run: $rp/$((rp+rf)) PASS ══"
    ;;
esac

echo ""
echo "Binaries in: $BUILD/"
ls -la "$BUILD/" 2>/dev/null | grep -v "^total" | grep -v "^d" | head -20
