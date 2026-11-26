import AVFoundation
import CoreImage
import GestureCore
import Vision

struct CameraFrame {
    let image: CGImage
    let points: [HCPoint]?
    let time: TimeInterval
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

    func points(using handler: VNImageRequestHandler) throws -> [HCPoint]? {
        try handler.perform([request])
        guard let hand = request.results?.first else { return nil }
        let recognized = try hand.recognizedPoints(.all)
        return Self.joints.map { joint in
            guard let p = recognized[joint], p.confidence >= 0.22 else { return HCPoint(x: -1, y: -1) }
            return HCPoint(x: 1 - p.location.x, y: 1 - p.location.y)
        }
    }
}

final class CameraFeed: NSObject, AVCaptureVideoDataOutputSampleBufferDelegate {
    private let session = AVCaptureSession()
    private let queue = DispatchQueue(label: "local.handcontrol.camera", qos: .userInitiated)
    private let delivery = DispatchSemaphore(value: 1)
    private let vision = HandVision()
    private let context = CIContext(options: [.cacheIntermediates: false])
    private let previewColorSpace = CGColorSpace(name: CGColorSpace.sRGB)
    private let onFrame: (CameraFrame) -> Void
    private let onFailure: (String) -> Void
    private var observers: [NSObjectProtocol] = []
    private var failed = false

    init(onFrame: @escaping (CameraFrame) -> Void, onFailure: @escaping (String) -> Void) {
        self.onFrame = onFrame
        self.onFailure = onFailure
    }

    func start() {
        queue.async {
            guard let device = AVCaptureDevice.default(for: .video) else {
                return self.fail("No camera found. Connect a camera and start again.")
            }
            do {
                let input = try AVCaptureDeviceInput(device: device)
                let output = AVCaptureVideoDataOutput()
                output.alwaysDiscardsLateVideoFrames = true
                output.videoSettings = [kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32BGRA]
                self.session.beginConfiguration()
                if self.session.canSetSessionPreset(.vga640x480) { self.session.sessionPreset = .vga640x480 }
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
            let points = try vision.points(using: VNImageRequestHandler(cvPixelBuffer: buffer, orientation: .up))
            let mirrored = CIImage(cvPixelBuffer: buffer).oriented(.upMirrored)
            // Deliver landmarks without waiting for a preview that may never be drawn.
            guard let image = context.createCGImage(mirrored, from: mirrored.extent, format: .RGBA8,
                                                    colorSpace: previewColorSpace, deferred: true) else {
                delivery.signal()
                return fail("Could not render the camera preview.")
            }
            DispatchQueue.main.async {
                defer { self.delivery.signal() }
                self.onFrame(CameraFrame(image: image, points: points, time: time))
            }
        } catch {
            delivery.signal()
            fail("Hand tracking failed: \(error.localizedDescription)")
        }
    }
}
