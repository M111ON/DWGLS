#!/bin/bash
set -e
cd /mnt/i/DWGLS-native-fs
mkdir -p colab-pack
for t in p2_view_perf layer_manifest_probe hexagram_cubes_probe zeckendorf_probe hosoya_view_probe circle_config_probe pascal_zigzag_probe; do
  if gcc -static -O2 -Wall -I core -o colab-pack/$t tools/$t.c -lm -s 2>err_$t.txt; then
    echo "OK  $t"
  else
    echo "ERR $t"
    head -3 err_$t.txt
  fi
done
ls -la colab-pack/
