import AppKit

final class CommandBar: NSView {
    private let row: NSStackView
    private var surface: NSView?
    private var solid: Bool?
    private static let radius: CGFloat = 16

    init(buttons: [NSButton]) {
        row = NSStackView(views: buttons)
        super.init(frame: .zero)
        row.spacing = 8
        row.alignment = .centerY
        row.translatesAutoresizingMaskIntoConstraints = false
        setAccessibilityElement(true)
        setAccessibilityRole(.toolbar)
        setAccessibilityLabel("Camera controls")
        updateAccessibilityMaterial()
        NSWorkspace.shared.notificationCenter.addObserver(self, selector: #selector(updateAccessibilityMaterial),
            name: NSWorkspace.accessibilityDisplayOptionsDidChangeNotification, object: nil)
    }

    required init?(coder: NSCoder) { nil }

    @objc private func updateAccessibilityMaterial() {
        let workspace = NSWorkspace.shared
        setSolid(workspace.accessibilityDisplayShouldReduceTransparency || workspace.accessibilityDisplayShouldIncreaseContrast)
    }

    func setSolid(_ enabled: Bool) {
        guard solid != enabled else { return }
        solid = enabled
        let focused = window?.firstResponder as? NSView
        let restoreFocus = focused?.isDescendant(of: row) == true
        row.removeFromSuperview()
        surface?.removeFromSuperview()

        let (background, content) = Self.makeSurface(solid: enabled)
        surface = background
        background.frame = bounds
        background.autoresizingMask = [.width, .height]
        addSubview(background)
        content.addSubview(row)
        NSLayoutConstraint.activate([
            row.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 12),
            row.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -12),
            row.topAnchor.constraint(equalTo: topAnchor, constant: 8),
            row.bottomAnchor.constraint(equalTo: bottomAnchor, constant: -8)
        ])
        if restoreFocus { window?.makeFirstResponder(focused) }
    }

    private static func makeSurface(solid: Bool) -> (NSView, NSView) {
        if solid {
            let box = NSBox()
            box.boxType = .custom
            box.titlePosition = .noTitle
            box.cornerRadius = radius
            box.borderWidth = 1
            box.borderColor = .labelColor
            box.fillColor = .controlBackgroundColor
            let content = NSView()
            box.contentView = content
            return (box, content)
        }
        #if compiler(>=6.2)
        if #available(macOS 26.0, *) {
            let glass = NSGlassEffectView()
            glass.style = .regular
            glass.cornerRadius = radius
            let content = NSView()
            glass.contentView = content
            return (glass, content)
        }
        #endif
        let material = NSVisualEffectView()
        material.material = .headerView
        material.blendingMode = .withinWindow
        material.wantsLayer = true
        material.layer?.cornerRadius = radius
        material.layer?.masksToBounds = true
        return (material, material)
    }
}
