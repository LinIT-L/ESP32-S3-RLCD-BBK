#!/usr/bin/env swift
import AppKit
import Foundation

let W = 96, H = 96

let symbols: [(name: String, cvar: String, symbolSize: CGFloat)] = [
    ("keyboard",            "icon_key",  56),
    ("gamecontroller.fill", "icon_pad",  56),
    ("speaker.wave.2.fill", "icon_vol",  56),
    ("sdcard",              "icon_sd",   56),
    ("gearshape",           "icon_info", 56),
    ("music.note",          "icon_play", 56),
]

func renderBluetooth() -> [[UInt8]]? {
    let targetSize = NSSize(width: CGFloat(W), height: CGFloat(H))
    let finalImage = NSImage(size: targetSize)
    finalImage.lockFocus()
    NSColor.white.setFill()
    NSRect(origin: NSPoint(x: 0, y: 0), size: targetSize).fill()
    NSColor.black.setStroke()

    let cx = CGFloat(W) / 2
    let topY = CGFloat(16)
    let botY = CGFloat(80)
    let midY = CGFloat(48)
    let sideX = cx + 18
    let sideX2 = cx - 18

    let path = NSBezierPath()
    path.lineWidth = 3
    path.lineCapStyle = .round
    path.lineJoinStyle = .round

    path.move(to: NSPoint(x: cx, y: topY))
    path.line(to: NSPoint(x: cx, y: botY))

    path.move(to: NSPoint(x: cx, y: topY))
    path.line(to: NSPoint(x: sideX, y: midY - 12))
    path.line(to: NSPoint(x: cx, y: midY))

    path.move(to: NSPoint(x: cx, y: midY))
    path.line(to: NSPoint(x: sideX, y: midY + 12))
    path.line(to: NSPoint(x: cx, y: botY))

    path.move(to: NSPoint(x: cx, y: topY))
    path.line(to: NSPoint(x: sideX2, y: midY - 12))
    path.line(to: NSPoint(x: cx, y: midY))

    path.move(to: NSPoint(x: cx, y: midY))
    path.line(to: NSPoint(x: sideX2, y: midY + 12))
    path.line(to: NSPoint(x: cx, y: botY))

    path.stroke()
    finalImage.unlockFocus()

    guard let tiff = finalImage.tiffRepresentation,
          let rep = NSBitmapImageRep(data: tiff) else { return nil }
    var pixels = [[UInt8]](repeating: [UInt8](repeating: 0, count: W), count: H)
    for y in 0..<H {
        for x in 0..<W {
            if let nsColor = rep.colorAt(x: x, y: y) {
                let brightness = (nsColor.redComponent + nsColor.greenComponent + nsColor.blueComponent) / 3
                if brightness < 0.5 {
                    pixels[y][x] = 1
                }
            }
        }
    }
    return pixels
}

func renderSymbol(_ name: String, size: CGFloat) -> [[UInt8]]? {
    let config = NSImage.SymbolConfiguration(pointSize: size, weight: .regular)
    guard let baseImage = NSImage(systemSymbolName: name, accessibilityDescription: nil) else {
        return nil
    }
    let image = baseImage.withSymbolConfiguration(config)!
    let targetSize = NSSize(width: CGFloat(W), height: CGFloat(H))
    let finalImage = NSImage(size: targetSize)
    finalImage.lockFocus()
    NSGraphicsContext.current?.imageInterpolation = .none
    NSColor.white.setFill()
    NSRect(origin: NSPoint(x: 0, y: 0), size: targetSize).fill()
    NSColor.black.setFill()

    let imgSize = image.size
    let scale = min(CGFloat(W) / imgSize.width, CGFloat(H) / imgSize.height) * 0.80
    let drawW = imgSize.width * scale
    let drawH = imgSize.height * scale
    let drawX = (CGFloat(W) - drawW) / 2
    let drawY = (CGFloat(H) - drawH) / 2

    image.draw(in: NSRect(x: drawX, y: drawY, width: drawW, height: drawH),
               from: NSRect(x: 0, y: 0, width: imgSize.width, height: imgSize.height),
               operation: .sourceOver,
               fraction: 1.0)
    finalImage.unlockFocus()

    guard let tiff = finalImage.tiffRepresentation,
          let rep = NSBitmapImageRep(data: tiff) else { return nil }
    var pixels = [[UInt8]](repeating: [UInt8](repeating: 0, count: W), count: H)
    for y in 0..<H {
        for x in 0..<W {
            if let nsColor = rep.colorAt(x: x, y: y) {
                let r = nsColor.redComponent
                let g = nsColor.greenComponent
                let b = nsColor.blueComponent
                let brightness = (r + g + b) / 3
                if brightness < 0.5 {
                    pixels[y][x] = 1
                }
            }
        }
    }
    return pixels
}

func strokeOutlineThin(_ pixels: [[UInt8]]) -> [[UInt8]] {
    var result = pixels
    let dirs4 = [(1,0),(-1,0),(0,1),(0,-1)]
    for _ in 0..<2 {
        var next = result
        for y in 0..<H {
            for x in 0..<W {
                if result[y][x] == 0 {
                    for (dx, dy) in dirs4 {
                        let nx = x + dx, ny = y + dy
                        if nx >= 0 && nx < W && ny >= 0 && ny < H && result[ny][nx] == 1 {
                            next[y][x] = 1
                            break
                        }
                    }
                }
            }
        }
        result = next
    }
    return result
}

for sym in symbols {
    if var pixels = renderSymbol(sym.name, size: sym.symbolSize) {
        pixels = strokeOutlineThin(pixels)
        var hex = ""
        for y in 0..<H {
            for xByte in 0..<(W/8) {
                var byte: UInt8 = 0
                for bit in 0..<8 {
                    let x = xByte * 8 + bit
                    if pixels[y][x] == 1 {
                        byte |= UInt8(1 << (7 - bit))
                    }
                }
                hex += String(format: "%02X", byte)
            }
        }
        print("\(sym.cvar):\(hex)")
    } else {
        FileHandle.standardError.write("FAILED: \(sym.name)\n".data(using: .utf8)!)
    }
}

if var pixels = renderBluetooth() {
    pixels = strokeOutlineThin(pixels)
    var hex = ""
    for y in 0..<H {
        for xByte in 0..<(W/8) {
            var byte: UInt8 = 0
            for bit in 0..<8 {
                let x = xByte * 8 + bit
                if pixels[y][x] == 1 {
                    byte |= UInt8(1 << (7 - bit))
                }
            }
            hex += String(format: "%02X", byte)
        }
    }
    print("icon_bt:\(hex)")
} else {
    FileHandle.standardError.write("FAILED: bluetooth\n".data(using: .utf8)!)
}
