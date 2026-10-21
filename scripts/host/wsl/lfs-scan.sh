#!/usr/bin/env bash
# LFS 対象候補の決定用: カーネルソース中の最大ファイルと kernel.git の大ファイルを計測
set -uo pipefail
ROOT="/mnt/c/Users/Sentaro Takenaka/Documents/GitHub/zhibox-os"
cd "$ROOT" || exit 1

echo "=== kernel source: top 20 largest blobs (bytes) ==="
git --git-dir=kernel.git ls-tree -r -l HEAD | sort -k4 -nr | head -20

echo "=== counts by threshold ==="
git --git-dir=kernel.git ls-tree -r -l HEAD | awk '$4+0 > 104857600 {c++} END {print "over100MB:", c+0}'
git --git-dir=kernel.git ls-tree -r -l HEAD | awk '$4+0 > 52428800  {c++} END {print "over50MB :", c+0}'
git --git-dir=kernel.git ls-tree -r -l HEAD | awk '$4+0 > 10485760  {c++} END {print "over10MB :", c+0}'
git --git-dir=kernel.git ls-tree -r -l HEAD | awk '$4+0 > 1048576   {c++} END {print "over1MB  :", c+0}'

echo "=== kernel.git dir: files over 1MB ==="
find kernel.git -type f -size +1M -printf '%s %p\n' | sort -nr | head -20

echo "=== totals ==="
du -sh kernel kernel.git 2>/dev/null

echo "=== done ==="
