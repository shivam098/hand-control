import AppKit
import GestureCore
import XCTest
@testable import HandControl

final class CommandBarTests: XCTestCase {
    @MainActor
    func testControlsRemainClickableWhenMaterialAndPermissionChange() async {
        _ = NSApplication.shared
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 920, height: 100),
                              styleMask: [.titled], backing: .buffered, defer: false)
        let host = NSView()
        window.contentView = host
        let cameraPicker = NSPopUpButton(frame: .zero, pullsDown: false)
        cameraPicker.addItems(withTitles: ["FaceTime HD Camera", "iPhone Camera"])
        let buttons = ["Start Camera", "Calibrate Reach", "Halo: On", "Allow Accessibility", "Show This App", "Camera Settings"]
            .map { NSButton(title: $0, target: nil, action: nil) }
        buttons[0].image = NSImage(systemSymbolName: "video", accessibilityDescription: nil)
        buttons[0].imagePosition = .imageLeading
        let controls: [NSButton] = [buttons[0], cameraPicker] + Array(buttons.dropFirst())
        let bar = CommandBar(buttons: controls)
        host.addSubview(bar)
        bar.translatesAutoresizingMaskIntoConstraints = false
        NSLayoutConstraint.activate([
            bar.leadingAnchor.constraint(equalTo: host.leadingAnchor, constant: 20),
            bar.trailingAnchor.constraint(lessThanOrEqualTo: host.trailingAnchor, constant: -20),
            bar.centerYAnchor.constraint(equalTo: host.centerYAnchor)
        ])
        for solid in [false, true, false] {
            bar.setSolid(solid)
            host.layoutSubtreeIfNeeded()
            for control in controls {
                let frame = host.convert(control.bounds, from: control)
                XCTAssertTrue(host.bounds.contains(frame))
                let hit = host.hitTest(NSPoint(x: frame.midX, y: frame.midY))
                XCTAssertTrue(hit === control || hit?.isDescendant(of: control) == true)
            }
        }
        let fullWidth = bar.frame.width
        buttons[3].isHidden = true
        buttons[4].isHidden = true
        host.layoutSubtreeIfNeeded()
        XCTAssertLessThan(bar.frame.width, fullWidth)

        let halo = CursorHalo()
        var out = HCOutput()
        out.mode = HC_MOVE
        out.position = HCPoint(x: 0.5, y: 0.5)
        out.quality = 0.9
        out.pinchLevel = 0.4
        out.locked = 1
        halo.update(out, enabled: true)
        halo.isEnabled = false
        halo.update(out, enabled: true)
        halo.hide()
    }
}
