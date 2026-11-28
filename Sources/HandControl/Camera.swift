import AVFoundation
import CoreImage
import GestureCore
import Vision

struct CameraDevice: Equatable {
    let id: String
    let name: String
}

struct HandObservation {
    let points: [HCPoint]
    let confidence: [Double]
}

struct CameraFrame {
    let image: CGImage
    let hand: HandObservation?
    let time: TimeInterval
    var points: [HCPoint]? { hand?.points }
}

final class HandVision {
    private let request = VNDetectHumanHandPoseRequest()
    private static let joints: [VNHumanHandPoseObservation.JointName] = [
        .wrist, .thumbCMC, .thumbMP, .thumbIP, .thumbTip,
        .indexMCP, .indexPIP, .indexDIP, .indexTip,
        .middleMCP, .middlePIP, .middleDIP, .middleTip,
        .ringMCP, .ringPIP, .ringDIP, .ringTip,
        .littleMCP, .littlePIP, .littleDIP, .littleTip
    ]

    init() { request.maximumHandCount = 1 }

    func observe(using handler: VNImageRequestHandler) throws -> HandObservation? {
        try handler.perform([request])
        guard let hand = request.results?.first else { return nil }
        let recognized = try hand.recognizedPoints(.all)
        var pts: [HCPoint] = []
        var conf: [Double] = []
        pts.reserveCapacity(21)
        conf.reserveCapacity(21)
        for joint in Self.joints {
            if let p = recognized[joint], p.confidence >= 0.05 {
                pts.append(HCPoint(x: 1 - p.location.x, y: 1 - p.location.y))
                conf.append(Double(p.confidence))
            } else {
                pts.append(HCPoint(x: -1, y: -1))
                conf.append(0)
            }
        }
        return HandObservation(points: pts, confidence: conf)
    }

    func points(using handler: VNImageRequestHandler) throws -> [HCPoint]? {
        guard let obs = try observe(using: handler) else { return nil }
        return zip(obs.points, obs.confidence).map { p, c in c >= 0.22 ? p : HCPoint(x: -1, y: -1) }
    }
}

final class CameraFeed: NSObject, AVCaptureVideoDataOutputSampleBufferDelegate {
    private let session = AVCaptureSession()
    private let queue = DispatchQueue(label: "local.handcontrol.camera", qos: .userInitiated)
    private let delivery = DispatchSemaphore(value: 1)
    private let vision = HandVision()
    private let context = CIContext(options: [.cacheIntermediates: false])
    private let previewColorSpace = CGColorSpace(name: CGColorSpace.sRGB)
    private let preferredDeviceID: String?
    private let onFrame: (CameraFrame) -> Void
    private let onFailure: (String) -> Void
    private var observers: [NSObjectProtocol] = []
    private var failed = false

