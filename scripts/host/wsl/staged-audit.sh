#!/usr/bin/env bash
# 親リポジトリにステージ済みの内容と kernel.git のインデックスを突き合わせ、
# 「インデックスにあるのに親に載っていないファイル」を列挙する。
set -uo pipefail
ROOT="/mnt/c/Users/Sentaro Takenaka/Documents/GitHub/zhibox-os"
cd "$ROOT" || exit 1

git --git-dir=kernel.git ls-files | sort > /tmp/k_idx.txt
git diff --cached --name-only -- kernel/ | sed 's|^kernel/||' | sort > /tmp/p_k.txt

echo "kernel.git index entries : $(wc -l < /tmp/k_idx.txt)"
echo "parent staged kernel files: $(wc -l < /tmp/p_k.txt)"
echo "staged in kernel.git/ dir : $(git diff --cached --name-only -- kernel.git/ | wc -l)"
echo "staged elsewhere (top-level/scripts): $(git diff --cached --name-only -- . ':!kernel' ':!kernel.git' | wc -l)"

comm -23 /tmp/k_idx.txt /tmp/p_k.txt > /tmp/missing.txt
echo "--- in kernel.git but NOT staged in parent: $(wc -l < /tmp/missing.txt) ---"
head -20 /tmp/missing.txt

comm -13 /tmp/k_idx.txt /tmp/p_k.txt > /tmp/extra.txt
echo "--- staged in parent but NOT in kernel.git index: $(wc -l < /tmp/extra.txt) ---"
head -10 /tmp/extra.txt

echo "--- done ---"
