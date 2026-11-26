import AppKit
import GestureCore

final class Preview: NSView {
    var frameImage: CameraFrame?
    var reach = hc_default_range()
    var firstCorner: HCPoint?
    override var isFlipped: Bool { true }
    override var isOpaque: Bool { true }

    override func draw(_ dirtyRect: NSRect) {
        NSColor(calibratedRed: 0.055, green: 0.08, blue: 0.12, alpha: 1).setFill()
        bounds.fill()
        guard let sample = frameImage else {
            let text = "Camera preview" as NSString
            let style: [NSAttributedString.Key: Any] = [.font: NSFont.systemFont(ofSize: 17, weight: .medium),
                                                       .foregroundColor: NSColor(white: 0.75, alpha: 1)]
            let size = text.size(withAttributes: style)
            text.draw(at: CGPoint(x: bounds.midX - size.width / 2, y: bounds.midY - size.height / 2), withAttributes: style)
            return
        }
        let w = CGFloat(sample.image.width), h = CGFloat(sample.image.height)
        let scale = min(bounds.width / w, bounds.height / h)
        let area = CGRect(x: bounds.midX - w * scale / 2, y: bounds.midY - h * scale / 2, width: w * scale, height: h * scale)
        NSImage(cgImage: sample.image, size: NSSize(width: w, height: h))
            .draw(in: area, from: .zero, operation: .copy, fraction: 1, respectFlipped: true, hints: nil)

        func at(_ p: HCPoint) -> CGPoint { CGPoint(x: area.minX + p.x * area.width, y: area.minY + p.y * area.height) }
        let top = at(HCPoint(x: reach.left, y: reach.top))
        let bottom = at(HCPoint(x: reach.right, y: reach.bottom))
        let box = NSBezierPath(roundedRect: CGRect(x: top.x, y: top.y, width: bottom.x - top.x, height: bottom.y - top.y), xRadius: 6, yRadius: 6)
        box.lineWidth = 1.5
        box.setLineDash([6, 5], count: 2, phase: 0)
        NSColor.white.withAlphaComponent(0.7).setStroke()
        box.stroke()
        if let p = firstCorner {
            NSColor.systemYellow.setFill()
            NSBezierPath(ovalIn: CGRect(origin: at(p), size: .zero).insetBy(dx: -6, dy: -6)).fill()
        }
        guard let points = sample.points else { return }
        let lines = NSBezierPath()
        for chain in [[0,1,2,3,4], [0,5,6,7,8], [5,9,13,17,0], [9,10,11,12], [13,14,15,16], [17,18,19,20]] {
            for (a,b) in zip(chain, chain.dropFirst()) where points[a].x >= 0 && points[b].x >= 0 {
                lines.move(to: at(points[a]))
                lines.line(to: at(points[b]))
            }
        }
        NSColor.systemMint.setStroke()
        lines.lineWidth = 2.5
        lines.stroke()
        for (i,p) in points.enumerated() where p.x >= 0 {
            let tip = [4,8,12].contains(i)
            (tip ? NSColor.systemYellow : NSColor.white).setFill()
            let r: CGFloat = tip ? 4.5 : 2.5
            NSBezierPath(ovalIn: CGRect(origin: at(p), size: .zero).insetBy(dx: -r, dy: -r)).fill()
        }
    }
}
