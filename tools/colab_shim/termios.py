"""POSIX termios shim for Windows - only imported, never used by `colab run`."""
TCSANOW = 0
TCSADRAIN = 1
TCSAFLUSH = 2
def tcgetattr(fd): return []
def tcsetattr(fd, when, attrs): pass
