#!/bin/bash
# Benchmark every TIER1 test: compile + run, report ms
cd "$(dirname "$0")/.."
CC="gcc"
CFLAGS="-O2 -Wall -Wextra -Wno-unused-parameter -Wno-format -I. -Icore -Icore/infra -no-pie"
LDFLAGS="-lm"
BUILD=build
mkdir -p $BUILD

# Extract TIER1 list from Makefile
TESTS=$(sed -n '/^TIER1/,/^[^ \\]/p' Makefile | sed 's/\\//g' | tr ' ' '\n' | grep -E '^[a-z]')

TOTAL=0
PASS=0
FAIL=0
SLOW=0
RESULTS=""

for t in $TESTS; do
  if [ ! -f "tests/$t.c" ]; then
    echo "  ⚠️  $t (source not found)" >&2
    continue
  fi
  
  TOTAL=$((TOTAL+1))
  START=$(date +%s%3N)
  
  # Compile
  $CC $CFLAGS -o $BUILD/test-$t tests/$t.c $LDFLAGS 2>/dev/null
  COMPILE_OK=$?
  
  if [ $COMPILE_OK -ne 0 ]; then
    END=$(date +%s%3N)
    ELAPSED=$((END-START))
    echo "  ❌ $t BUILD_FAIL ${ELAPSED}ms"
    FAIL=$((FAIL+1))
    continue
  fi
  
  # Run
  ./$BUILD/test-$t >/dev/null 2>&1
  RUN_OK=$?
  
  END=$(date +%s%3N)
  ELAPSED=$((END-START))
  
  if [ $RUN_OK -eq 0 ]; then
    PASS=$((PASS+1))
    if [ $ELAPSED -gt 1000 ]; then
      SLOW=$((SLOW+1))
      echo "  🐌 $t ${ELAPSED}ms (>1s SLOW)"
    elif [ $ELAPSED -gt 500 ]; then
      echo "  ⚡ $t ${ELAPSED}ms"
    else
      echo "  ✅ $t ${ELAPSED}ms"
    fi
  else
    FAIL=$((FAIL+1))
    echo "  ❌ $t ${ELAPSED}ms RUN_FAIL"
  fi
done

echo ""
echo "═══════════════════════════════════════"
echo "Total: $TOTAL  PASS: $PASS  FAIL: $FAIL  SLOW(>1s): $SLOW"
