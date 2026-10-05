import sys, platform, os
print("python:", sys.version.split()[0])
print("platform:", platform.platform())
print("cpu_count:", os.cpu_count())
print("cwd:", os.getcwd())
try:
    import numpy; print("numpy:", numpy.__version__)
except Exception as e:
    print("numpy: -", e)
