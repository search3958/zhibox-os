#!/usr/bin/env bash
#
# openEuler kernel (OLK-5.10) のビルド + QEMU ブート確認 一括スクリプト
#   WSL (Ubuntu) でも macOS (Apple Silicon) でも同じコマンドで動く:
#
#     bash scripts/build-and-boot.sh
#
#   WSL : ネイティブ gcc でビルド (gcc-x86_64-linux-gnu ツールチェーン不要)
#   macOS: x86_64-elf-gcc 等のクロスコンパイラでビルド (brew install x86_64-elf-gcc)
#          ケースセンシティブ APFS イメージ内でツリーを展開・ビルド
#
# 補足:
#   Windows 側 (NTFS) と macOS (APFS) は大文字小文字非区別のため、カーネルソース中の
#   "xt_CONNMARK.h / xt_connmark.h" のような13組の衝突ファイルを保持できない。
#   そこでビルド用ツリーは git のオブジェクトから直接展開して完全な状態を保つ。
#
set -euo pipefail

. "$(cd "$(dirname "$0")" && pwd)/lib/common.sh"

JOBS="${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || nproc)}"

# 進捗ログをリポジトリ側にも書き出す (別窓で tail できるように)
mkdir -p "$OUT"
if [ ! -t 1 ]; then :; else exec > >(tee "$OUT/build.log") 2>&1; fi

cd "$REPO_ROOT"

if [ "$OS" = macos ]; then
  # ------------------------------------------------- macOS: クロスコンパイル設定
  say "0/6 macOS ネイティブビルド環境を準備"
  zh_setup_path_macos
  CROSS_GCC="$(zh_find_cross_gcc)" || fail "クロスコンパイラが見つかりません。
  次のいずれかを導入してください:  brew install x86_64-elf-gcc
  (FiloSottile/homebrew-musl-cross の x86_64-linux-musl-gcc でも可)"
  CROSS_PREFIX="${CROSS_GCC%-gcc}-"
  echo "    CC=$CROSS_GCC ($( "$CROSS_GCC" -dumpversion ))  LD=${CROSS_PREFIX}ld"
  command -v gmake >/dev/null || fail "gmake がありません: brew install make"
  MAKE=gmake
  # ホストツール (modpost, relocs, selinux genheaders 等) は macOS に
  # elf.h / byteswap.h / endian.h / arch の uapi ヘッダが無いため差し込む
  # ($(srctree)/$(objtree) は make 側で展開される)
  HOSTCFLAGS_SHIM="-I$REPO_ROOT/scripts/host-include/darwin -I\$(srctree)/arch/x86/include/uapi -I\$(objtree)/arch/x86/include/generated/uapi"
  # 新 GCC 向け警告降格フラグ (ターゲットコードのみ)
  TARGET_KCFLAGS="$(zh_gcc_compat_flags "$CROSS_GCC")"
else
  # ------------------------------------------------- WSL/Linux: ネイティブビルド
  MAKE=make
  CROSS_PREFIX=""
  HOSTCFLAGS_SHIM=""
  TARGET_KCFLAGS=""
fi

MAKE_ARGS=(
  ARCH=x86
  KBUILD_DEFCONFIG=x86_64_defconfig
  HOSTCC="${HOSTCC:-cc}"
  -j"$JOBS"
)
[ -n "$CROSS_PREFIX" ] && MAKE_ARGS+=(CROSS_COMPILE="$CROSS_PREFIX")
[ -n "$HOSTCFLAGS_SHIM" ] && MAKE_ARGS+=(HOSTCFLAGS+="$HOSTCFLAGS_SHIM")
[ -n "$TARGET_KCFLAGS" ] && MAKE_ARGS+=(KCFLAGS="$TARGET_KCFLAGS")

# ------------------------------------------------------- 1. ツリー展開 + 同期
if [ "$OS" = macos ]; then zh_mount_volume; fi
SRC="$(zh_src_tree)"
if [ -f "$SRC/Makefile" ]; then
  if [ "${SYNC:-0}" = "1" ]; then
    say "1/6 リポジトリ側の変更をオーバーレイ同期 (SYNC=1, size比較のみで高速)"
    rsync -a --size-only --exclude='.git' "$REPO_ROOT/kernel/" "$SRC/"
  else
    say "1/6 ビルド用ツリーを利用 (同期は SYNC=1 で有効)"
  fi
else
  say "1/6 git オブジェクトから完全なツリーを展開 (大文字小文字衝突ファイルも含む)"
  zh_extract_tree "$SRC"
fi
cd "$SRC"
echo "    ツリー: $SRC ($(du -sh "$SRC" 2>/dev/null | cut -f1))  /  jobs=$JOBS"

