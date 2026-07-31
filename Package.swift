// swift-tools-version: 5.10
import PackageDescription
import Foundation

var rendererTargets: [Target] = [
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
    .target(
        name: "BTBHRendererGPUCore",
        path: "Sources/BTBHRendererGPUCore",
        publicHeadersPath: "include",
        cSettings: [
            .headerSearchPath("../../include"),
            .headerSearchPath("../../src/gpu"),
            .headerSearchPath("../../external/SDL/include"),
            .headerSearchPath("../../external/raylib/src/external"),
            .headerSearchPath("assimp-config"),
            .headerSearchPath("../../external/assimp/include"),
            .define("ASSIMP_DLL", .when(platforms: [.windows]))
        ],
        linkerSettings: [
            .linkedLibrary("SDL3-static", .when(platforms: [.windows])),
            .linkedLibrary("SDL3", .when(platforms: [.macOS])),
            .linkedLibrary(
                "assimp-vc143-mt",
                .when(platforms: [.windows])
            ),
            .linkedLibrary("winmm", .when(platforms: [.windows])),
            .linkedLibrary("kernel32", .when(platforms: [.windows])),
            .linkedLibrary("gdi32", .when(platforms: [.windows])),
            .linkedLibrary("user32", .when(platforms: [.windows])),
            .linkedLibrary("shell32", .when(platforms: [.windows])),
            .linkedLibrary("ole32", .when(platforms: [.windows])),
            .linkedLibrary("oleaut32", .when(platforms: [.windows])),
            .linkedLibrary("imm32", .when(platforms: [.windows])),
            .linkedLibrary("version", .when(platforms: [.windows])),
            .linkedLibrary("setupapi", .when(platforms: [.windows])),
            .linkedLibrary("uuid", .when(platforms: [.windows]))
        ]
    ),
    .target(
        name: "BTBHRendererGPU",
        dependencies: ["BTBHRenderer", "BTBHRendererGPUCore"],
        path: "Sources/BTBHRendererGPU"
    ),
    .testTarget(
        name: "BTBHRendererTests",
        dependencies: ["BTBHRenderer"],
        path: "Tests/BTBHRendererTests"
    )
]

if ProcessInfo.processInfo.environment["BTBH_ENABLE_GPU_TESTS"] == "1" {
    rendererTargets.append(
        .testTarget(
            name: "BTBHRendererGPUTests",
            dependencies: [
                "BTBHRendererGPU",
                "BTBHRendererGPUCore",
            ],
            path: "Tests/BTBHRendererGPUTests"
        )
    )
}

let package = Package(
    name: "BTBHRenderer",
    platforms: [.macOS(.v11)],
    products: [
        .library(name: "BTBHRenderer", targets: ["BTBHRenderer"]),
        .library(name: "BTBHRendererGPU", targets: ["BTBHRendererGPU"])
    ],
    targets: rendererTargets
)
