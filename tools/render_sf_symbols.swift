// Render SF Symbols to PNG using AppKit
// Usage: swift render_sf_symbols.swift <output_dir>
//   reads <output_dir>/sf_symbols.txt for (name symbol size) entries
import AppKit
import Foundation

let args = CommandLine.arguments
guard args.count >= 2 else {
    print("Usage: swift render_sf_symbols.swift <output_dir>")
    exit(1)
}
let outputDir = args[1]
let inputFile = "\(outputDir)/sf_symbols.txt"

guard let content = try? String(contentsOfFile: inputFile, encoding: .utf8) else {
    print("Cannot read: \(inputFile)")
    exit(1)
}

let lines = content.split(separator: "\n")
for line in lines {
    let trimmed = line.trimmingCharacters(in: .whitespaces)
    if trimmed.isEmpty || trimmed.hasPrefix("#") { continue }
    let parts = trimmed.split(separator: " ", maxSplits: 2, omittingEmptySubsequences: true)
    if parts.count < 2 { continue }
    let name = String(parts[0])
    let symbol = String(parts[1])
    let weight = parts.count >= 3 ? String(parts[2]) : "regular"
    let size: CGFloat = parts.count >= 4 ? (CGFloat(Double(parts[3]) ?? 96.0)) : 96.0
    let weightVal: NSFont.Weight
    switch weight {
    case "ultralight": weightVal = .ultraLight
    case "thin":       weightVal = .thin
    case "light":      weightVal = .light
    case "regular":    weightVal = .regular
    case "medium":     weightVal = .medium
    case "semibold":   weightVal = .semibold
    case "bold":       weightVal = .bold
    case "heavy":      weightVal = .heavy
    case "black":      weightVal = .black
    default:           weightVal = .regular
    }
    let config = NSImage.SymbolConfiguration(pointSize: size * 0.85, weight: weightVal)
    guard let img = NSImage(systemSymbolName: symbol, accessibilityDescription: nil)?
        .withSymbolConfiguration(config) else {
        print("FAIL: \(symbol)")
        continue
    }
    // 渲染到白色背景
    let final = NSImage(size: NSSize(width: size, height: size))
    final.lockFocus()
    NSColor.white.setFill()
    NSBezierPath(rect: NSRect(x: 0, y: 0, width: size, height: size)).fill()
    let imgSize = img.size
    let drawSize = min(size, imgSize.width, imgSize.height)
    let drawRect = NSRect(
        x: (size - drawSize) / 2,
        y: (size - drawSize) / 2,
        width: drawSize,
        height: drawSize
    )
    img.draw(in: drawRect, from: .zero, operation: .sourceOver, fraction: 1.0)
    final.unlockFocus()
    guard let tiff = final.tiffRepresentation,
          let bitmap = NSBitmapImageRep(data: tiff),
          let png = bitmap.representation(using: .png, properties: [:]) else { continue }
    let outPath = "\(outputDir)/\(name).png"
    try? png.write(to: URL(fileURLWithPath: outPath))
    print("OK: \(outPath) (symbol: \(symbol))")
}
