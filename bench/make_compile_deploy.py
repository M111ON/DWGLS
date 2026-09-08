import subprocess, base64

# Read kernel
result = subprocess.run(["wsl", "bash", "-c", "cat /mnt/i/DWGLS-native-fs/bench/tess_scatter_bench.cu"], capture_output=True, text=True)
b64 = base64.b64encode(result.stdout.encode()).decode()

script = f'''
import subprocess, base64

# Decode kernel
kernel_src = base64.b64decode("{b64}").decode()
with open("/content/tess_scatter_bench.cu", "w") as f:
    f.write(kernel_src)
print(f"Kernel: {{len(kernel_src)}} bytes")

# Install CUDA toolkit (nvcc only, no driver needed for compile)
print("Installing CUDA toolkit...")
r = subprocess.run(["apt-get", "update", "-qq"], capture_output=True, text=True, timeout=120)
r = subprocess.run(["apt-get", "install", "-y", "-qq", "nvidia-cuda-toolkit"], capture_output=True, text=True, timeout=600)
print(f"Install: rc={{r.returncode}}")

# Verify nvcc
r = subprocess.run(["nvcc", "--version"], capture_output=True, text=True)
if r.returncode == 0:
    lines = r.stdout.strip().split("\\n")
    print(f"CUDA: {{lines[-1]}}")

    # Compile
    print("Compiling tess_scatter_bench.cu...")
    r = subprocess.run(["nvcc", "-O3", "-std=c++17", "-arch=sm_75",
        "-o", "/content/tess_scatter_bench",
        "/content/tess_scatter_bench.cu", "-lm"],
        capture_output=True, text=True, timeout=300)
    print(f"Compile: rc={{r.returncode}}")
    if r.stderr:
        print(f"Stderr: {{r.stderr[:1000]}}")
    if r.returncode == 0:
        r2 = subprocess.run(["ls", "-la", "/content/tess_scatter_bench"], capture_output=True, text=True)
        print(f"Binary: {{r2.stdout.strip()}}")
else:
    print("nvcc install failed")
    print(r.stderr[:500] if r.stderr else "no error")
'''

with open("C:/Users/Administrator.AVENTADOR/AppData/Local/Temp/opencode/colab_compile.py", "w") as f:
    f.write(script)
print("Written:", len(script), "bytes")
