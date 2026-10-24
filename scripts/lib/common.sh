# 共通関数: OS 判定・パス・ツール検出 (WSL / macOS 両対応)
#   . で読み込む前提 (source しないこと、bash の . 構文を使用)
#
# 環境変数:
#   ZHIBOX_SRC  ビルド用ツリーの場所を既定値から変える場合に設定

_zh_common_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="${ZHIBOX_ROOT:-$(cd "$_zh_common_dir/../.." && pwd)}"

say()  { echo; echo "==> $*"; }
fail() { echo "ERROR: $*" >&2; exit 1; }

zh_os() {
  case "$(uname -s)" in
    Darwin) echo macos ;;
    *)
      if [ -n "${WSL_DISTRO_NAME:-}" ] || grep -qi microsoft /proc/version 2>/dev/null; then
        echo wsl
      else
        echo linux
      fi ;;
  esac
}

OS="$(zh_os)"

OUT="$REPO_ROOT/output"
INITRAMFS="${INITRAMFS:-$HOME/initramfs}"
INITRD="${INITRD:-$HOME/initramfs.cpio.gz}"
LOG="$HOME/boot.log"

# --------------------------------------------------------------- macOS: ケースセンシティブ領域
# APFS は大文字小文字非区別。カーネルソースには xt_CONNMARK.h / xt_connmark.h のような
# 衝突ペアが13組あるため、ケースセンシティブな APFS イメージ内でビルドする (sudo 不要)。
ZH_VOLDisk="$HOME/zhibox-kernel.sparsebundle"
ZH_VOL="/Volumes/zhibox-kernel"

zh_mount_volume() {
  [ "$(zh_os)" = macos ] || return 0
  if mount | grep -q " on $ZH_VOL "; then
    return 0
  fi
  if [ ! -d "$ZH_VOLDisk" ]; then
    say "ケースセンシティブ APFS イメージを作成 ($ZH_VOLDisk, 最大24GB・実使用分だけ拡張)"
    hdiutil create -size 24g -type SPARSEBUNDLE -fs 'Case-sensitive APFS' \
      -volname zhibox-kernel "$ZH_VOLDisk" >/dev/null
  fi
  hdiutil attach -nobrowse "$ZH_VOLDisk" >/dev/null
  mount | grep -q " on $ZH_VOL " || fail "ディスクイメージのマウントに失敗"
}

# --------------------------------------------------------------- ビルドツリー
# 注意: この関数は $() 内で使われるため、say() で標準出力へ書いてはいけない
zh_src_tree() {
  if [ -n "${ZHIBOX_SRC:-}" ]; then
    echo "$ZHIBOX_SRC"
  elif [ "$(zh_os)" = macos ]; then
    echo "$ZH_VOL/src"
  else
    echo "$HOME/kernel-src"
  fi
}

# git オブジェクトから完全なツリーを展開 (大文字小文字衝突ペアも両方入る)
zh_extract_tree() {
  local dst="$1"
  mkdir -p "$dst"
  if [ -d "$REPO_ROOT/kernel/.git" ]; then
    git -C "$REPO_ROOT/kernel" archive HEAD | tar -x -C "$dst"
  else
    git --git-dir="$REPO_ROOT/kernel.git" archive HEAD | tar -x -C "$dst"
  fi
}

# --------------------------------------------------------------- macOS の GNU ツールパス
zh_setup_path_macos() {
  local brewp
  brewp="$(brew --prefix 2>/dev/null)" || return 0
  local d
  for d in coreutils gnu-sed gawk findutils; do
    [ -d "$brewp/opt/$d/libexec/gnubin" ] && PATH="$brewp/opt/$d/libexec/gnubin:$PATH"
  done
  export PATH
}

# --------------------------------------------------------------- クロスコンパイラ検出 (macOS)
# カーネルはフリースタンディングなので *-elf-gcc (Linux 非対応のベアメタル) でも動く
zh_find_cross_gcc() {
  local c
  for c in x86_64-elf-gcc x86_64-linux-musl-gcc x86_64-unknown-linux-gnu-gcc x86_64-linux-gnu-gcc; do
    if command -v "$c" >/dev/null 2>&1; then
      echo "$c"
      return 0
    fi
  done
  return 1
}

# 新しめの GCC (>=14) は古いカーネルコードをエラー扱いするので警告へ降格させる
zh_gcc_compat_flags() {
  local cc="$1" ver major
  ver="$($cc -dumpfullversion 2>/dev/null || $cc -dumpversion 2>/dev/null)"
  major="${ver%%.*}"
  if [ "${major:-0}" -ge 14 ] 2>/dev/null; then
    echo "-Wno-error=implicit-function-declaration -Wno-error=implicit-int" \
         "-Wno-error=int-conversion -Wno-error=incompatible-pointer-types" \
         "-Wno-error=return-mismatch -Wno-error=declaration-missing-parameter-type"
  fi
}
