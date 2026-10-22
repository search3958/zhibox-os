#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Render HTML+CSS with WebKitGTK (JavaScript disabled) to raw RGBA.

Output format (little endian):
    offset 0  : magic[8] = "ZHRAW1\\0\\0"
    offset 8  : u32 width
    offset 12 : u32 height
    offset 16 : u32 format (0 = RGBA8888)
    offset 20 : u32 stride (= width * 4)
    offset 24 : RGBA pixels (top row first)

Live mode (--live):
    - keeps the Gtk window mapped under Xvfb
    - reads /out/html.input (``x y``) from fb-gradient
    - moves the X pointer with XTEST, so CSS :hover works
    - writes a new raw file atomically and bumps /out/html.version
    - exits when /out/html.stop appears

The one-shot mode is used by the serial boot verification; live mode is used
when the framebuffer desktop is running.
"""

import argparse
import ctypes
import os
import struct
import sys


def fail(message):
    print("render-html: " + message, file=sys.stderr)
    raise SystemExit(1)


def load_webkit():
    try:
        import gi
    except Exception as exc:  # pragma: no cover - depends on host packages
        fail("PyGObject is missing (%s)" % exc)

    gi.require_version("Gdk", "3.0")
    gi.require_version("Gtk", "3.0")
    version = None
    for candidate in ("4.1", "4.0"):
        try:
            gi.require_version("WebKit2", candidate)
            version = candidate
            break
        except ValueError:
            continue
    if version is None:
        fail("WebKit2 typelib not found "
             "(apt install gir1.2-webkit2-4.1 or gir1.2-webkit2-4.0)")

    from gi.repository import Gdk, GLib, Gtk, WebKit2
    return Gdk, GLib, Gtk, WebKit2


class XPointer:
    """Small ctypes binding for XTEST; avoids an xdotool dependency."""

    def __init__(self):
        self.x11 = None
        self.xtst = None
        self.display = None
        try:
            self.x11 = ctypes.CDLL("libX11.so.6")
            self.xtst = ctypes.CDLL("libXtst.so.6")
        except OSError as exc:
            print("render-html: XTEST unavailable (%s); hover disabled" % exc,
                  file=sys.stderr)
            return

        self.x11.XOpenDisplay.argtypes = [ctypes.c_char_p]
        self.x11.XOpenDisplay.restype = ctypes.c_void_p
        self.x11.XFlush.argtypes = [ctypes.c_void_p]
        self.x11.XFlush.restype = ctypes.c_int
        self.xtst.XTestFakeMotionEvent.argtypes = [
            ctypes.c_void_p, ctypes.c_int, ctypes.c_int,
            ctypes.c_int, ctypes.c_ulong]
        self.xtst.XTestFakeMotionEvent.restype = ctypes.c_int
        self.display = self.x11.XOpenDisplay(None)
        if not self.display:
            print("render-html: cannot open X display for XTEST",
                  file=sys.stderr)
            self.xtst = None

    def move(self, x, y):
        if not self.display or not self.xtst:
            return False
        self.xtst.XTestFakeMotionEvent(self.display, -1, int(x), int(y), 0)
        self.x11.XFlush(self.display)
        return True


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("input")
    parser.add_argument("output")
    parser.add_argument("width", nargs="?", type=int, default=640)
    parser.add_argument("height", nargs="?", type=int, default=400)
    parser.add_argument("--live", action="store_true",
                        help="keep rendering and track /out/html.input")
    args = parser.parse_args()

    if not os.path.isfile(args.input):
        fail("input not found: " + args.input)
    if args.width <= 0 or args.height <= 0:
        fail("invalid size %dx%d" % (args.width, args.height))

    out_dir = os.path.dirname(os.path.abspath(args.output))
    input_path = os.path.join(out_dir, "html.input")
    version_path = os.path.join(out_dir, "html.version")
    stop_path = os.path.join(out_dir, "html.stop")

    Gdk, GLib, Gtk, WebKit2 = load_webkit()

    try:
        Gtk.init(None)
    except Exception as exc:
        fail("cannot connect to a display (%s); use xvfb-run" % exc)

    settings = WebKit2.Settings()
    settings.set_enable_javascript(False)
    for name, value in (
        ("set_enable_webgl", False),
        ("set_enable_media", False),
        ("set_enable_developer_extras", False),
        ("set_enable_page_cache", False),
        ("set_enable_html5_database", False),
        ("set_enable_html5_local_storage", False),
    ):
        try:
            getattr(settings, name)(value)
        except AttributeError:
            pass

    # OffscreenWindow は X の入力イベントを受けないため、:hover を動かす
    # live モードでは通常のトップレベル窓を Xvfb 上に map する。
    window = Gtk.Window()
    window.set_decorated(False)
    window.set_resizable(False)
    window.set_default_size(args.width, args.height)
    view = WebKit2.WebView.new_with_settings(settings)
    view.set_size_request(args.width, args.height)
    window.add(view)
    window.show_all()
    window.move(0, 0)

    pointer = XPointer()
    # ページ左上の余白。ここには :hover 対象を置かない。
    pointer.move(1, 1)

    state = {
        "loaded": False,
        "busy": False,
        "pending": False,
        "source": 0,
        "frame": 0,
        "last_input": (None, None),
        "live": args.live,
    }

    def write_output(surface):
        width = surface.get_width()
        height = surface.get_height()
        if width <= 0 or height <= 0:
            fail("snapshot has invalid size %dx%d" % (width, height))

        pixbuf = Gdk.pixbuf_get_from_surface(surface, 0, 0, width, height)
        if pixbuf is None:
            fail("cannot convert the snapshot to a pixbuf")

        channels = pixbuf.get_n_channels()
        rowstride = pixbuf.get_rowstride()
        source = pixbuf.get_pixels()

        # WebKit のスナップショットは ARGB32 (4ch, padding なし) が普通。
        # TCG 上のゲストで毎フレーム 100 万画素を Python ループすると
        # hover が遅くなるため、その場合は bytes をそのまま使う。
        if channels == 4 and rowstride == width * 4:
            pixels = bytes(source)
        else:
            pixels = bytearray(width * height * 4)
            for y in range(height):
                src_row = y * rowstride
                dst_row = y * width * 4
                for x in range(width):
                    s = src_row + x * channels
                    d = dst_row + x * 4
                    pixels[d] = source[s]
                    pixels[d + 1] = source[s + 1]
                    pixels[d + 2] = source[s + 2]
                    pixels[d + 3] = source[s + 3] if channels == 4 else 255

        header = struct.pack("<8sIIII", b"ZHRAW1\x00\x00",
                             width, height, 0, width * 4)
        tmp = args.output + ".tmp"
        with open(tmp, "wb") as handle:
            handle.write(header)
            handle.write(pixels)
        os.replace(tmp, args.output)

        # fb-gradient はこの 4 バイトだけをポーリングし、変化した時だけ
        # 1MB の raw を読み直す。raw を先に rename するので tearing しない。
        state["frame"] = (state["frame"] + 1) & 0xffffffff
        tmp_version = version_path + ".tmp"
        with open(tmp_version, "wb") as handle:
            handle.write(struct.pack("<I", state["frame"]))
        os.replace(tmp_version, version_path)

        print("render-html: %dx%d RGBA -> %s (%d bytes) frame=%d" %
              (width, height, args.output, len(header) + len(pixels),
               state["frame"]), flush=True)

    def on_snapshot(view_, result, _data=None):
        state["busy"] = False
        try:
            surface = view_.get_snapshot_finish(result)
        except Exception as exc:
            fail("snapshot failed: %s" % exc)
        if surface is None:
            fail("snapshot returned nil")
        write_output(surface)
        if not state["live"]:
            Gtk.main_quit()
            return False
        if state["pending"]:
            state["pending"] = False
            schedule_snapshot(40)
        return False

    def request_snapshot():
        state["source"] = 0
        if not state["loaded"]:
            return False
        if state["busy"]:
            state["pending"] = True
            return False
        state["busy"] = True
        try:
            view.get_snapshot(WebKit2.SnapshotRegion.VISIBLE,
                              WebKit2.SnapshotOptions.NONE,
                              None, on_snapshot)
        except Exception as exc:
            state["busy"] = False
            fail("snapshot request failed: %s" % exc)
        return False

    def schedule_snapshot(delay_ms, replace=True):
        # 連続してマウスが動いている間も 1 回は撮れるよう、hover 入力では
        # 既に予約済みのタイマーを後ろへずらさない。
        if state["source"]:
            if not replace:
                return
            GLib.source_remove(state["source"])
        state["source"] = GLib.timeout_add(delay_ms, request_snapshot)

    def poll_live():
        if os.path.exists(stop_path):
            print("render-html: stop requested", flush=True)
            Gtk.main_quit()
            return False

        try:
            with open(input_path, "r") as handle:
                text = handle.read(64)
        except OSError:
            return True

        fields = text.split()
        if len(fields) < 2:
            return True
        try:
            x = int(fields[0])
            y = int(fields[1])
        except ValueError:
            return True
        if (x, y) == state["last_input"]:
            return True
        state["last_input"] = (x, y)

        if x < 0 or y < 0:
            # ページ外: hover を解除するため左上の余白へ逃がす。
            pointer.move(1, 1)
        else:
            pointer.move(x, y)
        # CSS transition (170-180ms) が終わった頃に 1 枚撮る。
        schedule_snapshot(250, replace=False)
        return True

    def on_load_changed(_view, event):
        if event == WebKit2.LoadEvent.FINISHED and not state["loaded"]:
            state["loaded"] = True
            # CSS / フォントの適用を待つ (TCG のゲストでも余裕を持たせる)
            schedule_snapshot(1000 if state["live"] else 1500)
            if state["live"]:
                GLib.timeout_add(50, poll_live)
        return False

    def on_load_failed(_view, _event, uri, error):
        fail("failed to load %s: %s" % (uri, error.message))
        return False

    def on_timeout():
        fail("timeout waiting for the page")
        return False

    view.connect("load-changed", on_load_changed)
    view.connect("load-failed", on_load_failed)
    view.load_uri(GLib.filename_to_uri(os.path.abspath(args.input), None))
    # WebKitGTK の初回起動は TCG エミュレーション上で時間がかかる。
    GLib.timeout_add_seconds(3600 if state["live"] else 180, on_timeout)

    print("render-html: WebKit2-%s.%s, JavaScript=%s, live=%s" %
          (WebKit2.get_major_version(), WebKit2.get_minor_version(),
           settings.get_enable_javascript(), state["live"]),
          flush=True)
    Gtk.main()


if __name__ == "__main__":
    main()
