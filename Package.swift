// swift-tools-version: 5.10
import PackageDescription

let package = Package(
    name: "BTBHRenderer",
    platforms: [.macOS(.v11)],
    products: [
        .library(name: "BTBHRenderer", targets: ["BTBHRenderer"])
    ],
    targets: [
        .target(
            name: "BTBHRendererCore",
            path: "Sources/BTBHRendererCore",
            publicHeadersPath: "include"
        ),
        .target(
            name: "BTBHRenderer",
            dependencies: ["BTBHRendererCore"],
            path: "Sources/BTBHRenderer"
        ),
        .testTarget(
            name: "BTBHRendererTests",
            dependencies: ["BTBHRenderer"],
            path: "Tests/BTBHRendererTests"
        )
    ]
)
