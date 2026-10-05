import subprocess, sys, os
print("=== GPU ENV ===")
for cmd in [["nvidia-smi"], ["nvidia-smi", "-L"]]:
    try:
        out = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
        print("$", " ".join(cmd))
        print(out.stdout or out.stderr)
    except Exception as e:
        print("$", " ".join(cmd), "-> ERR", e)

print("=== TORCH ===")
try:
    import torch
    print("torch:", torch.__version__)
    print("cuda_available:", torch.cuda.is_available())
    if torch.cuda.is_available():
        print("device:", torch.cuda.get_device_name(0))
        print("capability:", torch.cuda.get_device_capability(0))
except Exception as e:
    print("torch: -", e)

print("=== CPU INFO ===")
try:
    with open("/proc/cpuinfo") as f:
        for line in f:
            if line.startswith("model name"):
                print(line.strip()); break
except Exception as e:
    print("cpuinfo: -", e)
print("cpu_count:", os.cpu_count())
