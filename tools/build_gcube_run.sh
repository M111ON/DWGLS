#!/bin/bash
# build_gcube_run.sh — compile gcube_token_run against WSL llama.cpp build
set -e
cd /mnt/i/DWGLS
gcc -O2 -Wall -Wextra -o /tmp/gcube_run tools/gcube_token_run.c \
    -I/root/llama.cpp/ggml/include \
    -I/root/llama.cpp/include \
    -I/mnt/i/DWGLS/core \
    -L/root/llama.cpp/build/bin \
    -lllama -lggml-base \
    -Wl,-rpath,/root/llama.cpp/build/bin
echo "BUILD OK"