#!/usr/bin/env bash
# kernel クローン用ラッパー
#   履歴 (kernel.git/) とソース (kernel/) を分離しているため、git 操作は
#   必ず --git-dir と --work-tree を同時に指定する必要がある。それを自動付与する。
#
#   使い方 (WSL 内):  bash scripts/k.sh status
#                     bash scripts/k.sh log --oneline -5
#                     bash scripts/k.sh diff
#   Windows 側の PowerShell なら:
#     git --git-dir=kernel.git --work-tree=kernel status
#
ROOT="${ZHIBOX_ROOT:-/mnt/c/Users/Sentaro Takenaka/Documents/GitHub/zhibox-os}"
if [ -d "$ROOT/kernel/.git" ]; then
  exec git -C "$ROOT/kernel" "$@"
fi
exec git --git-dir="$ROOT/kernel.git" --work-tree="$ROOT/kernel" "$@"
