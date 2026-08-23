#!/bin/bash
# Build llama.cpp CUDA ONCE in Colab (detached background build + short polls).
# Survives local disconnects: build runs nohup-style inside the VM; we poll.
# Run from Windows: wsl -d Geomatt -- bash /mnt/i/DWGLS-native-fs/colab-pack/build_cuda_once.sh
set -e
export PATH="$HOME/.local/bin:$PATH"
S=dwgls-cuda-build
VER=b10588
ARCH=75   # T4 only (fastest); add ;80;89 later if needed
ART_REMOTE=/content/llama-cuda-${VER}-t4.tar.gz
ART_LOCAL=/mnt/i/DWGLS-native-fs/colab-pack/llama-cuda-${VER}-t4.tar.gz

[ -f "$ART_LOCAL" ] && { echo "artifact already cached: $ART_LOCAL"; exit 0; }

# reuse live session or allocate
if ! colab sessions 2>/dev/null | grep -q "\[$S\]"; then
  colab new -s "$S" --gpu T4
fi

cat > /tmp/cuda_build_launch.py << PYEOF
import subprocess, os
DONE="/content/BUILD_DONE"
LOG="/content/build.log"
if os.path.exists(DONE):
    print("already built"); raise SystemExit
if os.path.exists(LOG):
    print("build already in progress"); raise SystemExit
cmd = f'''
set -e
if [ ! -d /content/llama.cpp ]; then
  git clone --depth 1 --branch ${VER} https://github.com/ggml-org/llama.cpp /content/llama.cpp
fi
cd /content/llama.cpp
echo "== configure ==" >> {LOG}
cmake -B build -S . -DGGML_CUDA=ON -DCMAKE_CUDA_ARCHITECTURES=${ARCH} \\
      -DCMAKE_BUILD_TYPE=Release -DLLAMA_CURL=OFF >> {LOG} 2>&1
echo "== build ==" >> {LOG}
cmake --build build --config Release -j \$(nproc) \\
      --target llama-server llama-cli llama-bench >> {LOG} 2>&1
echo "== strip/pack ==" >> {LOG}
for b in build/bin/*; do
  [ -f "\$b" ] || continue
  case "\$b" in *.so*) ;; *) strip "\$b" 2>/dev/null || true ;; esac
done
tar czf ${ART_REMOTE} -C build bin
touch {DONE}
'''
subprocess.Popen(["bash","-c",cmd], start_new_session=True)
print("build launched (detached)")
PYEOF
colab exec -s "$S" --timeout 90 -f /tmp/cuda_build_launch.py

cat > /tmp/cuda_poll.py << 'PYEOF'
import os
DONE="/content/BUILD_DONE"; LOG="/content/build.log"
if os.path.exists(DONE):
    print("BUILD_DONE")
    out="/content/llama-cuda-b10588-t4.tar.gz"
    print(f"artifact {os.path.getsize(out)/1e6:.1f} MB")
    raise SystemExit
if not os.path.exists(LOG):
    print("(no log yet)")
else:
    txt=open(LOG,'rb').read()
    lines=[l for l in txt.splitlines() if l.strip()]
    print(lines[-1].decode(errors='replace')[:160])
    pct=[l for l in lines if b'%' in l]
    if pct: print(pct[-1].decode(errors='replace')[:160])
PYEOF

for i in $(seq 1 30); do
  OUT=$(colab exec -s "$S" --timeout 60 -f /tmp/cuda_poll.py 2>&1) || \
    { echo "POLL FAILED — session may have been pruned:"; echo "$OUT" | tail -5; exit 1; }
  echo "[poll $i] $OUT"
  echo "$OUT" | grep -q BUILD_DONE && break
  sleep 150
done

[ -f "$ART_LOCAL" ] || colab download -s "$S" "$ART_REMOTE" "$ART_LOCAL"
ls -la "$ART_LOCAL"
echo "artifact cached — future deploys: deploy_llama_cuda.sh (no rebuild)"
colab stop -s "$S"
