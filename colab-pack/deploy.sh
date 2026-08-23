#!/bin/bash
set -e
export PATH="$HOME/.local/bin:$PATH"
S=dwgls-p2
echo "== new session =="
colab new -s $S
echo "== upload binaries =="
cd /mnt/i/DWGLS-native-fs/colab-pack
for b in p2_view_perf layer_manifest_probe hexagram_cubes_probe zeckendorf_probe hosoya_view_probe circle_config_probe pascal_zigzag_probe; do
  colab upload -s $S ./$b /content/$b
done
echo "== remote script =="
cat > /tmp/dwgls_run.py << 'PYEOF'
import subprocess, os
for b in ["p2_view_perf","layer_manifest_probe","hexagram_cubes_probe",
          "zeckendorf_probe","hosoya_view_probe","circle_config_probe",
          "pascal_zigzag_probe"]:
    subprocess.run(["chmod","+x",f"/content/{b}"])
print("== smoke probes ==")
for b in ["hexagram_cubes_probe","zeckendorf_probe","layer_manifest_probe"]:
    r=subprocess.run([f"/content/{b}"],capture_output=True,text=True,timeout=120)
    print(b, "->", r.stdout.strip().splitlines()[-1])
print("== cpu info ==")
print(open("/proc/cpuinfo").read().split("model name")[1].split("\n")[0])
r=subprocess.run(["nproc"],capture_output=True,text=True); print("cores:",r.stdout.strip())
print("== downloading model ==")
url="https://huggingface.co/Qwen/Qwen2.5-0.5B-Instruct-GGUF/resolve/main/qwen2.5-0.5b-instruct-q8_0.gguf"
subprocess.run(["wget","-q","-O","/content/model.gguf",url],check=True)
sz=os.path.getsize("/content/model.gguf")
assert sz>500_000_000, f"download too small: {sz}"
print(f"model ok: {sz/1e6:.1f} MB")
print("== P2 view perf (RAM mode) ==")
r=subprocess.run(["/content/p2_view_perf","/content/model.gguf","/tmp/p2.twin","1"],
                 capture_output=True,text=True,timeout=900)
print(r.stdout)
if r.returncode!=0: print("STDERR:",r.stderr[:500])
PYEOF
colab exec -s $S --timeout 900 -f /tmp/dwgls_run.py
echo "== stop =="
colab stop -s $S
