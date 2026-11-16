// Renders an SVG to a transparent PNG: swift render-svg.swift in.svg size out.png
import AppKit
let a = CommandLine.arguments
guard a.count == 4, let img = NSImage(contentsOf: URL(fileURLWithPath: a[1])), let n = Int(a[2]) else {
    FileHandle.standardError.write("cannot load \(a.dropFirst().first ?? "")\n".data(using: .utf8)!); exit(1)
}
let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: n, pixelsHigh: n, bitsPerSample: 8,
                           samplesPerPixel: 4, hasAlpha: true, isPlanar: false,
                           colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0)!
rep.size = NSSize(width: n, height: n)
NSGraphicsContext.saveGraphicsState()
NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)
NSGraphicsContext.current?.imageInterpolation = .high
img.draw(in: NSRect(x: 0, y: 0, width: n, height: n))
NSGraphicsContext.restoreGraphicsState()
try! rep.representation(using: .png, properties: [:])!.write(to: URL(fileURLWithPath: a[3]))
