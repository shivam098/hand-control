import AVFoundation
import GestureCore
import Vision
import XCTest
@testable import HandControl

final class VideoReplayTests: XCTestCase {
    func testOfflineCameraPipeline() async throws {
        guard let path = ProcessInfo.processInfo.environment["HAND_CONTROL_TEST_VIDEO"] else {
            throw XCTSkip("Set HAND_CONTROL_TEST_VIDEO to exercise local video replay.")
        }
        let asset = AVURLAsset(url: URL(fileURLWithPath: path))
        let duration = try await asset.load(.duration).seconds
        guard duration.isFinite, duration > 0 else { return XCTFail("No video duration") }
        let generator = AVAssetImageGenerator(asset: asset)
        generator.appliesPreferredTrackTransform = true
        generator.requestedTimeToleranceBefore = .zero
        generator.requestedTimeToleranceAfter = CMTime(value: 1, timescale: 15)
        let vision = HandVision()
        var found = false
        let count = max(1, Int(min(36, ceil(duration))))
        for sample in 0..<count {
            let second = duration * Double(sample) / Double(count)
            let (image, _) = try await generator.image(at: CMTime(seconds: second, preferredTimescale: 600))
            if let points = try vision.points(using: VNImageRequestHandler(cgImage: image, options: [:])) {
                var core = HCState()
                hc_init(&core, hc_default_range(), 0.5)
                if hc_frame(&core, points, Double(image.width) / Double(image.height), Double(second)).mode != HC_SEARCH {
                    found = true
                    break
                }
            }
        }
        XCTAssertTrue(found, "No usable hand detected in the supplied clip.")
    }
}
