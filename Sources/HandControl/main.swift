import AppKit

let app = NSApplication.shared
let controller = AppController()
app.setActivationPolicy(.regular)
app.delegate = controller
app.run()