# ---------------------------------------------------------------- 2. 設定
say "2/6 make defconfig"
if [ ! -f .config ]; then
  "$MAKE" "${MAKE_ARGS[@]}" defconfig
else
  echo "    .config あり (削除すると再生成)"
fi
bash scripts/config --enable DRM_BOCHS

if [ "$OS" = macos ]; then
  # objtool (スタック検証 / ORC unwinder) は macOS ホストではビルド不能 (libelf 等に依存)。
  # X86_64 設定の "select STACK_VALIDATION" を Kconfig から外し (冪等)、
  # フレームポインタ unwinder に切り替える。
  sed -i 's/^[[:space:]]*select STACK_VALIDATION[[:space:]]\+if.*$//' arch/x86/Kconfig
  bash scripts/config --disable UNWINDER_ORC --enable UNWINDER_FRAME_POINTER \
                      --disable STACK_VALIDATION --disable GCC_PLUGINS
  # Darwin の uuid_t (配列型) とカーネルの uuid_t (struct) が file2alias.c で衝突する
  sed -i 's/^} uuid_t;$/} zk_uuid_t;\n#define uuid_t zk_uuid_t \/* macOS: Darwin uuid_t と衝突するため *\//' \
    scripts/mod/file2alias.c
  # macOS の listxattr/getxattr は Linux より引数が多い (末尾に options/position)
  sed -i \
    -e 's/listxattr(path, NULL, 0)/listxattr(path, NULL, 0, 0)/' \
    -e 's/listxattr(path, xattr_list, list_len)/listxattr(path, xattr_list, list_len, 0)/' \
    -e 's/getxattr(path, list_ptr, NULL, 0)/getxattr(path, list_ptr, NULL, 0, 0, 0)/' \
    -e 's/getxattr(path, list_ptr, xattr_value, value_len)/getxattr(path, list_ptr, xattr_value, value_len, 0, 0)/' \
    usr/gen_init_cpio.c
  # binutils 2.4x の gas は .quad 式末尾のセミコロンを許容しない
  sed -i 's/+ _PAGE_TABLE_NOENC;/+ _PAGE_TABLE_NOENC/' arch/x86/kernel/head_64.S
  # 新 GCC は既定が C23 (bool/false がキーワード化)。realmode は KBUILD_CFLAGS が
  # フィルタし直されるため -std=gnu11 を明示する
  sed -i 's/^export REALMODE_CFLAGS$/REALMODE_CFLAGS += -std=gnu11\nexport REALMODE_CFLAGS/' \
    arch/x86/Makefile
  # BSD regex (macOS libc) は空の選択肢 "(|foo)" を REG_EMPTY で拒否する (glibc は許容)
  sed -i 's/(|\([^)]*\))/(\1)?/g' arch/x86/tools/relocs.c
  # ホスト (arm64) とターゲット (x86_64) はどちらも LP64。ホストツール (vdso2c 等) が
  # ワード長を正しく得られるよう arch の判定に aarch64 を追加
  sed -i 's/^#if defined(__x86_64__) \&\& !defined(__ILP32__)$/#if (defined(__x86_64__) || defined(__aarch64__)) \&\& !defined(__ILP32__)/' \
    arch/x86/include/uapi/asm/bitsperlong.h
  # binutils 2.46 の gas (x86) は行中の "/" をコメント開始として扱うため、
  # アセンブリ中の除算はシフトに置き換える (値は等価)
  sed -i -e 's|\$(nr/2)|\$(nr>>1)|g' \
         -e 's|\$(BITS_PER_LONG/8)|\$(BITS_PER_LONG>>3)|g' \
    arch/x86/include/asm/nospec-branch.h
  sed -i -e 's|KERNEL_IMAGE_SIZE/PMD_SIZE|KERNEL_IMAGE_SIZE>>PMD_SHIFT|' \
    arch/x86/kernel/head_64.S
  sed -i -e 's|\$(PARAM_SIZE/4)|\$(PARAM_SIZE>>2)|' \
         -e 's|\$(COMMAND_LINE_SIZE/4)|\$(COMMAND_LINE_SIZE>>2)|' \
    arch/x86/kernel/head_32.S
  sed -i 's|BOOT_INIT_PGT_SIZE/4|BOOT_INIT_PGT_SIZE>>2|' arch/x86/boot/compressed/head_64.S
  sed -i 's|4096/64|4096>>6|g' arch/x86/lib/copy_page_64.S
  sed -i -e 's|\$4096/8|\$4096>>3|g' -e 's|\$4096/64|\$4096>>6|g' \
    arch/x86/lib/clear_page_64.S arch/x86/lib/copy_page_64.S
  # efi libstub / compressed boot も KBUILD_CFLAGS を上書きするので gnu11 を明示
  sed -i 's|^KBUILD_CFLAGS := \$(filter-out \$(CC_FLAGS_SCS), \$(KBUILD_CFLAGS))$|&\nKBUILD_CFLAGS += -std=gnu11|' \
    drivers/firmware/efi/libstub/Makefile
  sed -i 's|^KBUILD_CFLAGS := -m\$(BITS) -O2 \$(CLANG_FLAGS)$|&\nKBUILD_CFLAGS += -std=gnu11|' \
    arch/x86/boot/compressed/Makefile
  sed -i 's|\.rept (j/8)|.rept (j>>3)|' arch/x86/crypto/crc32c-pcl-intel-asm_64.S
  sed -i 's|\.rept 80/2 + 1|.rept (80>>1) + 1|' \
    arch/x86/crypto/sha512-ssse3-asm.S arch/x86/crypto/sha512-avx-asm.S
  # boot/header.S も同じ理由 (PE ヘッダのセクション数は除算せずループで数える)
  sed -i 's|\.long\s*(section_table - \.) / 8|.long\t(section_table - .) >> 3|' \
    arch/x86/boot/header.S
  sed -i 's|^\t\.set\tsection_count, (\. - section_table) / 40$|\t.set\tsection_count, 0\n\t.rept\t16\n\t.if\t(. - section_table) >= (section_count + 1) * 40\n\t.set\tsection_count, section_count + 1\n\t.endif\n\t.endr|' \
    arch/x86/boot/header.S
  # elf-gcc は -m32 マルチリブを持っていないことが多い → 32bit 互換を外す
  if ! echo 'void f(void){}' | "$CROSS_GCC" -m32 -c -x c - -o /dev/null 2>/dev/null; then
    echo "    (-m32 非対応ツールチェーン → CONFIG_IA32_EMULATION を無効化)"
    bash scripts/config --disable IA32_EMULATION
  fi
