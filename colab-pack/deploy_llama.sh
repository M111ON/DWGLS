#!/bin/bash
set -e
export PATH="$HOME/.local/bin:$PATH"
S=dwgls-llama
colab new -s $S
cat > /tmp/dwgls_llama.py << 'PYEOF'
import subprocess, os
print("== fetch prebuilt llama.cpp ==")
url="https://github.com/ggml-org/llama.cpp/releases/download/b10588/llama-b10588-bin-ubuntu-x64.tar.gz"
subprocess.run(["wget","-q","-O","/tmp/lc.tar.gz",url],check=True)
subprocess.run(["tar","xzf","/tmp/lc.tar.gz","-C","/content"],check=True)
# locate binaries dir
bindir=None
for root,dirs,files in os.walk("/content"):
    if "llama-cli" in files: bindir=root;break
assert bindir, "llama-cli not found"
subprocess.run(["chmod","+x",os.path.join(bindir,"llama-cli"),
                os.path.join(bindir,"llama-bench")])
print("bindir:",bindir)
print("== download model ==")
murl="https://huggingface.co/Qwen/Qwen2.5-0.5B-Instruct-GGUF/resolve/main/qwen2.5-0.5b-instruct-q8_0.gguf"
subprocess.run(["wget","-q","-O","/content/model.gguf",murl],check=True)
sz=os.path.getsize("/content/model.gguf"); assert sz>500_000_000
print(f"model ok {sz/1e6:.1f} MB")
print("== llama-bench (cpu) ==")
r=subprocess.run([os.path.join(bindir,"llama-bench"),"-m","/content/model.gguf",
                  "-p","128","-n","64","-t","2"],
                 capture_output=True,text=True,timeout=900)
print(r.stdout[-2000:])
if r.returncode!=0: print(r.stderr[-800:])
print("== llama-cli smoke ==")
r=subprocess.run([os.path.join(bindir,"llama-cli"),"-m","/content/model.gguf",
                  "-p","The capital of France is","-n","16","--no-cnv",
                  "-t","2","--temp","0"],
                 capture_output=True,text=True,timeout=300)
tail=[l for l in r.stderr.splitlines() if "tokens/sec" in l or "timings" in l.lower()]
print("\n".join(tail[-3:]) if tail else r.stdout[-500:])
PYEOF
colab exec -s $S --timeout 1200 -f /tmp/dwgls_llama.py
colab stop -s $S
