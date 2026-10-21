# zhibox-os — openEuler カーネル (OLK-5.10) ブータブル環境

`https://gitee.com/openeuler/kernel.git` の **OLK-5.10** を取得し、
**QEMU 上で実際にブートできる**状態まで構築したワークスペース。

ビルドは **WSL (Ubuntu)** と **macOS (Apple Silicon, ネイティブ)** の両方に対応しています。
スクリプトが `uname` で自動判定して分岐します。Linux コンテナや VM は使いません。

## 構成

| パス | 内容 | サイズ |
|---|---|---|
| `kernel/` | カーネルソース (74,490ファイル) | 1.1GB |
| `kernel.git/` | クローンの git 履歴（shallow, `OLK-5.10` @ `1cd35e303`） | 219MB |
| `scripts/` | ビルド・起動・検証スクリプト | 数KB |
| `output/` | ビルド成果物（`bzImage`, `config`, `System.map`, `initramfs.cpio.gz`, `boot.log`）※`.gitignore` 対象 | 17MB |
| `README.md` / `.gitignore` / `build.log` | ドキュメント・設定・ビルドログ | — |

> **レイアウトの注意**: `kernel/.git` は `kernel.git/` に分離してあります。
> リポジトリに「ファイルとして」カーネルを含めるため（同居すると git が
> サブモジュール化してファイル本体をコミットしなくなるため）。
> git 操作は必ず `--git-dir` と `--work-tree` を同時に指定します:

```powershell
# Windows (PowerShell / git for Windows)
git --git-dir=kernel.git --work-tree=kernel status
git --git-dir=kernel.git --work-tree=kernel log --oneline -5
git --git-dir=kernel.git --work-tree=kernel diff
```
```bash
# WSL 内ならラッパーで自動付与
bash scripts/tools/k.sh status
bash scripts/tools/k.sh log --oneline -5
```

## 必要な環境

### WSL: Ubuntu 22.04

- vhdx は `D:\wsl\Ubuntu-22.04`（C: の空きを守るため D: 配置）
- gcc 11.4 / make / flex / bison / libelf / libssl / bc / cpio / dwarves / rsync / git
- **qemu-system-x86_64** + **qemu-system-gui**（画面表示用 GTK/SDL）
- **busybox-static**（initramfs 用 = distro パッケージ）
- ビルドツリーは WSL の ext4 上 `~/kernel-src` に展開して使用
  （Windows の 9p マウントは遅く、かつ大文字小文字の制約を受けるため）

### macOS: Apple Silicon（ネイティブ、コンテナ不要）

```bash
brew install x86_64-elf-gcc x86_64-elf-binutils qemu make gnu-sed gawk coreutils findutils
```

- カーネルはフリースタンディングなので、`*-elf-gcc`（ベアメタル向け）でクロスコンパイルできる
- APFS は大文字小文字を区別しないため、**ケースセンシティブ APFS のスパースバンドル**
  (`~/zhibox-kernel.sparsebundle` → `/Volumes/zhibox-kernel`) を初回に自動作成し、
  その中の `/Volumes/zhibox-kernel/src` でビルドする（sudo 不要・使用分だけ拡張）
- ホストツール用に `scripts/host/darwin/` の `elf.h` / `byteswap.h` / `endian.h` を
  差し込むほか、新しめの GCC/binutils 向けの小さなソース修正をスクリプト内で自動適用する
  （すべて冪等。詳細は `scripts/build-and-boot.sh` の macOS ブロック）
- initramfs の `gradient`（フレームバッファ描画デモ）も macOS で生成できる。Linux 用の
  静的 libc が無いため、`scripts/host/darwin/linux-shim/`（syscall 直叩きの最小 libc 代替）と
  `x86_64-elf-gcc` で freestanding な静的 ELF としてクロスビルドする（追加依存なし）
- QEMU は TCG（ソフトウェアエミュレーション）で動くためブートに数分かかる

## ビルド + 起動確認（1コマンド）

```bash
# WSL / macOS 共通 (リポジトリ直下で実行)
bash scripts/build-and-boot.sh
```

```powershell
# Windows からは WSL 経由 (パスは環境に合わせて)
wsl -d Ubuntu-22.04 -u root -e bash /mnt/c/.../zhibox-os/scripts/build-and-boot.sh
```

1. git オブジェクトから**完全な**ツリーを展開（macOS はスパースバンドルをマウント）
2. `make defconfig` → 3. `make -j bzImage` → 4. busybox initramfs 生成
5. **QEMU でブート確認**（`ZHIBOX_BOOT_OK` を判定し自動電源オフ）→ 6. `output/` へ書き出し

- 2回目以降はツリーを再利用し差分のみ
- リポジトリ側の編集を取り込む: `SYNC=1 bash scripts/build-and-boot.sh`（`--size-only` 高速同期）
- ツリー作り直し: `bash scripts/host/wsl/reextract.sh`
- 途中経過: 別窓から `tail -f output/build.log`

## 起動方法

**画面付き（GUI ウィンドウ）** — WSL は WSLg 経由、macOS はネイティブ QEMU ウィンドウ:

```bash
bash scripts/boot-gui.sh
```

GUI 起動時はフレームバッファのデモ (`gradient`) が自動で起動します。

