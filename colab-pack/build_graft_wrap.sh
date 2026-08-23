#!/bin/bash
wsl -d Geomatt -- bash /mnt/i/DWGLS-native-fs/colab-pack/build_graft.sh 2>&1 | grep -Ei "error|BUILT|undefined" || echo "no errors"
ls -la /mnt/i/DWGLS-native-fs/colab-pack/rid_graft_linux 2>/dev/null
