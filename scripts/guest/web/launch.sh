#!/bin/sh
#
# QEMU ゲスト内で WebKitGTK を起動し、HTML+CSS を raw RGBA に描画する。
# ゲストの init から chroot 経由で実行される (/opt/zhibox/launch.sh)。
#
#   launch.sh [output.raw]
#
# 環境変数:
#   ZH_HTML_WIDTH / ZH_HTML_HEIGHT  描画サイズ (既定 640x400)
#   ZH_HTML_LIVE=1                 常駐して /out/html.input の hover を反映
#
# Xvfb を使うので /dev/fb0 は fb-gradient が占有したままでよい。
# JavaScript は render-html.py 側で無効化している。

set -u

OUT="${1:-/out/html.raw}"
WIDTH="${ZH_HTML_WIDTH:-640}"
HEIGHT="${ZH_HTML_HEIGHT:-400}"
LIVE="${ZH_HTML_LIVE:-0}"

export HOME=/tmp
export TMPDIR=/tmp
export XDG_RUNTIME_DIR=/tmp/xdg
export XDG_CACHE_HOME=/tmp/cache
export XDG_CONFIG_HOME=/tmp/config
export DISPLAY=:99
export GDK_BACKEND=x11
export GSETTINGS_BACKEND=memory
export NO_AT_BRIDGE=1
export WEBKIT_DISABLE_COMPOSITING_MODE=1
export WEBKIT_DISABLE_DMABUF_RENDERER=1
# ゲストのカーネルは CONFIG_USER_NS 無効なので、WebKit の sandbox を切る。
# 2.40 以降はこの名前。古い版では無視される。
export WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1
export LIBGL_ALWAYS_SOFTWARE=1

mkdir -p "$XDG_RUNTIME_DIR" "$XDG_CACHE_HOME" "$XDG_CONFIG_HOME" \
         /tmp/.X11-unix
chmod 700 "$XDG_RUNTIME_DIR" 2>/dev/null || true
chmod 1777 /tmp/.X11-unix 2>/dev/null || true

# 前回の出力が残っていると、失敗時に古い画像を表示してしまう。
OUT_DIR=$(dirname "$OUT")
rm -f "$OUT" "$OUT_DIR/html.version" 2>/dev/null || true

Xvfb :99 -screen 0 "${WIDTH}x${HEIGHT}x24" -nolisten tcp \
    +extension GLX +render -noreset >/tmp/xvfb.log 2>&1 &
XVFB_PID=$!

cleanup() {
    kill "$XVFB_PID" 2>/dev/null || true
    wait "$XVFB_PID" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

i=0
while [ ! -S /tmp/.X11-unix/X99 ] && [ "$i" -lt 100 ]; do
    i=$((i + 1))
    sleep 0.1
done

if [ ! -S /tmp/.X11-unix/X99 ]; then
    echo "guest-html: Xvfb failed to start" >&2
    cat /tmp/xvfb.log >&2 2>/dev/null || true
    exit 1
fi

echo "guest-html: Xvfb :99 ${WIDTH}x${HEIGHT} live=${LIVE}"

# ゲスト起動を止めないよう、WebKitGTK には外側のタイムアウトも付ける。
# live モードは fb-gradient が終了するまで動かす必要があるので長めにする。
TIMEOUT=""
if command -v timeout >/dev/null 2>&1; then
    if [ "$LIVE" = 1 ]; then
        TIMEOUT="timeout -k 5 3600"
    else
        TIMEOUT="timeout -k 5 240"
    fi
fi

LIVE_ARG=""
[ "$LIVE" = 1 ] && LIVE_ARG="--live"

if command -v dbus-run-session >/dev/null 2>&1; then
    $TIMEOUT dbus-run-session -- \
        python3 /opt/zhibox/render-html.py \
            /opt/zhibox/index.html "$OUT" "$WIDTH" "$HEIGHT" $LIVE_ARG
else
    $TIMEOUT python3 /opt/zhibox/render-html.py \
        /opt/zhibox/index.html "$OUT" "$WIDTH" "$HEIGHT" $LIVE_ARG
fi
rc=$?

if [ "$rc" -ne 0 ]; then
    echo "guest-html: render failed (rc=$rc)" >&2
    cat /tmp/xvfb.log >&2 2>/dev/null || true
else
    echo "guest-html: wrote $OUT"
fi

exit "$rc"
