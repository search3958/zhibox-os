#!/usr/bin/env bash
# Windows で git add できない「予約デバイス名」(AUX/CON/NUL/PRN/COM1..9/LPT1..9)
# を持つパスの洗い出し。ディレクトリ名・ファイル名の両方を対象とする。
set -uo pipefail

cd "/mnt/c/Users/Sentaro Takenaka/Documents/GitHub/zhibox-os" || exit 1
GD=kernel.git
WT=kernel

git --git-dir=$GD --work-tree=$WT ls-files > /tmp/allfiles.txt
echo "total index files: $(wc -l < /tmp/allfiles.txt)"

echo "--- reserved names used as DIRECTORY components ---"
grep -iE "/(aux|con|prn|nul|clock\$|com[0-9]|lpt[0-9])(/|$)" /tmp/allfiles.txt || echo "(none)"

echo "--- reserved names used as FILE basenames ---"
grep -iE "/(aux|con|prn|nul|clock\$|com[0-9]|lpt[0-9])(\.[^/]*)?$" /tmp/allfiles.txt || echo "(none)"

echo "--- skip-worktree/assume-unchanged flags on aux files ---"
git --git-dir=$GD --work-tree=$WT ls-files -v | grep -iE "aux\.(c|h)$" || echo "(none)"

echo "--- done ---"
