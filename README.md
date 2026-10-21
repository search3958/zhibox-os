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
- HTML+CSS ウィンドウのレンダリング用（任意）:
  `python3-gi` / `gir1.2-webkit2-4.0` / `libwebkit2gtk-4.0-37` / `xvfb`
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
- HTML+CSS のレンダリングは **システムの WebKit (WKWebView)** を使う
  （WebKitGTK は Linux 専用のため）。`swiftc`（Xcode Command Line Tools）が必要
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

- **壁紙**: `wallpaper.png` を画面いっぱい (cover) に縮小表示
- **文字**: `HarmonyOS_Sans_SC_Regular.ttf` を TrueType レンダリングで描画
- **マウス**: QEMU ウィンドウ内をクリックしてポインタをグラブ（PS/2 マウス）
- **ウィンドウ**: タイトルバーをドラッグで移動、クリックで最前面へ
- **HTML+CSS**: 4 番目の `WEBKIT` ウィンドウに表示（WebKit / JavaScript 無効）
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

### 壁紙 (PNG) と TrueType フォント

ゲスト側で自前のデコーダを使うため、libpng / zlib / libfreetype は不要です
（macOS の freestanding クロスビルドでもそのまま動きます）。

| ホスト側のファイル | initramfs 内のパス | 用途 |
|---|---|---|
| `scripts/guest/wallpaper.png` | `/usr/share/zhibox/wallpaper.png` | 壁紙 |
| `scripts/guest/HarmonyOS_Sans_SC_Regular.ttf` | `/usr/share/zhibox/font.ttf` | UI フォント |

- **PNG**: 8bit・非インターレースの color type 0/2/3/4/6 に対応。
  zlib の inflate と PNG フィルタ復元を内蔵し、RGBA に展開後、
  アスペクト比を保って画面を覆うようにバイリニア縮小（約 80% に減光）
- **TrueType**: `head` / `maxp` / `hhea` / `hmtx` / `cmap` (format 4/12) /
  `loca` / `glyf` を読み、2 次ベジェを整数 26.6 固定小数点で
  4 倍垂直スーパーサンプリング + 水平被覆率により描画
- フォントや壁紙が見つからない場合は、従来のグラデーション背景と
  5x7 ビットマップフォントに自動フォールバック
- 起動ログで確認できます:
  ```
  gradient: font loaded (TrueType)
  gradient: wallpaper 1920x1080 (PNG)
  ```
- **差し替え方法**: 上記 2 ファイルを置き換えて initramfs だけ再生成すれば
  反映されます（カーネルの再ビルドは不要）:
  ```bash
  bash scripts/build-initramfs.sh
  cp ~/initramfs.cpio.gz output/initramfs.cpio.gz   # GUI 起動が参照する場所
  bash scripts/boot-gui.sh
  ```

### HTML+CSS ウィンドウ (WebKit / JavaScript 無効)

`scripts/guest/web/index.html` を WebKit でレンダリングし、**PNG を介さず
raw RGBA のまま** initramfs に同梱して、デスクトップの 4 番目のウィンドウ
(`WEBKIT`) に表示します。

```
HTML+CSS --[WebKitGTK (WSL) / WKWebView (macOS), JS off]--> raw RGBA
         --[initramfs]--> /usr/share/zhibox/html.raw
         --[gradient]--> バイリニア縮小 --> /dev/fb0 の WEBKIT ウィンドウ
```

- **レンダリング**: 既定 640x400。ウィンドウ本文 (約 500x313) に
  アスペクト比を保って縮小するため、文字が読めるサイズになる
- **JavaScript 無効**: WebKitGTK は `WebKitSettings:enable-javascript=FALSE`、
  macOS は `WKWebpagePreferences.allowsContentJavaScript = NO`。さらに
  HTML 側の CSP でも script を禁止。ページ内の `<noscript>` バッジで確認できる
