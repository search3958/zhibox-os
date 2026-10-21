// render-html.swift - HTML+CSS を WebKit (WKWebView) でレンダリングし、
//                     JS を無効にしたまま raw RGBA (ZHRAW1) を書き出す。
//
//   macOS には WebKitGTK が無い (Homebrew の bottle は Linux のみ) ため、
//   同じ WebKit エンジンのシステムフレームワークを使う。
//
//   swiftc -O -framework Cocoa -framework WebKit render-html.swift -o render-html
//   ./render-html input.html output.raw [width] [height]
//
// 出力フォーマット (リトルエンディアン):
//   offset 0  : magic[8] = "ZHRAW1\0\0"
//   offset 8  : u32 width
//   offset 12 : u32 height
//   offset 16 : u32 format (0 = RGBA8888)
//   offset 20 : u32 stride (= width * 4)
//   offset 24 : RGBA pixels (top row first)
import Cocoa
import WebKit

func fail(_ message: String) -> Never {
    FileHandle.standardError.write(("render-html: " + message + "\n").data(using: .utf8)!)
    exit(1)
}

let args = CommandLine.arguments
guard args.count >= 3 else {
    fail("usage: render-html <input.html> <output.raw> [width] [height]")
}

let inputPath = args[1]
let outputPath = args[2]
let width = args.count > 3 ? (Int(args[3]) ?? 640) : 640
let height = args.count > 4 ? (Int(args[4]) ?? 400) : 400
guard width > 0 && height > 0 && width <= 8192 && height <= 8192 else {
    fail("invalid size \(width)x\(height)")
}
guard FileManager.default.fileExists(atPath: inputPath) else {
    fail("input not found: \(inputPath)")
}

func appendLE32(_ data: inout Data, _ value: UInt32) {
    var little = value.littleEndian
    withUnsafeBytes(of: &little) { data.append(contentsOf: $0) }
}

final class Renderer: NSObject, WKNavigationDelegate {
    let webView: WKWebView
    let window: NSWindow
    let inputPath: String
    let outputPath: String
    let width: Int
    let height: Int
    var done = false

    init(inputPath: String, outputPath: String, width: Int, height: Int) {
        self.inputPath = inputPath
        self.outputPath = outputPath
        self.width = width
        self.height = height

        let configuration = WKWebViewConfiguration()
        configuration.preferences.javaScriptEnabled = false
        if #available(macOS 11.0, *) {
            configuration.defaultWebpagePreferences.allowsContentJavaScript = false
        }
        configuration.preferences.plugInsEnabled = false
        configuration.preferences.javaEnabled = false

        let frame = NSRect(x: 0, y: 0, width: width, height: height)
        self.webView = WKWebView(frame: frame, configuration: configuration)
        self.window = NSWindow(contentRect: frame,
                               styleMask: [.borderless],
                               backing: .buffered,
                               defer: false)
        super.init()

        window.isReleasedWhenClosed = false
        window.contentView = webView
        window.backgroundColor = .black
        // 画面外に置いたままレンダリングさせる。ウィンドウサーバには
        // 登録されるが、ユーザーの画面には現れない。
        window.setFrameOrigin(NSPoint(x: -20000, y: -20000))
        window.orderFrontRegardless()
        webView.navigationDelegate = self
    }

    func start() {
        let url = URL(fileURLWithPath: inputPath)
        webView.loadFileURL(url, allowingReadAccessTo: url.deletingLastPathComponent())

        DispatchQueue.main.asyncAfter(deadline: .now() + 30) { [weak self] in
            guard let self = self, !self.done else { return }
            fail("timeout waiting for \(self.inputPath)")
        }
    }

    func webView(_ webView: WKWebView, didFinish navigation: WKNavigation!) {
        // CSS の適用とフォントの準備を少し待つ
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.8) { [weak self] in
            self?.snapshot()
        }
    }

    func webView(_ webView: WKWebView,
                 didFail navigation: WKNavigation!,
                 withError error: Error) {
        fail("navigation failed: \(error.localizedDescription)")
    }

    func webView(_ webView: WKWebView,
                 didFailProvisionalNavigation navigation: WKNavigation!,
                 withError error: Error) {
        fail("provisional navigation failed: \(error.localizedDescription)")
    }

    func snapshot() {
        let configuration = WKSnapshotConfiguration()
        configuration.rect = CGRect(x: 0, y: 0, width: width, height: height)
        configuration.snapshotWidth = NSNumber(value: width)

        webView.takeSnapshot(with: configuration) { [weak self] image, error in
            guard let self = self else { return }
            if let error = error {
                fail("snapshot failed: \(error.localizedDescription)")
            }
            guard let image = image else {
                fail("snapshot returned nil")
            }
            self.write(image)
            self.done = true
            NSApp.terminate(nil)
        }
    }

    func write(_ image: NSImage) {
        guard let rep = NSBitmapImageRep(bitmapDataPlanes: nil,
                                        pixelsWide: width,
                                        pixelsHigh: height,
                                        bitsPerSample: 8,
                                        samplesPerPixel: 4,
                                        hasAlpha: true,
                                        isPlanar: false,
                                        colorSpaceName: .deviceRGB,
                                        bytesPerRow: width * 4,
                                        bitsPerPixel: 32) else {
            fail("cannot allocate bitmap")
        }
        rep.size = NSSize(width: width, height: height)

        NSGraphicsContext.saveGraphicsState()
        guard let context = NSGraphicsContext(bitmapImageRep: rep) else {
            fail("cannot create bitmap context")
        }
        NSGraphicsContext.current = context
        NSColor.black.setFill()
        NSRect(x: 0, y: 0, width: width, height: height).fill()
        image.draw(in: NSRect(x: 0, y: 0, width: width, height: height),
                   from: .zero, operation: .copy, fraction: 1.0)
        context.flushGraphics()
        NSGraphicsContext.restoreGraphicsState()

        guard let base = rep.bitmapData else {
            fail("bitmap has no data")
        }
        let stride = rep.bytesPerRow
        var pixels = [UInt8](repeating: 0, count: width * height * 4)
        let alphaFirst = rep.bitmapFormat.contains(.alphaFirst)

        for y in 0..<height {
            let srcRow = base.advanced(by: y * stride)
            let dstRow = y * width * 4
            for x in 0..<width {
                let s = srcRow.advanced(by: x * 4)
                let d = dstRow + x * 4
                if alphaFirst {
                    pixels[d + 0] = s[1]
                    pixels[d + 1] = s[2]
                    pixels[d + 2] = s[3]
                    pixels[d + 3] = s[0]
                } else {
                    pixels[d + 0] = s[0]
                    pixels[d + 1] = s[1]
                    pixels[d + 2] = s[2]
                    pixels[d + 3] = s[3]
                }
            }
        }

        var out = Data()
        out.append(contentsOf: [0x5a, 0x48, 0x52, 0x41, 0x57, 0x31, 0x00, 0x00])
        appendLE32(&out, UInt32(width))
        appendLE32(&out, UInt32(height))
        appendLE32(&out, 0)
        appendLE32(&out, UInt32(width * 4))
        out.append(contentsOf: pixels)

        do {
            try out.write(to: URL(fileURLWithPath: outputPath))
        } catch {
            fail("cannot write \(outputPath): \(error.localizedDescription)")
        }
        print("render-html: \(width)x\(height) RGBA -> \(outputPath) (\(out.count) bytes)")
    }
}

let app = NSApplication.shared
app.setActivationPolicy(.accessory)
let renderer = Renderer(inputPath: inputPath,
                        outputPath: outputPath,
                        width: width,
                        height: height)
renderer.start()
app.run()
