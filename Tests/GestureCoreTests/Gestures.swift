import XCTest
import GestureCore
import ApplicationServices
@testable import HandControl

final class GestureTests: XCTestCase {
    var s = HCState()
    let open = [
        (0.50,0.79), (0.41,0.72), (0.36,0.67), (0.31,0.65), (0.26,0.64),
        (0.43,0.58), (0.43,0.48), (0.42,0.41), (0.42,0.34),
        (0.50,0.56), (0.50,0.51), (0.51,0.59), (0.51,0.64),
        (0.55,0.59), (0.55,0.53), (0.55,0.60), (0.55,0.66),
        (0.60,0.65), (0.60,0.59), (0.59,0.65), (0.59,0.69)
    ].map { HCPoint(x: $0.0, y: $0.1) }
    var pinch: [HCPoint] { var p = open; p[4] = p[8]; return p }
    override func setUp() { hc_init(&s, hc_default_range(), 0.5) }
    @discardableResult func frame(_ points: [HCPoint], _ time: Double) -> HCOutput {
        hc_frame(&s, points, 1, time)
    }

    func testRestingPointerRejectsTremorAndStillSettlesOnSmallTargets() {
        let screenWidth = 1600.0
        let range = HCRange(left: 0, top: 0, right: 1, bottom: 1)
        func shifted(_ dx: Double) -> [HCPoint] { open.map { HCPoint(x: $0.x + dx, y: $0.y) } }
        for fps in [24, 30, 60] {
            hc_init(&s, range, 0.5)
            let origin = frame(open, 0).position.x
            var energy = 0.0
            for i in 1...(3 * fps) {
                let t = Double(i) / Double(fps)
                let dx = 4 / screenWidth * sin(2 * .pi * 9 * t)
                let cursor = frame(shifted(dx), t)
                XCTAssertEqual(cursor.action, HC_POINTER)
                if i > fps { energy += pow((cursor.position.x - origin) * screenWidth, 2) }
            }
            let rms = sqrt(energy / Double(2 * fps))
            print(String(format: "%d Hz stationary trace: %.2f px RMS", fps, rms))
            XCTAssertLessThan(rms, 0.70, "Stationary jitter on a 1600 px screen at \(fps) Hz")

            var end = origin
            for i in 1...fps {
                end = frame(shifted(2 / screenWidth), 3 + Double(i) / Double(fps)).position.x
            }
            XCTAssertEqual((end - origin) * screenWidth, 2, accuracy: 0.1, "Fine adjustments must not hit a dead zone")
        }
    }

    func testQuickPointerAndDragFollowWithoutOvershoot() {
        for fps in [24, 30, 60] {
            for dragging in [false, true] {
                hc_init(&s, HCRange(left: 0, top: 0, right: 1, bottom: 1), 0.5)
                frame(open, 0)
                let pose = dragging ? pinch : open
                frame(pose, 0.04)
                let start = frame(pose, 0.38)
                XCTAssertEqual(start.action, dragging ? HC_DOWN : HC_POINTER)
                var previous = start.position.x
                var lag = 0.0
                let speed = 0.8  // Screen widths per second.
                for i in 1...(fps / 3) {
                    let t = Double(i) / Double(fps)
                    let dx = speed * t
                    let shifted = pose.map { HCPoint(x: $0.x + dx, y: $0.y) }
                    let moved = frame(shifted, 0.38 + t)
                    XCTAssertEqual(moved.action, dragging ? HC_DRAGGED : HC_POINTER)
                    XCTAssertGreaterThanOrEqual(moved.position.x, previous)
                    XCTAssertLessThanOrEqual(moved.position.x, shifted[8].x)
                    previous = moved.position.x
                    lag = (shifted[8].x - previous) / speed
                }
                if !dragging { print(String(format: "%d Hz sweep: %.1f ms filter lag", fps, lag * 1000)) }
                XCTAssertLessThan(lag, 0.018, "Fast \(dragging ? "drag" : "move") at \(fps) Hz")
            }
        }
    }

    func testPinchReleasesAtAnchorAndSecondTapIsDoubleClick() {
        let move = frame(open, 0)
        XCTAssertEqual(move.action, HC_POINTER)
        XCTAssertEqual(frame(pinch, 0.04).action, HC_NO_EVENT)
        let first = frame(open, 0.12)
        XCTAssertEqual(first.action, HC_CLICK)
        XCTAssertEqual(first.position.x, move.position.x)
        XCTAssertEqual(first.clickCount, 1)
        frame(pinch, 0.2)
        let second = frame(open, 0.28)
        XCTAssertEqual(second.action, HC_CLICK)
        XCTAssertEqual(second.clickCount, 2)
    }

    func testHoldDragDropsOnceWithoutClick() {
        frame(open, 1)
        frame(pinch, 1.03)
        XCTAssertEqual(frame(pinch, 1.36).action, HC_DOWN)
        var moved = pinch
        for i in moved.indices { moved[i].x += 0.1 }
        let drag = frame(moved, 1.40)
        XCTAssertEqual(drag.action, HC_DRAGGED)
        XCTAssertGreaterThan(drag.position.x, s.anchor.x)
        XCTAssertEqual(frame(open, 1.45).action, HC_UP)
        XCTAssertEqual(frame(open, 1.48).action, HC_POINTER)
        XCTAssertEqual(hc_stop(&s).action, HC_NO_EVENT)
    }

