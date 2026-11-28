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
    @discardableResult func hold(_ pose: [HCPoint], from start: Double, duration: Double = 0.34) -> HCOutput {
        var result = frame(pose, start)
        let steps = Int(ceil(duration * 30))
        for i in 1...steps { result = frame(pose, start + duration * Double(i) / Double(steps)) }
        return result
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
                let start = hold(pose, from: 0.04)
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

    func testRestingAimIgnoresTremorAndAnIsolatedFingertipJump() {
        hc_init(&s, HCRange(left: 0, top: 0, right: 1, bottom: 1), 0.5)
        let origin = frame(open, 0).position
        var worst = 0.0
        for i in 1...120 {
            let t = Double(i) / 30
            let dx = 0.0025 * sin(2 * .pi * 9 * t)
            var pose = open.map { HCPoint(x: $0.x + dx, y: $0.y) }
            if i.isMultiple(of: 19) { pose[8].x += 0.045 }
            let out = frame(pose, t)
            XCTAssertEqual(out.action, HC_POINTER)
            worst = max(worst, abs(out.position.x - origin.x) * 1600)
        }
        XCTAssertLessThan(worst, 1, "Peak resting excursion on the noisy trace, in pixels")
        var aimed = open
        aimed[8].x += 0.04
        for i in 1...6 { frame(aimed, 4 + Double(i)/30) }
        XCTAssertGreaterThan(s.cursor.x, origin.x + 0.03, "A sustained finger-only adjustment must still move")
    }

    func testPinchApproachKeepsTargetAndSingleNoisySamplesCannotClick() {
        let aim = frame(open, 0).position
        for (i,tip) in [8,12].enumerated() {
            var glitch = open; glitch[4] = glitch[tip]
            let t = 0.1 + Double(i) * 0.15
            frame(glitch, t)
            let actions = [frame(open, t + 0.05).action, frame(open, t + 0.09).action]
            XCTAssertFalse(actions.contains(HC_CLICK) || actions.contains(HC_RIGHT_CLICK))
        }
        var approach = open
        approach[8].x += 0.035
        approach[4] = HCPoint(x: approach[8].x - 0.125, y: approach[8].y)
        let closing = frame(approach, 0.39)
        XCTAssertEqual(closing.position.x, aim.x, accuracy: 1e-8)
        var closed = approach; closed[4] = closed[8]
        frame(closed, 0.42); frame(closed, 0.46)
        XCTAssertEqual(frame(open, 0.50).action, HC_NO_EVENT)
        frame(closed, 0.53)
        XCTAssertEqual(frame(open, 0.56).action, HC_NO_EVENT)
        let click = frame(open, 0.60)
        XCTAssertEqual(click.action, HC_CLICK)
        XCTAssertEqual(click.position.x, aim.x, accuracy: 1e-8)
        XCTAssertEqual(click.clickCount, 1)
    }

    func testDragUsesPalmTravelAndDoesNotDropOnOneBadObservation() {
        let aim = frame(open, 0).position
        var closed = pinch
        closed[8].y += 0.065; closed[4] = closed[8]
        XCTAssertEqual(hold(closed, from: 0.04, duration: 0.33).action, HC_DOWN)
        closed[8].x += 0.04; closed[4] = closed[8]
        let curl = frame(closed, 0.41)
        XCTAssertEqual(curl.action, HC_DRAGGED)
        XCTAssertEqual(curl.position.x, aim.x, accuracy: 1e-8)
        XCTAssertEqual(curl.position.y, aim.y, accuracy: 1e-8)
        var moved = closed.map { HCPoint(x: $0.x + 0.10, y: $0.y) }
        moved[20] = HCPoint(x: -1, y: -1) // Unrelated fingertip is occluded.
        let travel = frame(moved, 0.45)
        XCTAssertEqual(travel.action, HC_DRAGGED)
        XCTAssertGreaterThan(travel.position.x, aim.x + 0.05)
        var split = moved; split[4].x -= 0.22
        XCTAssertEqual(frame(split, 0.49).action, HC_NO_EVENT)
        XCTAssertEqual(frame(moved, 0.52).action, HC_DRAGGED)
        XCTAssertEqual(frame(split, 0.56).action, HC_NO_EVENT)
        XCTAssertEqual(frame(split, 0.60).action, HC_UP)
        XCTAssertEqual(hc_stop(&s).action, HC_NO_EVENT)
    }

    func testAmbiguousFistDoesNotDropUntilConfirmedButLossReleasesImmediately() {
        frame(open, 0); XCTAssertEqual(hold(pinch, from: 0.03).action, HC_DOWN)
        var curled = pinch; curled[8].y = 0.67; curled[4] = curled[8]
        XCTAssertEqual(frame(curled, 0.41).action, HC_NO_EVENT)
        XCTAssertEqual(frame(pinch, 0.44).action, HC_DRAGGED)
        frame(curled, 0.48)
        let pause = frame(curled, 0.52)
        XCTAssertEqual(pause.action, HC_UP)
        XCTAssertEqual(pause.mode, HC_PAUSE)
        frame(open, 0.55); XCTAssertEqual(hold(pinch, from: 0.59).action, HC_DOWN)
        frame(open, 0.96)
        var missing = pinch; missing[4] = HCPoint(x: -1, y: -1)
        XCTAssertEqual(frame(missing, 0.98).action, HC_UP)
        XCTAssertEqual(hc_lost(&s).action, HC_NO_EVENT)
    }

    func testSparseObservationsCannotArmADragOrConfirmADrop() {
        frame(open, 0); frame(pinch, 0.04)
        XCTAssertEqual(frame(pinch, 0.38).action, HC_NO_EVENT)
        frame(pinch, 0.42); frame(pinch, 0.54); frame(pinch, 0.65)
        XCTAssertEqual(frame(pinch, 0.72).action, HC_DOWN)
        frame(open, 0.76)
        XCTAssertEqual(frame(open, 0.95).action, HC_NO_EVENT)
        XCTAssertEqual(frame(open, 0.99).action, HC_UP)
    }

    func testPinchReleasesAtAnchorAndSecondTapIsDoubleClick() {
        let move = frame(open, 0)
        XCTAssertEqual(move.action, HC_POINTER)
        XCTAssertEqual(frame(pinch, 0.04).action, HC_NO_EVENT)
        frame(pinch, 0.07)
        XCTAssertEqual(frame(open, 0.12).action, HC_NO_EVENT)
        let first = frame(open, 0.15)
        XCTAssertEqual(first.action, HC_CLICK)
        XCTAssertEqual(first.position.x, move.position.x)
        XCTAssertEqual(first.clickCount, 1)
        frame(pinch, 0.20); frame(pinch, 0.23); frame(open, 0.28)
        let second = frame(open, 0.31)
        XCTAssertEqual(second.action, HC_CLICK)
        XCTAssertEqual(second.clickCount, 2)
    }

    func testTwoNearbyTargetsAreNotMergedIntoADoubleClick() {
        hc_init(&s, HCRange(left: 0, top: 0, right: 1, bottom: 1), 0.5)
        frame(open, 0); frame(pinch, 0.03); frame(pinch, 0.06); frame(open, 0.09)
        let first = frame(open, 0.12)
        XCTAssertEqual(first.action, HC_CLICK)
        let next = open.map { HCPoint(x: $0.x + 0.023, y: $0.y) }
        for i in 1...6 { frame(next, 0.12 + Double(i) * 0.03) }
        var grasp = next; grasp[4] = grasp[8]
        frame(grasp, 0.33); frame(grasp, 0.36); frame(next, 0.39)
        let second = frame(next, 0.42)
        XCTAssertEqual(second.action, HC_CLICK)
        XCTAssertEqual(second.clickCount, 1)
        XCTAssertGreaterThan(second.position.x - first.position.x, 0.015)
    }

    func testHoldDragDropsOnceWithoutClick() {
        frame(open, 1)
        XCTAssertEqual(hold(pinch, from: 1.03, duration: 0.33).action, HC_DOWN)
        var moved = pinch
        for i in moved.indices { moved[i].x += 0.1 }
        let drag = frame(moved, 1.40)
        XCTAssertEqual(drag.action, HC_DRAGGED)
        XCTAssertGreaterThan(drag.position.x, s.anchor.x)
        XCTAssertEqual(frame(open, 1.45).action, HC_NO_EVENT)
        XCTAssertEqual(frame(open, 1.48).action, HC_UP)
        XCTAssertEqual(frame(open, 1.52).action, HC_POINTER)
        XCTAssertEqual(hc_stop(&s).action, HC_NO_EVENT)
    }

    func testMissingOrStaleFramesReleaseAndReacquisitionCannotClick() {
        frame(open, 1); hold(pinch, from: 1.04)
        XCTAssertEqual(frame(pinch, 2.0).action, HC_UP)
        XCTAssertEqual(frame(pinch, 2.04).action, HC_NO_EVENT)
        XCTAssertEqual(frame(open, 2.10).action, HC_POINTER)
        frame(pinch, 2.14)
        var invalid = open; invalid[8].x = -1
        XCTAssertEqual(frame(invalid, 2.20).mode, HC_SEARCH)
        XCTAssertEqual(frame(open, 2.24).action, HC_POINTER)
        hold(pinch, from: 2.28)
        XCTAssertEqual(hc_lost(&s).action, HC_UP)
        XCTAssertEqual(hc_lost(&s).action, HC_NO_EVENT)
    }

    func testRightClickScrollAndFistHaveDistinctEffects() {
        frame(open, 0)
        var right = open; right[4] = right[12]
        XCTAssertEqual(frame(right, 0.04).mode, HC_RIGHT_PINCH)
        XCTAssertEqual(frame(right, 0.18).action, HC_NO_EVENT)
        XCTAssertEqual(frame(open, 0.21).action, HC_NO_EVENT)
        XCTAssertEqual(frame(open, 0.24).action, HC_RIGHT_CLICK)
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
        frame(open, 0); hold(pinch, from: 0.03)
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
        for (pose, t) in [(open,0.0), (pinch,0.04), (pinch,0.07), (open,0.10), (open,0.13),
                          (pinch,0.16), (pinch,0.19), (open,0.22), (open,0.25),
                          (pinch,0.28), (pinch,0.32), (pinch,0.45), (pinch,0.55),
                          (pinch,0.61), (pinch,0.65)] { mouse.apply(frame(pose,t)) }
        mouse.release()
        mouse.release()
        XCTAssertEqual(events.map(\.type), [.mouseMoved, .leftMouseDown, .leftMouseUp,
                                             .leftMouseDown, .leftMouseUp, .leftMouseDown, .leftMouseDragged, .leftMouseUp])
        XCTAssertEqual(events[3].getIntegerValueField(.mouseEventClickState), 2)
        XCTAssertEqual(events[4].getIntegerValueField(.mouseEventClickState), 2)
        var right = open; right[4] = right[12]
        var two = open; two[12].y = 0.34
        _ = hc_stop(&s)
        mouse.apply(frame(open,1)); mouse.apply(frame(right,1.04)); mouse.apply(frame(right,1.08))
        mouse.apply(frame(open,1.12)); mouse.apply(frame(open,1.15))
        mouse.apply(frame(two,1.18)); two[8].y -= 0.03; two[12].y -= 0.03
        mouse.apply(frame(two,1.22))
        XCTAssertEqual(events.suffix(3).map(\.type), [.rightMouseDown, .rightMouseUp, .scrollWheel])
    }

    func testClippedWristAndLowConfidenceCurledFingersStillTrackAndClick() {
        var clipped = open
        clipped[0] = HCPoint(x: -1, y: -1)
        var conf = Array(repeating: 0.88, count: 21)
        conf[0] = 0.0
        for j in 13...20 { conf[j] = 0.08 }

        let initial = hc_frame_weighted(&s, clipped, conf, nil, 16.0 / 9.0, 0.0)
        XCTAssertEqual(initial.mode, HC_MOVE)
        XCTAssertEqual(initial.action, HC_POINTER)

        var clippedPinch = clipped
        clippedPinch[4] = clippedPinch[8]
        _ = hc_frame_weighted(&s, clippedPinch, conf, nil, 16.0 / 9.0, 0.04)
        let held = hc_frame_weighted(&s, clippedPinch, conf, nil, 16.0 / 9.0, 0.07)
        XCTAssertEqual(held.mode, HC_PINCH)
        _ = hc_frame_weighted(&s, clipped, conf, nil, 16.0 / 9.0, 0.11)
        let released = hc_frame_weighted(&s, clipped, conf, nil, 16.0 / 9.0, 0.14)
        XCTAssertEqual(released.action, HC_CLICK)
        XCTAssertEqual(released.clickCount, 1)
    }

    func testShortOcclusionCoastsThroughActiveDragWhileLongLossReleasesSafely() {
        let fullConf = Array(repeating: 0.9, count: 21)
        _ = hc_frame_weighted(&s, open, fullConf, nil, 1, 0.0)
        var t = 0.03
        var down = HCOutput()
        while t <= 0.37 {
            down = hc_frame_weighted(&s, pinch, fullConf, nil, 1, t)
            t += 0.03
        }
        XCTAssertEqual(down.mode, HC_DRAG)

        let coast1 = hc_coast(&s, 0.40)
        let coast2 = hc_coast(&s, 0.44)
        XCTAssertEqual(coast1.mode, HC_DRAG)
        XCTAssertEqual(coast2.mode, HC_DRAG)
        XCTAssertNotEqual(coast2.action, HC_UP)

        let moved = pinch.map { HCPoint(x: $0.x + 0.06, y: $0.y) }
        let resumed = hc_frame_weighted(&s, moved, fullConf, nil, 1, 0.48)
        XCTAssertEqual(resumed.mode, HC_DRAG)
        XCTAssertEqual(resumed.action, HC_DRAGGED)

        _ = hc_coast(&s, 0.52)
        let expired = hc_coast(&s, 0.65)
        XCTAssertEqual(expired.action, HC_UP)
        XCTAssertEqual(expired.mode, HC_SEARCH)
    }

    func testVelocityPrePinchBrakeLocksAimFromRelaxedMidGapAndSlowClosing() {
        hc_init(&s, HCRange(left: 0, top: 0, right: 1, bottom: 1), 0.5)
        var midOpen = open
        midOpen[4] = HCPoint(x: 0.34, y: 0.44)
        _ = frame(midOpen, 0.0)
        let aimed = frame(midOpen, 0.04).position

        for step in 1...7 {
            let frac = Double(step) / 7.0
            var closing = midOpen
            closing[8].x += 0.014 * frac
            closing[8].y += 0.030 * frac
            closing[4].x = closing[8].x - (1.0 - frac) * 0.07
            closing[4].y = closing[8].y + (1.0 - frac) * 0.07
            let out = frame(closing, 0.04 + Double(step) * 0.035)
            if step >= 2 && out.mode == HC_MOVE {
                XCTAssertEqual(out.locked, 1)
            }
        }
        var closed = midOpen
        closed[8].x += 0.014; closed[8].y += 0.030
        closed[4] = closed[8]
        frame(closed, 0.32)
        frame(closed, 0.35)
        frame(midOpen, 0.39)
        let click = frame(midOpen, 0.42)
        XCTAssertEqual(click.action, HC_CLICK)
        XCTAssertLessThan(hypot(click.position.x - aimed.x, click.position.y - aimed.y) * 1600.0, 2.5)
    }

    func testInstantMoveToSelectStartsDragWithin80msAtExactAnchor() {
        hc_init(&s, HCRange(left: 0, top: 0, right: 1, bottom: 1), 0.5)
        let target = frame(open, 0.0).position
        frame(pinch, 0.03)
        frame(pinch, 0.06)
        let glided = pinch.map { HCPoint(x: $0.x + 0.022, y: $0.y) }
        let startDrag = frame(glided, 0.10)
        XCTAssertEqual(startDrag.mode, HC_DRAG)
        XCTAssertEqual(startDrag.action, HC_DOWN)
        XCTAssertEqual(startDrag.position.x, target.x, accuracy: 0.001)
        XCTAssertEqual(startDrag.position.y, target.y, accuracy: 0.001)

        let glidedFurther = pinch.map { HCPoint(x: $0.x + 0.055, y: $0.y) }
        let selecting = frame(glidedFurther, 0.13)
        XCTAssertEqual(selecting.action, HC_DRAGGED)
        XCTAssertGreaterThan(selecting.position.x, target.x + 0.01)
    }

    func testMagneticDoubleClickAndThreeFingerSynthesizedDoubleClick() {
        let firstAim = frame(open, 0.0).position
        frame(pinch, 0.04); frame(pinch, 0.07)
        frame(open, 0.10)
        let click1 = frame(open, 0.13)
        XCTAssertEqual(click1.action, HC_CLICK)
        XCTAssertEqual(click1.clickCount, 1)

        let recoiledOpen = open.map { HCPoint(x: $0.x + 0.008, y: $0.y - 0.005) }
        let recoiledPinch = pinch.map { HCPoint(x: $0.x + 0.008, y: $0.y - 0.005) }
        let magnetized = frame(recoiledOpen, 0.17)
        XCTAssertEqual(magnetized.locked, 1)
        XCTAssertEqual(magnetized.position.x, click1.position.x, accuracy: 0.0001)
        frame(recoiledPinch, 0.21); frame(recoiledPinch, 0.24)
        frame(recoiledOpen, 0.28)
        let click2 = frame(recoiledOpen, 0.31)
        XCTAssertEqual(click2.action, HC_CLICK)
        XCTAssertEqual(click2.clickCount, 2)
        XCTAssertEqual(click2.synthesized, 0)
        XCTAssertEqual(click2.position.x, firstAim.x, accuracy: 0.001)

        var events: [CGEvent] = []
        let mouse = DesktopMouse { events.append($0) }
        hc_init(&s, hc_default_range(), 0.5)
        let conf = Array(repeating: 0.92, count: 21)
        let worldOpen = open.map { HCPoint3D(x: $0.x * 0.18, y: $0.y * 0.18, z: 0.0) }
        _ = hc_frame_weighted(&s, open, conf, worldOpen, 1, 1.0)
        var triple = open
        triple[8] = HCPoint(x: 0.42, y: 0.38)
        triple[12] = HCPoint(x: 0.43, y: 0.39)
        triple[4] = HCPoint(x: 0.425, y: 0.385)
        var worldTriple = worldOpen
        worldTriple[8] = HCPoint3D(x: 0.075, y: 0.068, z: 0.0)
        worldTriple[12] = HCPoint3D(x: 0.077, y: 0.070, z: 0.004)
        worldTriple[4] = HCPoint3D(x: 0.076, y: 0.069, z: 0.002)

        _ = hc_frame_weighted(&s, triple, conf, worldTriple, 1, 1.04)
        let heldTriple = hc_frame_weighted(&s, triple, conf, worldTriple, 1, 1.08)
        XCTAssertEqual(heldTriple.mode, HC_DOUBLE_PINCH)
        _ = hc_frame_weighted(&s, open, conf, worldOpen, 1, 1.12)
        let releasedTriple = hc_frame_weighted(&s, open, conf, worldOpen, 1, 1.16)
        XCTAssertEqual(releasedTriple.action, HC_CLICK)
        XCTAssertEqual(releasedTriple.clickCount, 2)
        XCTAssertEqual(releasedTriple.synthesized, 1)

        mouse.apply(releasedTriple)
        XCTAssertEqual(events.map(\.type), [.leftMouseDown, .leftMouseUp, .leftMouseDown, .leftMouseUp])
        XCTAssertEqual(events[0].getIntegerValueField(.mouseEventClickState), 1)
        XCTAssertEqual(events[2].getIntegerValueField(.mouseEventClickState), 2)
    }
}
