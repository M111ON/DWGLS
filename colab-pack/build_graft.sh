#!/bin/bash
set -e
cd /mnt/i/DWGLS-native-fs
LB=/tmp/llama-b9733
gcc -O2 -Wall \
  -I core -I $LB/include -I $LB/ggml/include \
  -o colab-pack/rid_graft_linux tools/geo_rid_graft.c \
  $LB/build/src/libllama.a \
  $LB/build/ggml/src/libggml.a \
  $LB/build/ggml/src/libggml-base.a \
  $LB/build/ggml/src/libggml-cpu.a \
  -lm -lpthread -ldl -fopenmp -s
echo BUILT
./colab-pack/rid_graft_linux --help 2>/dev/null | head -2 || true
