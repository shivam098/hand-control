import AppKit
import GestureCore
import os

final class DesktopMouse {
    private let source = CGEventSource(stateID: .hidSystemState)
    private var heldAt: CGPoint?
    private let send: (CGEvent) -> Void
    private let log = Logger(subsystem: "local.handcontrol.desktop", category: "input")

    init(send: @escaping (CGEvent) -> Void = { $0.post(tap: .cghidEventTap) }) {
        self.send = send
        source?.localEventsSuppressionInterval = 0
    }

    private func point(_ p: HCPoint) -> CGPoint {
        let r = CGDisplayBounds(CGMainDisplayID())
        return CGPoint(x: r.minX + p.x * (r.width - 1), y: r.minY + p.y * (r.height - 1))
    }

    @discardableResult
    private func post(_ kind: CGEventType, at p: CGPoint, button: CGMouseButton = .left, count: Int64 = 0) -> Bool {
        guard let event = CGEvent(mouseEventSource: source, mouseType: kind, mouseCursorPosition: p, mouseButton: button) else {
            log.error("Could not allocate a pointer event")
            return false
        }
        event.setIntegerValueField(.mouseEventClickState, value: count)
        send(event)
        return true
    }

    func release() {
        if let p = heldAt {
            post(.leftMouseUp, at: p, count: 1)
            heldAt = nil
        }
    }

    func apply(_ out: HCOutput) {
        let p = point(out.position)
        switch out.action {
        case HC_POINTER: post(.mouseMoved, at: p)
        case HC_CLICK, HC_RIGHT_CLICK:
            let right = out.action == HC_RIGHT_CLICK
            if !right && out.synthesized != 0 && out.clickCount == 2 {
                post(.leftMouseDown, at: p, button: .left, count: 1)
                post(.leftMouseUp, at: p, button: .left, count: 1)
            }
            post(right ? .rightMouseDown : .leftMouseDown, at: p, button: right ? .right : .left, count: Int64(out.clickCount))
            post(right ? .rightMouseUp : .leftMouseUp, at: p, button: right ? .right : .left, count: Int64(out.clickCount))
        case HC_DOWN:
            if post(.leftMouseDown, at: p, count: 1) { heldAt = p }
        case HC_DRAGGED:
            guard heldAt != nil else { return }
            post(.leftMouseDragged, at: p, count: 1)
            heldAt = p
        case HC_UP: release()
        case HC_WHEEL:
            guard let wheel = CGEvent(scrollWheelEvent2Source: source, units: .pixel, wheelCount: 1, wheel1: out.wheel, wheel2: 0, wheel3: 0) else {
                log.error("Could not allocate a scroll event")
                return
            }
            send(wheel)
        default: break
        }
    }
}
