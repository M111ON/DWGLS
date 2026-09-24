#!/bin/bash
# run_gpu_benches.sh — build + run the 3 DWGLS GPU benches on Linux (Colab T4 etc.)
# Usage:  bash scripts/run_gpu_benches.sh [ARCH]
#   ARCH default sm_75 (T4).  sm_61=1050Ti, sm_70=V100, sm_80=A100, sm_89=L4/H100
# Run from repo root:  git clone https://github.com/M111ON/DWGLS.git && cd DWGLS
set -e

ARCH="${1:-sm_75}"
NVCC="${NVCC:-nvcc}"
mkdir -p build

command -v "$NVCC" >/dev/null || { echo "FATAL: nvcc not found (Colab: /usr/local/cuda/bin)"; exit 2; }

echo "== GPU =="
nvidia-smi --query-gpu=name,compute_cap,driver_version --format=csv,noheader || true
echo "== nvcc ($ARCH fatbin: native + PTX forward-compat) =="
"$NVCC" --version | tail -1

FLAGS="-O2 -gencode arch=${ARCH},code=${ARCH} -gencode arch=${ARCH},code=compute_${ARCH#sm_}"
# also embed PTX of a lower baseline when targeting sm_75+ so older tools can JIT
# (keep it simple: native SASS + its own PTX)

for f in gpu_launch_bench gpu_batch_break_even gpu_bandwidth_bench; do
    echo "== build $f =="
    "$NVCC" $FLAGS -Icore -Icore/infra "bench/$f.cu" -o "build/$f"
done

fail=0
for f in gpu_launch_bench gpu_batch_break_even gpu_bandwidth_bench; do
    echo
    echo "════════════ run $f ════════════"
    if ! "./build/$f"; then fail=1; fi
done

echo
if [ $fail -eq 0 ]; then echo "ALL_BENCH_OK"; else echo "SOME_BENCH_FAILED"; fi
exit $fail
