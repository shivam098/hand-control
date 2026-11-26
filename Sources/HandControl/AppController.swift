import AppKit
import AVFoundation
import GestureCore

final class AppController: NSObject, NSApplicationDelegate {
    private let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 800, height: 660),
                                  styleMask: [.titled, .closable, .miniaturizable, .resizable],
                                  backing: .buffered, defer: false)
    private let preview = Preview()
    private let status = NSTextField(labelWithString: "Ready. Start the camera to see your hand.")
    private let permission = NSTextField(labelWithString: "")
    private let start = NSButton(title: "Start Camera", target: nil, action: nil)
    private let calibrate = NSButton(title: "Calibrate Reach", target: nil, action: nil)
    private let allow = NSButton(title: "Allow Accessibility", target: nil, action: nil)
    private let reveal = NSButton(title: "Show This App", target: nil, action: nil)
    private let cameraSettings = NSButton(title: "Camera Settings", target: nil, action: nil)
    private let mouse = DesktopMouse()
    private var core = HCState()
    private var feed: CameraFeed?
    private var trackingActivity: NSObjectProtocol?
    private var timer: Timer?
    private var generation = 0
    private var active = false
    private var trusted = false
    private var calibrated = false
    private var lastFrameTime = 0.0
    private var lastPermissionCheck = 0.0
    private let settingsKey = "handcontrol.reach.v1"

    func applicationDidFinishLaunching(_ notification: Notification) {
        var range = hc_default_range()
        if let values = UserDefaults.standard.array(forKey: settingsKey) as? [Double], values.count == 4 {
            let saved = HCRange(left: values[0], top: values[1], right: values[2], bottom: values[3])
            if hc_valid_range(saved) != 0 { range = saved; calibrated = true }
        }
        hc_init(&core, range, NSEvent.doubleClickInterval)
        preview.reach = range
        buildWindow()
        updatePermission()
        window.center()
        window.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
        let watchdog = Timer(timeInterval: 0.2, repeats: true) { [weak self] _ in self?.tick() }
        RunLoop.main.add(watchdog, forMode: .common)
        timer = watchdog
    }

    private func buildWindow() {
        window.title = "Hand Control"
        window.minSize = NSSize(width: 710, height: 540)
        window.isReleasedWhenClosed = false
        let menu = NSMenu()
        let appMenu = NSMenu()
        let appItem = NSMenuItem()
        appItem.submenu = appMenu
        menu.addItem(appItem)
        appMenu.addItem(withTitle: "Quit Hand Control", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
        let windowMenu = NSMenu(title: "Window")
        let windowItem = NSMenuItem()
        windowItem.submenu = windowMenu
        menu.addItem(windowItem)
        windowMenu.addItem(withTitle: "Minimize", action: #selector(NSWindow.performMiniaturize(_:)), keyEquivalent: "m")
        NSApp.mainMenu = menu
        NSApp.windowsMenu = windowMenu

        let title = NSTextField(labelWithString: "Hand Control")
        title.font = .systemFont(ofSize: 28, weight: .semibold)
        let subtitle = NSTextField(labelWithString: "Control your desktop with one hand. Video stays on this Mac.")
        subtitle.textColor = .secondaryLabelColor
        let guide = NSTextField(wrappingLabelWithString: "Point with your index finger to move. Pinch thumb + index to click; pinch twice to double-click, or hold to drag and release to drop. Pinch thumb + middle to right-click. Raise only index + middle and move vertically to scroll. Make a fist to pause; open your hand to resume.")
        guide.font = .systemFont(ofSize: 13)
        guide.maximumNumberOfLines = 0
        status.font = .systemFont(ofSize: 14, weight: .semibold)
        status.lineBreakMode = .byWordWrapping
        status.maximumNumberOfLines = 2
        permission.font = .systemFont(ofSize: 12)
        permission.textColor = .secondaryLabelColor
        permission.lineBreakMode = .byWordWrapping
        permission.maximumNumberOfLines = 0
        for (button, action) in [(start, #selector(toggleCamera)), (calibrate, #selector(toggleCalibration)),
                                 (allow, #selector(openAccessibility)), (reveal, #selector(showThisApp)),
                                 (cameraSettings, #selector(openCameraSettings))] {
            button.target = self
            button.action = action
            button.bezelStyle = .rounded
        }
        calibrate.isEnabled = false
        cameraSettings.isHidden = true
        let buttons = NSStackView(views: [start, calibrate, allow, reveal, cameraSettings])
        buttons.spacing = 8
        let root = NSView()
        window.contentView = root
        let stack = NSStackView(views: [title, subtitle, preview, status, guide, buttons, permission])
        stack.orientation = .vertical
        stack.alignment = .leading
        stack.spacing = 10
        stack.setCustomSpacing(2, after: title)
        stack.setCustomSpacing(16, after: subtitle)
        root.addSubview(stack)
        stack.translatesAutoresizingMaskIntoConstraints = false
        preview.setContentHuggingPriority(.init(1), for: .vertical)
        NSLayoutConstraint.activate([
            stack.leadingAnchor.constraint(equalTo: root.leadingAnchor, constant: 20),
            stack.trailingAnchor.constraint(equalTo: root.trailingAnchor, constant: -20),
            stack.topAnchor.constraint(equalTo: root.topAnchor, constant: 16),
            stack.bottomAnchor.constraint(equalTo: root.bottomAnchor, constant: -16),
            preview.widthAnchor.constraint(equalTo: stack.widthAnchor),
            preview.heightAnchor.constraint(greaterThanOrEqualToConstant: 160),
            guide.widthAnchor.constraint(equalTo: stack.widthAnchor),
            status.widthAnchor.constraint(equalTo: stack.widthAnchor),
            permission.widthAnchor.constraint(equalTo: stack.widthAnchor)
        ])
    }

    @objc private func toggleCamera() {
        if active { stop("Camera off. Desktop control is stopped."); return }
        generation += 1
        let id = generation
        active = true
        start.title = "Stop Camera"
        cameraSettings.isHidden = true
        status.stringValue = "Starting camera..."
        switch AVCaptureDevice.authorizationStatus(for: .video) {
        case .authorized: startFeed(id)
        case .notDetermined:
            status.stringValue = "Waiting for Camera permission..."
            AVCaptureDevice.requestAccess(for: .video) { granted in
                DispatchQueue.main.async {
                    guard self.generation == id else { return }
                    if granted { self.startFeed(id) } else { self.cameraDenied() }
                }
            }
        default: cameraDenied()
        }
    }

    private func cameraDenied() {
        stop("Camera access is off. Enable Hand Control in Privacy & Security > Camera, then reopen the app.")
        cameraSettings.isHidden = false
    }

    private func startFeed(_ id: Int) {
        lastFrameTime = ProcessInfo.processInfo.systemUptime
        trackingActivity = ProcessInfo.processInfo.beginActivity(options: .userInitiatedAllowingIdleSystemSleep,
                                                               reason: "Track gestures while the user is in other apps")
        let camera = CameraFeed(onFrame: { [weak self] frame in
            guard let self, self.generation == id else { return }
            self.consume(frame)
        }, onFailure: { [weak self] message in
            guard let self, self.generation == id else { return }
            self.stop(message)
        })
        feed = camera
        camera.start()
    }

    private func consume(_ frame: CameraFrame) {
        guard ProcessInfo.processInfo.systemUptime - frame.time < 0.4 else {
            loseTracking()
            return
        }
        lastFrameTime = frame.time
        calibrate.isEnabled = true
        let out: HCOutput
        if let points = frame.points {
            out = hc_frame(&core, points, Double(frame.image.width) / Double(frame.image.height), frame.time)
        } else {
            out = hc_lost(&core)
        }
        preview.frameImage = frame
        display(out)
    }

    private func display(_ out: HCOutput) {
        if trusted { mouse.apply(out) }
        preview.firstCorner = core.calStage == 2 ? core.firstCorner : nil
        preview.reach = core.range
        preview.needsDisplay = true
        var text: String
        switch out.mode {
        case HC_MOVE: text = "Move"
        case HC_PINCH: text = "Pinch: release to click or hold to drag"
        case HC_RIGHT_PINCH: text = "Middle pinch: release to right-click"
        case HC_DRAG: text = "Dragging: move your hand, then open the pinch to drop"
        case HC_SCROLL: text = "Scroll"
        case HC_PAUSE: text = "Paused: open your hand and separate your fingertips"
        case HC_CAL_FIRST: text = "1 of 2: point at your comfortable UPPER LEFT and hold still"
        case HC_CAL_SECOND: text = "2 of 2: point farther DOWN and RIGHT and hold still"
        case HC_CAL_DONE:
            let r = core.range
            UserDefaults.standard.set([r.left, r.top, r.right, r.bottom], forKey: settingsKey)
            calibrated = true
            calibrate.title = "Calibrate Reach"
            text = "Calibration saved"
        default: text = "Looking for a hand: show your palm and fingertips"
        }
        if core.calStage != 0 {
            text += "   \(Int(out.progress * 100))%"
        } else {
            if out.action == HC_CLICK { text = out.clickCount == 2 ? "Double-click" : "Click" }
            if out.action == HC_RIGHT_CLICK { text = "Right-click" }
            text += calibrated ? "   |   Calibrated" : "   |   Default reach"
        }
        if status.stringValue != text { status.stringValue = text }
    }

    private func loseTracking() {
        let out = hc_lost(&core)
        if trusted { mouse.apply(out) }
        preview.frameImage = nil
        preview.needsDisplay = true
        status.stringValue = "Waiting for fresh camera frames. Desktop control is paused."
    }

    private func tick() {
        let now = ProcessInfo.processInfo.systemUptime
        if now - lastPermissionCheck > 1 {
            updatePermission()
            lastPermissionCheck = now
        }
        if feed != nil && now - lastFrameTime > 0.45 { loseTracking() }
    }

    private func updatePermission() {
        let allowed = CGPreflightPostEventAccess()
        if trusted != allowed {
            mouse.release()
            _ = hc_lost(&core)
        }
        trusted = allowed
        allow.isHidden = allowed
        reveal.isHidden = allowed
        permission.stringValue = allowed
            ? "Desktop control enabled. Minimize to keep tracking; close the window to stop."
            : "Preview only: macOS has not allowed this copy to control the pointer. If Accessibility already shows Hand Control enabled, remove that entry and re-add the copy shown by Show This App."
    }

    func applicationDidBecomeActive(_ notification: Notification) { updatePermission() }

    @objc private func showThisApp() {
        NSWorkspace.shared.activateFileViewerSelecting([Bundle.main.bundleURL])
    }

    @objc private func openAccessibility() {
        _ = CGRequestPostEventAccess()
        updatePermission()
        guard !trusted else { return }
        guard let url = URL(string: "x-apple.systempreferences:com.apple.preference.security?Privacy_Accessibility"),
              NSWorkspace.shared.open(url) else {
            status.stringValue = "Open System Settings > Privacy & Security > Accessibility and add this copy of Hand Control."
            return
        }
    }

    @objc private func openCameraSettings() {
        if let url = URL(string: "x-apple.systempreferences:com.apple.preference.security?Privacy_Camera") {
            NSWorkspace.shared.open(url)
        }
    }

    @objc private func toggleCalibration() {
        if core.calStage != 0 {
            _ = hc_stop(&core)
            calibrate.title = "Calibrate Reach"
            status.stringValue = "Calibration cancelled. Previous reach retained."
        } else {
            display(hc_begin_calibration(&core))
            calibrate.title = "Cancel Calibration"
        }
    }

    private func stop(_ message: String) {
        generation += 1
        active = false
        _ = hc_stop(&core)
        mouse.release()
        feed?.stop()
        feed = nil
        if let activity = trackingActivity {
            ProcessInfo.processInfo.endActivity(activity)
            trackingActivity = nil
        }
        preview.frameImage = nil
        preview.firstCorner = nil
        preview.needsDisplay = true
        start.title = "Start Camera"
        calibrate.title = "Calibrate Reach"
        calibrate.isEnabled = false
        status.stringValue = message
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }
    func applicationWillTerminate(_ notification: Notification) {
        timer?.invalidate()
        stop("Stopped")
    }
}