fi
"$MAKE" "${MAKE_ARGS[@]}" olddefconfig

# ---------------------------------------------------------------- 3. ビルド
say "3/6 make -j$JOBS bzImage"
"$MAKE" "${MAKE_ARGS[@]}" bzImage
ls -lh arch/x86/boot/bzImage

# ------------------------------------------------------------- 4. initramfs
say "4/6 最小 initramfs (busybox) を作成"
bash "$REPO_ROOT/scripts/build-initramfs.sh"

# ----------------------------------------------------------- 5. QEMU ブートテスト
say "5/6 QEMU でブート確認 (最大300秒, TCG エミュレーションのため低速)"
QEMU="$(command -v qemu-system-x86_64)" || fail "qemu-system-x86_64 がありません (brew install qemu)"
TIMEOUT_CMD=timeout
command -v gtimeout >/dev/null && TIMEOUT_CMD=gtimeout
set +e
"$TIMEOUT_CMD" -k 10 300 "$QEMU" \
  -machine pc -vga std -m 512M -smp 2 -no-reboot \
  -kernel "$SRC/arch/x86/boot/bzImage" \
  -initrd "$INITRD" \
  -append "console=ttyS0 rdinit=/init panic=-1 loglevel=4 boottest" \
  -nographic -monitor none \
  > "$LOG" 2>&1
set -e

echo "---- シリアルコンソール出力 (先頭40行) ----"
head -40 "$LOG"
echo "-------------------------------------------"

if grep -q "ZHIBOX_BOOT_OK" "$LOG"; then
  echo "[OK] カーネルは QEMU 上で正常にブートしました。"
else
  echo "[NG] ブート確認に失敗しました。以下が全出力です:"
  cat "$LOG"
  exit 1
fi

# ------------------------------------------------------------ 6. 成果物の書き出し
say "6/6 成果物を output/ へコピー"
mkdir -p "$OUT"
cp -f "$SRC/arch/x86/boot/bzImage" "$OUT/bzImage"
cp -f "$SRC/.config"               "$OUT/config"
cp -f "$SRC/System.map"            "$OUT/System.map"
cp -f "$INITRD"                    "$OUT/initramfs.cpio.gz"
cp -f "$LOG"                       "$OUT/boot.log"
cp -f "$REPO_ROOT/scripts/boot-qemu.sh" "$OUT/boot-qemu.sh"
ls -lh "$OUT"

echo
echo "===== 完了 ====="
echo "bzImage : $OUT/bzImage"
echo "起動確認: bash scripts/boot-qemu.sh        (画面付き)"
echo "          SERIAL=1 bash scripts/boot-qemu.sh (シリアルコンソール)"
