#!/bin/bash
# usage: ./ann_colab_bench.sh [nq] [reps]
NQ=${1:-1000}; REPS=${2:-5}
chmod +x ann_base ann_fused
for bin in ann_base ann_fused; do
  for m in 0 1 2 3 4 5; do
    for r in $(seq 1 $REPS); do
      ./$bin siftsmall_base.fvecs siftsmall_query.fvecs siftsmall_groundtruth.ivecs --k 64 --topb 4 --nq $NQ --mode $m 2>&1 | grep -E "mode=|us/q"
    done
  done
done