    static func availableDevices() -> [CameraDevice] {
        let types: [AVCaptureDevice.DeviceType]
        if #available(macOS 14.0, *) {
            types = [.builtInWideAngleCamera, .continuityCamera, .external]
        } else {
            types = [.builtInWideAngleCamera, .externalUnknown]
        }
        let found = AVCaptureDevice.DiscoverySession(deviceTypes: types, mediaType: .video, position: .unspecified).devices
        let defaultID = AVCaptureDevice.default(for: .video)?.uniqueID
        let ordered = found.sorted { a, b in
            if a.uniqueID == defaultID { return true }
            if b.uniqueID == defaultID { return false }
            return a.localizedName.localizedCaseInsensitiveCompare(b.localizedName) == .orderedAscending
        }
        return ordered.map { CameraDevice(id: $0.uniqueID, name: $0.localizedName) }
    }

    init(deviceID: String? = nil, onFrame: @escaping (CameraFrame) -> Void, onFailure: @escaping (String) -> Void) {
        self.preferredDeviceID = deviceID
        self.onFrame = onFrame
        self.onFailure = onFailure
    }

    private static func selectDevice(preferredID: String?) -> AVCaptureDevice? {
        if let id = preferredID, !id.isEmpty, let match = AVCaptureDevice(uniqueID: id) {
            return match
        }
        return AVCaptureDevice.default(for: .video)
    }

    private static func configureBestFormat(for device: AVCaptureDevice, session: AVCaptureSession) {
        struct Candidate {
            let format: AVCaptureDevice.Format
            let fps: Double
            let score: Double
        }
        var best: Candidate?
        for format in device.formats {
            let desc = format.formatDescription
            guard CMFormatDescriptionGetMediaType(desc) == kCMMediaType_Video else { continue }
            let dims = CMVideoFormatDescriptionGetDimensions(desc)
            let w = Double(dims.width), h = Double(dims.height)
            guard w >= 640, h >= 480, w <= 1920, h <= 1440 else { continue }
            let maxFPS = format.videoSupportedFrameRateRanges.map(\.maxFrameRate).max() ?? 30
            guard maxFPS >= 24 else { continue }
            let targetFPS = min(60.0, maxFPS >= 59 ? 60.0 : (maxFPS >= 29.9 ? 30.0 : maxFPS))
            let resPenalty = abs(log(w / 1280.0)) + abs(log(h / 720.0))
            let aspectPenalty = abs((w / h) - (16.0 / 9.0)) * 0.35
            let fpsBonus = targetFPS >= 59 ? -0.22 : 0.0
            let score = resPenalty + aspectPenalty + fpsBonus
            if best == nil || score < best!.score {
                best = Candidate(format: format, fps: targetFPS, score: score)
            }
        }
        guard let chosen = best else {
            if session.canSetSessionPreset(.hd1280x720) {
                session.sessionPreset = .hd1280x720
            } else if session.canSetSessionPreset(.vga640x480) {
                session.sessionPreset = .vga640x480
            }
            return
        }
        do {
            try device.lockForConfiguration()
            defer { device.unlockForConfiguration() }
            device.activeFormat = chosen.format
            if let range = chosen.format.videoSupportedFrameRateRanges.first(where: {
                $0.minFrameRate <= chosen.fps + 0.5 && $0.maxFrameRate >= chosen.fps - 0.5
            }) {
                let clamped = min(range.maxFrameRate, max(range.minFrameRate, chosen.fps))
                let duration = CMTime(value: 1000, timescale: CMTimeScale( (clamped * 1000).rounded() ))
                device.activeVideoMinFrameDuration = duration
                device.activeVideoMaxFrameDuration = duration
            }
        } catch {
            if session.canSetSessionPreset(.hd1280x720) { session.sessionPreset = .hd1280x720 }
        }
    }

    func start() {
        queue.async {
            guard let device = Self.selectDevice(preferredID: self.preferredDeviceID) else {
                return self.fail("No camera found. Connect a camera and start again.")
            }
            do {
                let input = try AVCaptureDeviceInput(device: device)
                let output = AVCaptureVideoDataOutput()
                output.alwaysDiscardsLateVideoFrames = true
                output.videoSettings = [kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32BGRA]
                self.session.beginConfiguration()
                if self.session.canSetSessionPreset(.hd1280x720) {
                    self.session.sessionPreset = .hd1280x720
                } else if self.session.canSetSessionPreset(.vga640x480) {
                    self.session.sessionPreset = .vga640x480
                }
                guard self.session.canAddInput(input) else {
                    self.session.commitConfiguration()
                    return self.fail("Could not open this camera. Check whether another app is using it.")
                }
                self.session.addInput(input)
                guard self.session.canAddOutput(output) else {
                    self.session.commitConfiguration()
                    return self.fail("This camera cannot supply video frames.")
                }
                self.session.addOutput(output)
                if let connection = output.connection(with: .video), connection.isVideoMirroringSupported {
                    connection.automaticallyAdjustsVideoMirroring = false
                    connection.isVideoMirrored = false
                }
                output.setSampleBufferDelegate(self, queue: self.queue)
                self.session.commitConfiguration()
                Self.configureBestFormat(for: device, session: self.session)
                let center = NotificationCenter.default
                self.observers.append(center.addObserver(forName: .AVCaptureSessionRuntimeError, object: self.session, queue: nil) { [weak self] note in
                    let error = note.userInfo?[AVCaptureSessionErrorKey] as? Error
                    self?.reportInterruption(error?.localizedDescription ?? "The camera stopped unexpectedly.")
                })
                self.observers.append(center.addObserver(forName: .AVCaptureSessionWasInterrupted, object: self.session, queue: nil) { [weak self] _ in
                    self?.reportInterruption("Camera interrupted. Start again when it is available.")
                })
                self.session.startRunning()
                if !self.session.isRunning { self.fail("The camera did not start. Check Camera permission and try again.") }
            } catch {
                self.fail("Camera error: \(error.localizedDescription)")
            }
        }
    }

    private func reportInterruption(_ message: String) {
        queue.async { self.fail(message) }
    }

    private func fail(_ message: String) {
        guard !failed else { return }
        failed = true
        DispatchQueue.main.async { self.onFailure(message) }
    }

    func stop() {
        queue.async {
            self.failed = true
            for observer in self.observers { NotificationCenter.default.removeObserver(observer) }
            self.observers.removeAll()
            if self.session.isRunning { self.session.stopRunning() }
        }
    }

    func captureOutput(_ output: AVCaptureOutput, didOutput sample: CMSampleBuffer, from connection: AVCaptureConnection) {
        guard !failed, let buffer = CMSampleBufferGetImageBuffer(sample),
              delivery.wait(timeout: .now()) == .success else { return }
        let time = ProcessInfo.processInfo.systemUptime
        do {
            let hand = try vision.observe(using: VNImageRequestHandler(cvPixelBuffer: buffer, orientation: .up))
            let mirrored = CIImage(cvPixelBuffer: buffer).oriented(.upMirrored)
            // Deliver landmarks without waiting for a preview that may never be drawn.
            guard let image = context.createCGImage(mirrored, from: mirrored.extent, format: .RGBA8,
                                                    colorSpace: previewColorSpace, deferred: true) else {
                delivery.signal()
                return fail("Could not render the camera preview.")
            }
            DispatchQueue.main.async {
                defer { self.delivery.signal() }
                self.onFrame(CameraFrame(image: image, hand: hand, time: time))
            }
        } catch {
            delivery.signal()
            fail("Hand tracking failed: \(error.localizedDescription)")
        }
    }
}
