import subprocess, base64, sys

# Read kernel source from I: drive (accessible from WSL)
result = subprocess.run(["wsl", "bash", "-c", "cat /mnt/i/DWGLS-native-fs/bench/tess_scatter_bench.cu"], capture_output=True, text=True)
kernel_src = result.stdout
if not kernel_src:
    print("ERROR: Could not read kernel source")
    sys.exit(1)

b64 = base64.b64encode(kernel_src.encode()).decode()
print("Kernel:", len(kernel_src), "bytes -> b64:", len(b64), "bytes")

# Write a Python script that colab-cli will send to Colab
# The script embeds the kernel as base64 and decodes it on Colab
script_lines = []
script_lines.append("import subprocess, os, base64")
script_lines.append("")
script_lines.append("kernel_b64 = '" + b64 + "'")
script_lines.append("kernel_src = base64.b64decode(kernel_b64).decode()")
script_lines.append("with open('/content/tess_scatter_bench.cu', 'w') as f:")
script_lines.append("    f.write(kernel_src)")
script_lines.append("print('Kernel:', len(kernel_src), 'bytes')")
script_lines.append("")
script_lines.append("r = subprocess.run(['nvidia-smi', '--query-gpu=name,memory.total,memory.free', '--format=csv,noheader'], capture_output=True, text=True)")
script_lines.append("print('GPU:', r.stdout.strip())")
script_lines.append("")
script_lines.append("r = subprocess.run(['nvcc', '-O3', '-std=c++17', '-arch=sm_75', '-o', '/content/tess_scatter_bench', '/content/tess_scatter_bench.cu', '-lm'], capture_output=True, text=True, timeout=180)")
script_lines.append("print('Compile: rc=' + str(r.returncode))")
script_lines.append("if r.stderr:")
script_lines.append("    print('Errors:', r.stderr[:1000])")
script_lines.append("if r.returncode == 0:")
script_lines.append("    print('Running with 100 capos...')")
script_lines.append("    r2 = subprocess.run(['/content/tess_scatter_bench', '/content/lfm8b.tesspack', '/content/LFM2.5-8B-A1B-Q4_K_M.gguf', '100'], capture_output=True, text=True, timeout=120)")
script_lines.append("    print(r2.stdout)")
script_lines.append("    if r2.stderr:")
script_lines.append("        print('STDERR:', r2.stderr[:500])")
script_lines.append("else:")
script_lines.append("    print('COMPILE FAILED')")

script = "\n".join(script_lines)

# Write to WSL /tmp
result = subprocess.run(
    ["wsl", "bash", "-c", "cat > /tmp/colab_write_kernel.py"],
    input=script, capture_output=True, text=True
)
print("Deploy script written:", len(script), "bytes")

# Verify
result = subprocess.run(["wsl", "bash", "-c", "wc -c /tmp/colab_write_kernel.py"], capture_output=True, text=True)
print("Verify:", result.stdout.strip())
