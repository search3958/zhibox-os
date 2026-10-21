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

This is the WSL/Linux counterpart of scripts/host/darwin/render-html.swift.
It uses the WebKitGTK snapshot API, so no X server screenshot tool is needed.
"""

import argparse
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


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("input")
    parser.add_argument("output")
    parser.add_argument("width", nargs="?", type=int, default=640)
    parser.add_argument("height", nargs="?", type=int, default=400)
    args = parser.parse_args()

    if not os.path.isfile(args.input):
        fail("input not found: " + args.input)
    if args.width <= 0 or args.height <= 0:
        fail("invalid size %dx%d" % (args.width, args.height))

    Gdk, GLib, Gtk, WebKit2 = load_webkit()

    try:
        Gtk.init(None)
    except Exception as exc:
        fail("cannot connect to a display (%s); use xvfb-run" % exc)

    settings = WebKit2.Settings()
    settings.set_enable_javascript(False)
    for name, value in (
        ("set_enable_webgl", False),
        ("set_enable_plugins", False),
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

    window = Gtk.OffscreenWindow()
    window.set_default_size(args.width, args.height)
    view = WebKit2.WebView.new_with_settings(settings)
    view.set_size_request(args.width, args.height)
    window.add(view)
    window.show_all()

    finished = {"value": False, "scheduled": False}

    def write_surface(surface):
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
        with open(args.output, "wb") as handle:
            handle.write(header)
            handle.write(pixels)
        print("render-html: %dx%d RGBA -> %s (%d bytes)" %
              (width, height, args.output, len(header) + len(pixels)))

    def on_snapshot(view_, result, _data):
        try:
            surface = view_.get_snapshot_finish(result)
        except Exception as exc:
            fail("snapshot failed: %s" % exc)
        if surface is None:
            fail("snapshot returned nil")
        write_surface(surface)
        finished["value"] = True
        Gtk.main_quit()
        return False

    def take_snapshot():
        try:
            view.get_snapshot(WebKit2.SnapshotRegion.VISIBLE,
                              WebKit2.SnapshotOptions.NONE,
                              None, on_snapshot)
        except Exception as exc:
            fail("snapshot request failed: %s" % exc)
        return False

    def on_load_changed(_view, event):
        if event == WebKit2.LoadEvent.FINISHED and not finished["value"]:
            if not finished["scheduled"]:
                finished["scheduled"] = True
                # CSS / fonts の適用を少し待つ
                GLib.timeout_add(800, take_snapshot)
        return False

    def on_load_failed(_view, _event, uri, error):
        fail("failed to load %s: %s" % (uri, error.message))
        return False

    def on_timeout():
        if not finished["value"]:
            fail("timeout waiting for the page")
        return False

    view.connect("load-changed", on_load_changed)
    view.connect("load-failed", on_load_failed)
    view.load_uri(GLib.filename_to_uri(os.path.abspath(args.input), None))
    GLib.timeout_add_seconds(45, on_timeout)

    print("render-html: WebKit2-%s, JavaScript=%s" %
          (WebKit2.get_major_version(), settings.get_enable_javascript()),
          flush=True)
    Gtk.main()


if __name__ == "__main__":
    main()
