# gradient — zhibox GUI umbrella

`gradient` は zhibox の GUI 総称。baram-os の GUI スタック (compositor /
window manager / IME / Warp4 / SVG カーソル / 設定アプリ) を Linux ネイティブ
の実行環境へ移植した一式をまとめている。

## 構成

| パス | 内容 |
| --- | --- |
| `baram-bsd/` | URI (`os://…`) / 設定保存 / VFS / セキュリティ周りの BSD shim。`config.xml` はここに置いた**実使用キーのみ**のスリム版 (`display/gpu/enabled` を含む) |
| `baram-iokit/` | evdev (キーボード/マウス) → baram IOKit 互換入力 shim |
| `nano-system/` | ブートランタイム用の nano system shim (CRT / alloc / clock) |
| `runner/` | `crates/baram-boot` 用の Linux Cargo.toml (`/bin/desktop` にビルド) |
| `uefi/` | UEFI API を最小限に再現した Linux shim (crate 名維持のため) |
| `warp4/` | 旧 gradient / WebKit フローのフォールバック (`/bin/warp4`) |

## Linux ネイティブの要点

- **プラットフォームは Linux 専用**。macOS/WSL はホスト側のクロスビルド経由のみ
  であり、ゲスト内の実行パスは `/dev/fb0` + evdev + DRM (`DRM_IOCTL_MODE_DIRTYFB`)
  の直接使用のみ。
- GPU 表示モードは `display/gpu/enabled` で切替:
  - **有効**: virtio-gpu に変更範囲を DRM dirty rectangle で通知し、壁紙ブラーを表示する。
  - **無効**: CPU 描画の全画面更新に切り替える。壁紙はそのまま表示し、タスクバーやランチャーの背景ブラーだけを止める。
- 現在の virtio-gpu 2D 経路はコンポジションを CPU で行い、GPU/DRM に変更範囲を渡して表示する。virgl 3D レンダリングには対応していない。
- URI コマンドはアプリ内 (`WarpUI.OnClick` → `os://display/gpu?enabled=…` /
  設定アプリ) からはもちろん、ゲスト側からは `zh_uri=os://…` カーネル引数
  (ワンショット) または `echo os://… > /tmp/zhibox.uri` でも実行できる。
- 仮想メモリ / swap は使わない (`CONFIG_SWAP=n` のカーネル + swapon なし)。

## 設定 (config.xml) の方針

GUI 実行時に `baram-bsd` が参照するキーのみを持ち、GUI が書き込むキーは
`config.set()` が動的に作るため、将来の追加も破損しない (足りないキーは既定値で
フォールバック)。変更はこの README と `uri.rs` のドキュメントコメントに追記して
から反映する。
