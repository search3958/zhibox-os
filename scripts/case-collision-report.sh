#!/usr/bin/env bash
#
# Git のインデックスから「大文字小文字だけが違う」ファイルの組を列挙する。
# NTFS(Windows)は大文字小文字非区別なので、この組が存在すると
# クローン時に片方がもう片方に潰れて欠落する (= Linux のビルド不能)。
#
# 出力: /tmp/case_pairs.txt に元パスを2行1組で出力し、画面には件数と対象ディレクトリを表示
#
set -euo pipefail

ROOT="${ZHIBOX_ROOT:-/mnt/c/Users/Sentaro Takenaka/Documents/GitHub/zhibox-os}"
REPO="${1:-$ROOT/kernel}"
if [ -d "$REPO/.git" ]; then GD="$REPO/.git"; else GD="$ROOT/kernel.git"; fi
OUT=/tmp/case_pairs.txt
DIRS=/tmp/case_dirs.txt

cd "$REPO" || { echo "cd failed: $REPO"; exit 1; }

git --git-dir="$GD" --work-tree="$REPO" ls-files \
  | awk '{ print tolower($0) "\t" $0 }' \
  | sort \
  | awk -F'\t' '
      $1 == prev { if (!printed) print pl; print $2; printed = 1; next }
      { printed = 0; prev = $1; pl = $2 }
    ' \
  | sort -u > "$OUT"

n_pairs=$(wc -l < "$OUT")
echo "CASE_COLLISION_FILES=$n_pairs"

echo "--- 衝突しているファイル (2行1組) ---"
cat "$OUT"

xargs -n1 dirname < "$OUT" | sort -u > "$DIRS"
echo "--- 対象ディレクトリ ---"
cat "$DIRS"
