#!/bin/bash
# restore-file-dates.sh — Restore file mtime to last git commit date
# Usage: bash restore-file-dates.sh [directory]
# If no directory given, uses current working directory.
# Only affects TRACKED files with git history.
# Untracked files keep their original dates.

set -e

DIR="${1:-.}"
cd "$DIR"

echo "=== Restoring file dates in: $(pwd) ==="
echo "Branch: $(git branch --show-current 2>/dev/null || echo 'detached')"
echo ""

updated=0
skipped=0

git ls-files | while read f; do
    if [ -f "$f" ]; then
        commit_ts=$(git log -1 --format='%at' -- "$f" 2>/dev/null)
        if [ -n "$commit_ts" ]; then
            touch -d "@$commit_ts" "$f" 2>/dev/null
            updated=$((updated + 1))
        else
            skipped=$((skipped + 1))
        fi
    fi
done

echo "Done. Updated tracked files with git history."
echo "Untracked files (new, no git history) were NOT modified."
