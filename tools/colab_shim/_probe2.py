import subprocess, os, json, time

res = {}
try:
    o = subprocess.run(["nvidia-smi", "--query-gpu=name,driver_version,memory.total",
                        "--format=csv,noheader"], capture_output=True, text=True, timeout=30)
    res["nvidia_smi"] = (o.stdout or o.stderr).strip()
except Exception as e:
    res["nvidia_smi"] = "ERR " + str(e)

try:
    import torch
    res["torch"] = torch.__version__
    res["cuda"] = torch.cuda.is_available()
    if torch.cuda.is_available():
        res["gpu_name"] = torch.cuda.get_device_name(0)
        res["gpu_cap"] = list(torch.cuda.get_device_capability(0))
except Exception as e:
    res["torch"] = "ERR " + str(e)

res["cpu_count"] = os.cpu_count()

with open("/content/gpu_report.json", "w") as f:
    json.dump(res, f, indent=2)
print("written /content/gpu_report.json")
