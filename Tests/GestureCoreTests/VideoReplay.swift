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
        var core = HCState()
        hc_init(&core, hc_default_range(), 0.5)
        let count = max(1, Int(min(72, ceil(duration * 3))))
        for sample in 0..<count {
            let second = duration * Double(sample) / Double(count)
            let (image, _) = try await generator.image(at: CMTime(seconds: second, preferredTimescale: 600))
            let aspect = Double(image.width) / Double(image.height)
            let out: HCOutput
            if let hand = try vision.observe(using: VNImageRequestHandler(cgImage: image, options: [:])) {
                out = hc_frame_weighted(&core, hand.points, hand.confidence, nil, aspect, Double(second))
            } else {
                out = hc_coast(&core, Double(second))
            }
            if out.mode != HC_SEARCH {
                found = true
                break
            }
        }
        XCTAssertTrue(found, "No usable hand detected in the supplied clip.")
    }
}
