// swift-tools-version:5.9
import PackageDescription

let package = Package(
    name: "FlashIO",
    platforms: [.iOS(.v14), .macOS(.v11), .tvOS(.v14), .watchOS(.v7)],
    products: [
        .library(name: "FlashIO", targets: ["FlashIO"]),
    ],
    targets: [
        .target(name: "CFlashIO", path: "Sources/CFlashIO"),
        .target(name: "FlashIO", dependencies: ["CFlashIO"], path: "Sources/FlashIO"),
        .testTarget(name: "FlashIOTests", dependencies: ["FlashIO"], path: "Tests/FlashIOTests"),
    ]
)
