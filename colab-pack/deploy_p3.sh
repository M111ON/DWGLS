#!/bin/bash
set -e
export PATH="$HOME/.local/bin:$PATH"
S=dwgls-p3
colab new -s $S
cd /mnt/i/DWGLS-native-fs/colab-pack
colab upload -s $S ./p3_view_roundtrip /content/p3
cat > /tmp/dwgls_p3.py << 'PYEOF'
import subprocess, os, time
subprocess.run(["chmod","+x","/content/p3"])
print("== model ==")
murl="https://huggingface.co/Qwen/Qwen2.5-0.5B-Instruct-GGUF/resolve/main/qwen2.5-0.5b-instruct-q8_0.gguf"
subprocess.run(["wget","-q","-O","/content/model.gguf",murl],check=True)
sz=os.path.getsize("/content/model.gguf"); assert sz>500_000_000
md5o=subprocess.run(["md5sum","/content/model.gguf"],capture_output=True,text=True).stdout.split()[0]
print(f"model {sz/1e6:.1f}MB md5={md5o[:12]}")
print("== prebuilt llama-bench ==")
u="https://github.com/ggml-org/llama.cpp/releases/download/b10588/llama-b10588-bin-ubuntu-x64.tar.gz"
subprocess.run(["wget","-q","-O","/tmp/lc.tar.gz",u],check=True)
subprocess.run(["tar","xzf","/tmp/lc.tar.gz","-C","/content"],check=True)
bindir=None
for root,dirs,files in os.walk("/content"):
    if "llama-bench" in files: bindir=root;break
bench=os.path.join(bindir,"llama-bench"); os.chmod(bench,0o755)
views=["pent","tri","snubL","snubR","hosoya","zeck"]
print(f"{'view':7} {'bake_s':>8} {'unfold_s':>9} {'md5ok':>6} {'pp64':>12} {'tg32':>12}")
for vi,name in enumerate(views):
    t0=time.time()
    r=subprocess.run(["/content/p3","bake","/content/model.gguf",
                      "/content/reg.twin",str(vi)],capture_output=True,text=True)
    assert r.returncode==0, r.stderr[-300:]
    tb=time.time()-t0; t0=time.time()
    r=subprocess.run(["/content/p3","unfold","/content/reg.twin",
                      f"/content/unfolded_{name}.gguf",str(vi),"5156",str(sz)],
                     capture_output=True,text=True)
    assert r.returncode==0, r.stderr[-300:]
    tu=time.time()-t0
    md5=subprocess.run(["md5sum",f"/content/unfolded_{name}.gguf"],
                       capture_output=True,text=True).stdout.split()[0]
    ok="YES" if md5==md5o else "NO!"
    r=subprocess.run([bench,"-m",f"/content/unfolded_{name}.gguf",
                      "-p","64","-n","32","-t","2"],
                     capture_output=True,text=True,timeout=600)
    pp=tg="?"
    for line in r.stdout.splitlines():
        if "| qwen2" in line:
            parts=[p.strip() for p in line.split("|")]
            # [0]'' [1]model [2]size [3]params [4]backend [5]threads
            # [6]test [7]t/s
            if len(parts)>7:
                val=parts[7].split()[0]
                if parts[6]=="pp64": pp=val
                if parts[6]=="tg32": tg=val
    print(f"{name:7} {tb:8.2f} {tu:9.2f} {ok:>6} {pp:>12} {tg:>12}")
PYEOF
colab exec -s $S --timeout 1500 -f /tmp/dwgls_p3.py
colab stop -s $S
