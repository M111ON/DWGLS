#!/bin/bash
# deploy_termux.sh — Transfer DWGLS to Termux and build
# Usage: bash deploy_termux.sh <termux_host> [user]
# Example: bash deploy_termux.sh 192.168.1.100
set -e

HOST="${1:?Usage: $0 <termux_host> [user]}"
USER="${2:-termux}"
REMOTE_DIR="/data/data/com.termux/files/home/dwgls"
PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"

echo "══ Deploying DWGLS to Termux ══"
echo "Host: $USER@$HOST:$REMOTE_DIR"
echo "Source: $PROJECT_DIR"
echo ""

# Create remote dir
ssh "$USER@$HOST" "mkdir -p $REMOTE_DIR"

# Sync (excluding build dirs, .git, large files)
echo "▶ Syncing files..."
rsync -avz --progress \
  --exclude='build/' \
  --exclude='build_arm/' \
  --exclude='.git/' \
  --exclude='*.exe' \
  --exclude='*.tesspack' \
  --exclude='*.gguf' \
  --exclude='*.bin' \
  --exclude='tess_out/' \
  "$PROJECT_DIR/" "$USER@$HOST:$REMOTE_DIR/"

echo ""
echo "▶ Installing build deps on Termux..."
ssh "$USER@$HOST" "pkg install -y gcc make binutils" 2>/dev/null || true

echo ""
echo "▶ Building on Termux..."
ssh "$USER@$HOST" "cd $REMOTE_DIR && bash scripts/build_termux.sh test"

echo ""
echo "══ Deploy complete ══"