    func testMissingOrStaleFramesReleaseAndReacquisitionCannotClick() {
        frame(open, 1); frame(pinch, 1.04); frame(pinch, 1.38)
        XCTAssertEqual(frame(pinch, 2.0).action, HC_UP)
        XCTAssertEqual(frame(pinch, 2.04).action, HC_NO_EVENT)
        XCTAssertEqual(frame(open, 2.10).action, HC_POINTER)
        frame(pinch, 2.14)
        var invalid = open; invalid[8].x = -1
        XCTAssertEqual(frame(invalid, 2.20).mode, HC_SEARCH)
        XCTAssertEqual(frame(open, 2.24).action, HC_POINTER)
        frame(pinch, 2.28); frame(pinch, 2.62)
        XCTAssertEqual(hc_lost(&s).action, HC_UP)
        XCTAssertEqual(hc_lost(&s).action, HC_NO_EVENT)
    }

    func testRightClickScrollAndFistHaveDistinctEffects() {
        frame(open, 0)
        var right = open; right[4] = right[12]
        XCTAssertEqual(frame(right, 0.04).mode, HC_RIGHT_PINCH)
        XCTAssertEqual(frame(right, 0.18).action, HC_NO_EVENT)
        XCTAssertEqual(frame(open, 0.22).action, HC_RIGHT_CLICK)
        var two = open; two[12].y = 0.34
        XCTAssertEqual(frame(two, 0.26).mode, HC_SCROLL)
        two[8].y -= 0.04; two[12].y -= 0.04
        XCTAssertGreaterThan(frame(two, 0.30).wheel, 0)
        two[8].y += 0.06; two[12].y += 0.06
        XCTAssertLessThan(frame(two, 0.34).wheel, 0)
        var fist = open; fist[8].y = 0.67
        XCTAssertEqual(frame(fist, 0.40).mode, HC_PAUSE)
        XCTAssertEqual(frame(pinch, 0.45).action, HC_NO_EVENT)
        XCTAssertEqual(frame(open, 0.50).action, HC_POINTER)
    }

    func testCalibrationRequiresTwoStableOrderedCornersAndSuppressesInput() {
        frame(open, 0); frame(pinch, 0.03); frame(pinch, 0.37)
        XCTAssertEqual(hc_begin_calibration(&s).action, HC_UP)
        func pose(_ x: Double, _ y: Double) -> [HCPoint] {
            open.map { HCPoint(x: x + ($0.x - open[8].x) * 0.55, y: y + ($0.y - open[8].y) * 0.55) }
        }
        for i in 0...24 { XCTAssertEqual(frame(pose(0.24, 0.23), 1 + Double(i)*0.04).action, HC_NO_EVENT) }
        XCTAssertEqual(s.calStage, 2)
        for i in 25...49 { frame(pose(0.28, 0.25), 1 + Double(i)*0.04) }
        XCTAssertEqual(s.calStage, 2)
        for i in 50...70 { XCTAssertEqual(frame(pose(0.76, 0.68), 1 + Double(i)*0.04).action, HC_NO_EVENT) }
        XCTAssertEqual(s.calStage, 0)
        XCTAssertEqual(s.range.left, 0.24, accuracy: 0.001)
        XCTAssertEqual(s.range.bottom, 0.68, accuracy: 0.001)
        let edge = frame(pose(0.81, 0.71), 3.84)
        XCTAssertEqual(edge.position.x, 1, accuracy: 0.001)
        XCTAssertEqual(edge.position.y, 1, accuracy: 0.001)
    }

    func testMacAdapterProducesQuartzEventsWithoutPostingToDesktop() {
        var events: [CGEvent] = []
        let mouse = DesktopMouse { events.append($0) }
        for (pose, t) in [(open,0.0), (pinch,0.04), (open,0.1), (pinch,0.16), (open,0.22),
                          (pinch,0.26), (pinch,0.60), (pinch,0.64)] { mouse.apply(frame(pose,t)) }
        mouse.release()
        mouse.release()
        XCTAssertEqual(events.map(\.type), [.mouseMoved, .leftMouseDown, .leftMouseUp,
                                             .leftMouseDown, .leftMouseUp, .leftMouseDown, .leftMouseDragged, .leftMouseUp])
        XCTAssertEqual(events[3].getIntegerValueField(.mouseEventClickState), 2)
        XCTAssertEqual(events[4].getIntegerValueField(.mouseEventClickState), 2)
        var right = open; right[4] = right[12]
        var two = open; two[12].y = 0.34
        _ = hc_stop(&s)
        mouse.apply(frame(open,1)); mouse.apply(frame(right,1.04)); mouse.apply(frame(open,1.14))
        mouse.apply(frame(two,1.18)); two[8].y -= 0.03; two[12].y -= 0.03
        mouse.apply(frame(two,1.22))
        XCTAssertEqual(events.suffix(3).map(\.type), [.rightMouseDown, .rightMouseUp, .scrollWheel])
    }
}
