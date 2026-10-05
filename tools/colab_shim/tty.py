"""POSIX tty shim for Windows - only imported, never used by `colab run`."""
def setraw(fd, when=0): pass
def setcbreak(fd, when=0): pass