- **macOS と WSL の違い**: WebKitGTK は Linux 専用 (Homebrew の bottle も
  Linux のみ) のため、macOS は同じ WebKit エンジンのシステムフレームワーク
  (WKWebView) を使う。WSL は WebKitGTK を使う
- **静的スナップショット**: ビルド時にレンダリングする。ページ内のスクロールや
  リンク操作はできないが、ウィンドウのドラッグ・前面化は通常どおり動く

**WSL の依存パッケージ** (初回のみ):

```bash
sudo apt install python3-gi python3-gi-cairo gir1.2-gtk-3.0 \
    gir1.2-webkit2-4.0 libwebkit2gtk-4.0-37 xvfb fonts-dejavu-core
# Ubuntu 24.04 以降は gir1.2-webkit2-4.1 / libwebkit2gtk-4.1-0
```

**単体でレンダリング / 確認**:

```bash
bash scripts/render-html.sh            # -> output/html.raw
bash scripts/render-html.sh /tmp/x.raw # 出力先を指定
```

`build-initramfs.sh` は自動で `render-html.sh` を呼びます。WebKit が無い環境では
警告を出して続行し、ゲスト側は `HTML RENDER UNAVAILABLE` のプレースホルダーに
なります。レンダリングをスキップしたい場合は `ZH_HTML_RENDER=0`、
既存の raw を使う場合は `ZH_HTML_RAW=/path/to/html.raw` を指定してください。

```bash
bash scripts/build-initramfs.sh
cp ~/initramfs.cpio.gz output/initramfs.cpio.gz
bash scripts/boot-gui.sh
```

起動ログに次のように出れば、raw の読み込みと縮小キャッシュが成功しています:

```
gradient: html 640x400 (raw RGBA, JS off)
gradient: html 640x400 -> 500x313 (raw RGBA)
```

raw 形式 (リトルエンディアン):

| オフセット | 内容 |
|---|---|
| 0 | magic 8 bytes `ZHRAW1\0\0` |
| 8 | u32 width |
| 12 | u32 height |
| 16 | u32 format (0 = RGBA8888) |
| 20 | u32 stride (bytes) |
| 24 | RGBA 画素 (先頭が上端) |

- ページを変更したら `bash scripts/build-initramfs.sh` を再実行する
  （カーネルの再ビルドは不要）
- 解像度を変える: `ZH_HTML_WIDTH=800 ZH_HTML_HEIGHT=500 bash scripts/render-html.sh`
  （ウィンドウ側は自動でフィットする）

### シリアルコンソール（対話・-nographic）

```bash
bash scripts/boot-qemu.sh
```

- シリアル起動ではデモは自動起動しません（`gradient` と入力すると起動、`q` + Enter で終了）
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
| `scripts/build-initramfs.sh` | busybox + gradient + WebKit raw の initramfs を生成 |
| `scripts/render-html.sh` | HTML+CSS を WebKit で raw RGBA にレンダリング (WSL/macOS 分岐) |
| `scripts/boot-qemu.sh` | シリアルコンソールで対話起動 |
| `scripts/boot-gui.sh` | **画面付き (GUI)** で起動（WSL は WSLg / macOS はネイティブ） |
| `scripts/lib/common.sh` | OS 判定 (WSL/macOS)・パス・ツール検出の共通処理 |
| `scripts/guest/` | **ゲスト (Linux) 側**で動くもの: `init` (PID 1) / `fb-gradient.c` / `zh_font.*` (TrueType) / `zh_png.*` (PNG) / `web/index.html` / フォント・壁紙アセット |
| `scripts/host/darwin/` | **macOS 専用**: ホスト用ヘッダ (`elf.h` 等) / `linux-shim/` / `render-html.swift` (WKWebView) |
| `scripts/host/wsl/` | **WSL 専用**: NTFS・大文字小文字衝突まわりの補助 / `render-html.py` (WebKitGTK) |
| `scripts/tools/` | ホスト共通ツール: `k.sh` / `case-collision-report.sh` |

> ディレクトリ構成の詳細と、新しいスクリプトを置く場所は `scripts/README.md` を参照。
