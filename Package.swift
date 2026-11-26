// swift-tools-version: 5.9
import PackageDescription

let package = Package(
    name: "HandControl",
    platforms: [.macOS(.v13)],
    products: [.executable(name: "HandControl", targets: ["HandControl"])],
    targets: [
        .target(name: "GestureCore", publicHeadersPath: "include"),
        .executableTarget(name: "HandControl", dependencies: ["GestureCore"]),
        .testTarget(name: "GestureCoreTests", dependencies: ["GestureCore", "HandControl"])
    ]
)
