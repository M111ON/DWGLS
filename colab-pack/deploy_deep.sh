#!/bin/bash
set -e
export PATH="$HOME/.local/bin:$PATH"
S=dwgls-deep
colab new -s $S
cd /mnt/i/DWGLS-native-fs/colab-pack
colab upload -s $S ./rid_graft_linux /content/rid_graft_linux
cat > /tmp/dwgls_deep.py << 'PYEOF'
import subprocess, os
subprocess.run(["chmod","+x","/content/rid_graft_linux"])
print("== cpu ==")
print(open("/proc/cpuinfo").read().split("model name")[1].split("\n")[0])
os.makedirs("/content/build", exist_ok=True)
print("== model ==")
u="https://huggingface.co/Qwen/Qwen2.5-0.5B-Instruct-GGUF/resolve/main/qwen2.5-0.5b-instruct-q8_0.gguf"
subprocess.run(["wget","-q","-O","/content/model.gguf",u],check=True)
sz=os.path.getsize("/content/model.gguf"); assert sz>500_000_000
print(f"ok {sz/1e6:.1f} MB")
print("== RID graft gates on Colab ==")
r=subprocess.run(["/content/rid_graft_linux","/content/model.gguf",
                  "The capital of France is","16","/content/reg.twin"],
                 capture_output=True,text=True,timeout=3000)
out=r.stdout.splitlines()
print(r.stdout[-3000:] if r.stdout else "(no stdout)")
if r.returncode!=0:
    print("STDERR tail:", r.stderr[-600:])
PYEOF
colab exec -s $S --timeout 3600 -f /tmp/dwgls_deep.py
colab stop -s $S
