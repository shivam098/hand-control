import AppKit
import GestureCore

final class CursorHalo {
    private let panel: NSPanel
    private let ring = HaloView(frame: NSRect(x: 0, y: 0, width: 64, height: 64))
    var isEnabled: Bool = true {
        didSet { if !isEnabled { hide() } }
    }

    init() {
        panel = NSPanel(contentRect: ring.bounds, styleMask: [.borderless, .nonactivatingPanel],
                        backing: .buffered, defer: true)
        panel.isOpaque = false
        panel.backgroundColor = .clear
        panel.hasShadow = false
        panel.ignoresMouseEvents = true
        panel.level = .statusBar
        panel.hidesOnDeactivate = false
        panel.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary, .stationary, .ignoresCycle]
        panel.contentView = ring
    }

    func update(_ out: HCOutput, enabled: Bool) {
        guard isEnabled, enabled else {
            hide()
            return
        }
        switch out.mode {
        case HC_SEARCH, HC_CAL_FIRST, HC_CAL_SECOND, HC_CAL_DONE:
            hide()
            return
        default:
            break
        }
        guard let screen = NSScreen.screens.first else {
            hide()
            return
        }
        let frame = screen.frame
        let x = frame.minX + CGFloat(out.position.x) * frame.width
        let y = frame.maxY - CGFloat(out.position.y) * frame.height
        panel.setFrameOrigin(NSPoint(x: round(x - ring.bounds.width / 2),
                                     y: round(y - ring.bounds.height / 2)))
        ring.state = HaloState(mode: out.mode, action: out.action, clickCount: Int(out.clickCount),
                               pinchLevel: CGFloat(out.pinchLevel), quality: CGFloat(out.quality),
                               locked: out.locked != 0)
        if !panel.isVisible { panel.orderFrontRegardless() }
    }

    func hide() {
        if panel.isVisible { panel.orderOut(nil) }
    }
}

private struct HaloState: Equatable {
    var mode: HCMode = HC_SEARCH
    var action: HCAction = HC_NO_EVENT
    var clickCount: Int = 1
    var pinchLevel: CGFloat = 0
    var quality: CGFloat = 1
    var locked: Bool = false
}

private final class HaloView: NSView {
    var state = HaloState() {
        didSet { if state != oldValue { needsDisplay = true } }
    }

    override var isOpaque: Bool { false }

    override func draw(_ dirtyRect: NSRect) {
        let center = CGPoint(x: bounds.midX, y: bounds.midY)
        let highContrast = NSWorkspace.shared.accessibilityDisplayShouldIncreaseContrast
        let alphaScale: CGFloat = highContrast ? 1.0 : max(0.45, state.quality)

        if state.mode == HC_PAUSE {
            let r: CGFloat = 16
            let path = NSBezierPath(ovalIn: CGRect(x: center.x - r, y: center.y - r, width: r * 2, height: r * 2))
            path.lineWidth = highContrast ? 3.0 : 2.0
            path.setLineDash([4, 4], count: 2, phase: 0)
            NSColor.systemOrange.withAlphaComponent(0.75).setStroke()
            path.stroke()
            return
        }

        let isDrag = state.mode == HC_DRAG
        let isRight = state.mode == HC_RIGHT_PINCH || state.action == HC_RIGHT_CLICK
        let isTriple = state.mode == HC_DOUBLE_PINCH || (state.action == HC_CLICK && state.clickCount == 2)
        let isScroll = state.mode == HC_SCROLL

        let radius: CGFloat
        if isDrag {
            radius = 13
        } else if isScroll {
            radius = 17
        } else {
            radius = 19 - 8 * min(1, max(0, state.pinchLevel))
        }

        let strokeColor: NSColor
        if isDrag || isTriple {
            strokeColor = .systemYellow
        } else if isRight {
            strokeColor = .systemCyan
        } else if state.quality < 0.55 {
            strokeColor = .systemOrange
        } else {
            strokeColor = .systemMint
        }

        let rect = CGRect(x: center.x - radius, y: center.y - radius, width: radius * 2, height: radius * 2)
        if !highContrast {
            let shadowRing = NSBezierPath(ovalIn: rect)
            shadowRing.lineWidth = 4.5
            NSColor.black.withAlphaComponent(0.28 * alphaScale).setStroke()
            shadowRing.stroke()
        }

        if isDrag {
            NSColor.systemYellow.withAlphaComponent(highContrast ? 0.35 : 0.22).setFill()
            NSBezierPath(ovalIn: rect).fill()
        }

        let ringPath = NSBezierPath(ovalIn: rect)
        ringPath.lineWidth = highContrast ? 3.2 : (isDrag ? 3.0 : 2.2)
        strokeColor.withAlphaComponent(0.92 * alphaScale).setStroke()
        ringPath.stroke()

        if state.locked || isDrag {
            let dotR: CGFloat = isDrag ? 3.5 : 2.8
            let dotRect = CGRect(x: center.x - dotR, y: center.y - dotR, width: dotR * 2, height: dotR * 2)
            strokeColor.withAlphaComponent(0.95).setFill()
            NSBezierPath(ovalIn: dotRect).fill()
        }

        if isScroll {
            let chevrons = NSBezierPath()
            for dir: CGFloat in [-1, 1] {
                let tipY = center.y + dir * (radius + 6)
                chevrons.move(to: CGPoint(x: center.x - 4, y: tipY - dir * 3.5))
                chevrons.line(to: CGPoint(x: center.x, y: tipY))
                chevrons.line(to: CGPoint(x: center.x + 4, y: tipY - dir * 3.5))
            }
            chevrons.lineWidth = 2.0
            strokeColor.withAlphaComponent(0.9).setStroke()
            chevrons.stroke()
        }
    }
}
