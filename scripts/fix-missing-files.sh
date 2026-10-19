#!/usr/bin/env bash
#
# NTFS の「大文字小文字区別」ディレクトリ有効化後、欠落しているファイルを
# git のオブジェクトから復元する。Windows 側(git for Windows)の Win32 レイヤーは
# 区別フラグを無視して "File exists" で失敗するため、WSL(NT レベル)から書き戻す。
#
set -uo pipefail

REPO="/mnt/c/Users/Sentaro Takenaka/Documents/GitHub/zhibox-os/kernel"
cd "$REPO" || exit 1
git config --global --add safe.directory "$REPO" 2>/dev/null

missing=$(git status --porcelain | grep '^ D' | cut -c4- || true)
if [ -z "$missing" ]; then
  echo "欠落ファイルはありません (既に完全)"
  exit 0
fi

echo "欠落ファイル数: $(echo "$missing" | wc -l)"
echo "$missing" | while IFS= read -r f; do
  [ -z "$f" ] && continue
  if git checkout -f HEAD -- "$f" 2>/tmp/co_err.txt; then
    echo "  OK   $f"
  else
    echo "  NG   $f  : $(cat /tmp/co_err.txt)"
  fi
done

echo "--- 復元後の状態 ---"
left=$(git status --porcelain | wc -l)
echo "残存 dirty = $left"
git status --porcelain | head -20

echo "--- ペア検証 (26ファイルが揃っているか) ---"
for p in \
  include/uapi/linux/netfilter/xt_CONNMARK.h \
  include/uapi/linux/netfilter/xt_connmark.h \
  include/uapi/linux/netfilter/xt_MARK.h \
  include/uapi/linux/netfilter/xt_mark.h \
  include/uapi/linux/netfilter/xt_DSCP.h \
  include/uapi/linux/netfilter/xt_dscp.h \
  net/netfilter/xt_HL.c \
  net/netfilter/xt_hl.c ; do
  if [ -f "$p" ]; then echo "  有 : $p"; else echo "  欠 : $p"; fi
done