- **マウス**: QEMU ウィンドウ内をクリックしてポインタをグラブ（PS/2 マウス）
- **ウィンドウ**: タイトルバーをドラッグで移動、クリックで最前面へ
- **終了**: `q` / `Esc` / `Ctrl+C` でデモを終了し、busybox シェルに戻る

### GPU アクセラレーション (virtio-gpu)

GUI 起動では QEMU の **virtio-gpu** (`-vga virtio`) を使います。

- カーネルは `CONFIG_VIRTIO_PCI` / `CONFIG_DRM_VIRTIO_GPU` を有効化
- `gradient` は `/dev/fb0`（virtio-gpu の GEM バッファ）に描画し、
  DRM の `DIRTYFB` で**変更された矩形だけ**をホストへ転送する
  （bochs の VRAM への MMIO 書き込みより大幅に速い）
- `DIRTYFB` に対応していないドライバでは、自動的に従来の直接描画へフォールバック
- 起動ログに `gradient: drm acceleration on (virtio-gpu)` と出れば加速パスが有効
- なお Homebrew の QEMU (macOS) は virglrenderer 非対応のため、
  OpenGL/3D のハードウェアアクセラレーションは対象外（2D の転送高速化まで）

**シリアルコンソール（対話・-nographic）**:

```bash
bash scripts/boot-qemu.sh
```

- シリアル起動ではデモは自動起動しません（`gradient` と入力すると起動、`q` で終了）
- どちらも `/` に着地し busybox シェルが待機（`Linux (none) 5.10.0 ...`）
- 終了: 画面の `poweroff -f`、または `Ctrl+A` → `X`（nographic 時）
- macOS は TCG のため起動に数分かかる（スクリプトは最大300秒待つ）

## 運用上の注意

### 1. git の取得元

gitee 直結のクローンはこの回線で**パック転送が必ず途中切断**する（`early EOF` を3回確認）ため、
openEuler 公式ミラー **`github.com/openeuler-mirror/kernel`** から同一ブランチを取得した。

```
origin  = https://gitee.com/openeuler/kernel.git        # ご要望の取得元
mirror  = https://github.com/openeuler-mirror/kernel.git # 実際に取得した公式ミラー
```

gitee が安定するまでは `git --git-dir=kernel.git --work-tree=kernel fetch --depth 1 mirror OLK-5.10` 推奨。

### 2. 大文字小文字非区別ファイルシステムによる 13 ファイルの欠落 (NTFS / APFS 共通)

カーネルには `xt_CONNMARK.h` / `xt_connmark.h` のような**大文字小文字だけ違う組が13組（26ファイル）**あり、
Windows の NTFS も macOS の APFS（既定）も非区別のため **`kernel/` には片方しか存在できない**（この環境では区別ディレクトリ化も無効だった）。

- `kernel/` は**参照用**（13ファイル欠落）。`git status` が怒らないよう元のリポジトリ側で `skip-worktree` 設定済み
- **完全な26ファイルは `kernel.git/` のオブジェクトに保存されており**、ビルドはそこから展開するためビルド結果には影響しない
- 完全なツリーの実体: WSL は `~/kernel-src`（ext4）、macOS は `/Volumes/zhibox-kernel/src`
  （ケースセンシティブ APFS スパースバンドル。スクリプトが初回に自動作成・マウント）

### 3. コミット / push について

- 本リポジトリは **ローカルコミットのみ**（push 未実施）。
- `kernel.git/objects/pack/*.pack` は **約208MB の単一ファイル**で、**GitHub の 100MB 上限を超える**。
  push したい場合は、先に `/kernel.git/` を `.gitignore` に追加して除外するか、履歴を持ち込まないこと。
- カーネルソース一式は約1.3GB。push は非推奨（GitHub の推奨は1GB以下・警告は5GB）。

### 4. ソースを変更する場合

- ビルドするならビルド用ツリー側で編集するのが確実
  （WSL: `~/kernel-src`、macOS: `/Volumes/zhibox-kernel/src`）
- リポジトリ側 `kernel/` で編集した場合: `SYNC=1` 付きでビルドスクリプトを再実行すると取り込む

## スクリプト一覧

| パス | 用途 |
|---|---|
| `scripts/build-and-boot.sh` | ビルド → initramfs → QEMU ブート確認 → 成果物書き出し |
| `scripts/build-initramfs.sh` | busybox + gradient の initramfs を生成 |
| `scripts/boot-qemu.sh` | シリアルコンソールで対話起動 |
| `scripts/boot-gui.sh` | **画面付き (GUI)** で起動（WSL は WSLg / macOS はネイティブ） |
| `scripts/lib/common.sh` | OS 判定 (WSL/macOS)・パス・ツール検出の共通処理 |
| `scripts/guest/` | **ゲスト (Linux) 側**で動くもの: `init` (PID 1) / `fb-gradient.c` |
| `scripts/host/darwin/` | **macOS 専用**: ホスト用ヘッダ (`elf.h` 等) / `linux-shim/` |
| `scripts/host/wsl/` | **WSL 専用**: NTFS・大文字小文字衝突まわりの補助スクリプト |
| `scripts/tools/` | ホスト共通ツール: `k.sh` / `case-collision-report.sh` |

> ディレクトリ構成の詳細と、新しいスクリプトを置く場所は `scripts/README.md` を参照。
