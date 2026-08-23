#!/bin/bash
# Deploy llama.cpp CUDA via prebuilt Python wheel — NO BUILD needed.
# Run: wsl -d Geomatt -- bash /mnt/i/DWGLS-native-fs/colab-pack/deploy_llama_cuda.sh
set -e
export PATH="$HOME/.local/bin:$PATH"
S=dwgls-llama-cuda

# kill orphan if exists
orphan=$(colab sessions 2>/dev/null | grep -oP '\[\?\]\s+\K\S+' | head -1)
if [ -n "$orphan" ]; then
  echo "killing orphan: $orphan"
  colab stop -s "$orphan" 2>/dev/null || true
  sleep 5
fi

colab new -s "$S" --gpu T4

cat > /tmp/dwgls_cuda_infer.py << 'PYEOF'
import subprocess, os, sys

print("== install llama-cpp-python (prebuilt CUDA wheel) ==")
subprocess.run([sys.executable,"-m","pip","install","-q",
    "llama-cpp-python[server]",
    "--extra-index-url","https://abetlen.github.io/llama-cpp-python/whl/cu124"],
    check=True)

print("== download model ==")
murl="https://huggingface.co/Qwen/Qwen2.5-0.5B-Instruct-GGUF/resolve/main/qwen2.5-0.5b-instruct-q8_0.gguf"
subprocess.run(["wget","-q","-O","/content/model.gguf",murl],check=True)
sz=os.path.getsize("/content/model.gguf"); assert sz>500_000_000
print(f"model ok {sz/1e6:.1f} MB")

print("== GPU check ==")
subprocess.run(["nvidia-smi","--query-gpu=name,memory.total","--format=csv"])

print("== test inference ==")
from llama_cpp import Llama
llm = Llama(model_path="/content/model.gguf", n_gpu_layers=99, verbose=True)
output = llm("The capital of France is", max_tokens=16, temperature=0)
print("OUTPUT:", output["choices"][0]["text"])

print("== bench ==")
import time
for p in [32, 128, 512]:
    t0=time.time()
    llm("*"*(p-1), max_tokens=64, temperature=0)
    dt=time.time()-t0
    toks=64
    print(f"  prompt={p:>4d} tokens={toks} time={dt:.2f}s tok/s={toks/dt:.1f}")

print("== DONE: llama-cpp-python CUDA ready ==")
PYEOF

colab exec -s "$S" --timeout 900 -f /tmp/dwgls_cuda_infer.py
colab stop -s "$S"
