import AppKit
import XCTest
@testable import HandControl

final class CommandBarTests: XCTestCase {
    @MainActor
    func testControlsRemainClickableWhenMaterialAndPermissionChange() async {
        _ = NSApplication.shared
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 710, height: 100),
                              styleMask: [.titled], backing: .buffered, defer: false)
        let host = NSView()
        window.contentView = host
        let buttons = ["Start Camera", "Calibrate Reach", "Allow Accessibility", "Show This App", "Camera Settings"]
            .map { NSButton(title: $0, target: nil, action: nil) }
        buttons[0].image = NSImage(systemSymbolName: "video", accessibilityDescription: nil)
        buttons[0].imagePosition = .imageLeading
        let bar = CommandBar(buttons: buttons)
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
            for button in buttons {
                let frame = host.convert(button.bounds, from: button)
                XCTAssertTrue(host.bounds.contains(frame))
                XCTAssertGreaterThanOrEqual(frame.width + 1, button.intrinsicContentSize.width, button.title)
                let hit = host.hitTest(NSPoint(x: frame.midX, y: frame.midY))
                XCTAssertTrue(hit === button || hit?.isDescendant(of: button) == true, button.title)
            }
        }
        let fullWidth = bar.frame.width
        buttons[2].isHidden = true
        buttons[3].isHidden = true
        host.layoutSubtreeIfNeeded()
        XCTAssertLessThan(bar.frame.width, fullWidth)
    }
}
