#include "GestureCore.h"
#include <math.h>

static double clamp(double x, double lo, double hi) { return fmin(hi, fmax(x, lo)); }
static double distance(HCPoint a, HCPoint b, double aspect) {
    return hypot((a.x - b.x) * aspect, a.y - b.y);
}
static int visible(HCPoint p) {
    return p.x >= 0 && p.x <= 1 && p.y >= 0 && p.y <= 1;
}
static void reset_motion(HCState *s) {
    s->hasTipHistory = 0;
    s->quietDirection = (HCPoint){0, 0};
}
static HCOutput output(const HCState *s, HCAction action) {
    return (HCOutput){.mode = s->mode, .action = action, .position = s->cursor, .clickCount = 1};
}
HCRange hc_default_range(void) { return (HCRange){.18, .16, .82, .82}; }
int hc_valid_range(HCRange r) {
    return r.left >= 0 && r.top >= 0 && r.right <= 1 && r.bottom <= 1 &&
           r.right - r.left >= .16 && r.bottom - r.top >= .14;
}
void hc_init(HCState *s, HCRange range, double doubleClickSeconds) {
    *s = (HCState){.range = hc_valid_range(range) ? range : hc_default_range(),
                   .doubleInterval = clamp(doubleClickSeconds, .15, 2), .needOpen = 1};
}
HCOutput hc_lost(HCState *s) {
    HCAction release = s->mode == HC_DRAG ? HC_UP : HC_NO_EVENT;
    s->mode = s->calStage == 1 ? HC_CAL_FIRST : s->calStage == 2 ? HC_CAL_SECOND : HC_SEARCH;
    s->needOpen = 1;
    s->dwelling = s->hasCursor = s->hasClick = 0;
    s->scrollRest = 0;
    s->approachArmed = 0;
    s->aimUntil = 0;
    reset_motion(s);
    return output(s, release);
}
HCOutput hc_stop(HCState *s) {
    s->calStage = 0;
    return hc_lost(s);
}
HCOutput hc_begin_calibration(HCState *s) {
    s->calStage = 1;
    return hc_lost(s);
}
static HCOutput calibrate(HCState *s, HCPoint tip, int pointing, double aspect, double now) {
    s->mode = s->calStage == 1 ? HC_CAL_FIRST : HC_CAL_SECOND;
    HCOutput out = output(s, HC_NO_EVENT);
    HCRange proposed = {s->firstCorner.x, s->firstCorner.y, tip.x, tip.y};
    if (!pointing || (s->calStage == 2 && !hc_valid_range(proposed))) {
        s->dwelling = 0;
        return out;
    }
    if (!s->dwelling || distance(tip, s->dwellAt, aspect) > .035) {
        s->dwellAt = tip;
        s->dwellTime = now;
        s->dwelling = 1;
    }
    out.progress = clamp((now - s->dwellTime) / .8, 0, 1);
    if (out.progress < 1) return out;
    s->dwelling = 0;
    if (s->calStage == 1) {
        s->firstCorner = tip;
        s->calStage = 2;
        s->mode = HC_CAL_SECOND;
    } else {
        s->range = proposed;
        s->calStage = 0;
        s->mode = HC_CAL_DONE;
        s->needOpen = 1;
    }
    return output(s, HC_NO_EVENT);
}
static HCPoint mapped(const HCState *s, HCPoint tip) {
    return (HCPoint){clamp((tip.x - s->range.left) / (s->range.right - s->range.left), 0, 1),
                     clamp((tip.y - s->range.top) / (s->range.bottom - s->range.top), 0, 1)};
}
static double median3(double a, double b, double c) {
    return fmax(fmin(a, b), fmin(fmax(a, b), c));
}
static HCPoint pointing_target(HCState *s, HCPoint tip, HCPoint palm) {
    // Median-filter finger motion relative to the palm, not the whole hand.
    // A real hand translation is available immediately, without a frame of delay.
    HCPoint offset = {tip.x - palm.x, tip.y - palm.y};
    if (!s->hasTipHistory) {
        s->tipOffsets[0] = s->tipOffsets[1] = offset;
        s->hasTipHistory = 1;
    }
    HCPoint a = s->tipOffsets[0], b = s->tipOffsets[1];
    s->tipOffsets[0] = b;
    s->tipOffsets[1] = offset;
    HCPoint target = mapped(s, (HCPoint){palm.x + median3(a.x, b.x, offset.x),
                                         palm.y + median3(a.y, b.y, offset.y)});
    HCPoint raw = mapped(s, tip);
    return (HCPoint){clamp(target.x, fmin(s->cursor.x, raw.x), fmax(s->cursor.x, raw.x)),
                     clamp(target.y, fmin(s->cursor.y, raw.y), fmax(s->cursor.y, raw.y))};
}
static HCPoint smooth(HCState *s, HCPoint to, double dt) {
    HCPoint from = s->cursor, delta = {to.x - from.x, to.y - from.y};
    const double steadyError = .004, sweepError = .045;
    double error = hypot(delta.x, delta.y);
    if (error <= steadyError) {
        // Alternating small displacements are tremor; a sustained fine adjustment is not.
        if (delta.x * s->quietDirection.x + delta.y * s->quietDirection.y <= 0) {
            s->quietDirection = delta;
            s->quietSince = s->lastTime;
        }
        if (s->lastTime - s->quietSince < .080) return from;
    } else {
        s->quietDirection = (HCPoint){0, 0};
    }
    double u = clamp((error - steadyError) / (sweepError - steadyError), 0, 1);
    double blend = u * u * (3 - 2 * u);
    double rate = (1 - blend) / .120 + blend / .008;
    double alpha = dt * rate / (1 + dt * rate);
    return (HCPoint){from.x + alpha * (to.x - from.x), from.y + alpha * (to.y - from.y)};
}
static const double pinchClose = .30, pinchOpen = .46;
enum { NO_RELEASE, OPEN_RELEASE, FIST_RELEASE };

static int confirmed_release(HCState *s, int observation, double now) {
    if (s->pendingRelease != observation) {
        s->pendingRelease = observation;
        s->releaseSince = now;
        return 0;
    }
    return now - s->releaseSince >= .025;
}

static HCOutput track_pinch(HCState *s, double gap, int open, HCPoint palm, double dt, double now) {
    int right = s->mode == HC_RIGHT_PINCH;
    if (!s->pinchConfirmed) {
        if (gap > .36) {
            s->mode = HC_MOVE;
            s->needOpen = !open;
            return output(s, HC_NO_EVENT);
        }
        if (now - s->pinchTime < .025) return output(s, HC_NO_EVENT);
        s->pinchConfirmed = 1;
    }
    if (gap >= pinchOpen) {
        if (!confirmed_release(s, OPEN_RELEASE, now)) return output(s, HC_NO_EVENT);
        int dragging = s->mode == HC_DRAG;
        s->mode = HC_MOVE;
        s->needOpen = !open;
        reset_motion(s);
        if (dragging) return output(s, HC_UP);
        if (right) return output(s, HC_RIGHT_CLICK);
        HCOutput out = output(s, HC_CLICK);
        if (s->hasClick && now - s->clickTime <= s->doubleInterval &&
            distance(s->anchor, s->lastClick, 1) <= .009) {
            out.clickCount = 2;
            out.position = s->cursor = s->lastClick;
            s->hasClick = 0;
        } else {
            s->lastClick = s->anchor;
            s->clickTime = now;
            s->hasClick = 1;
        }
        return out;
    }
    s->pendingRelease = NO_RELEASE;
    if (right) return output(s, HC_NO_EVENT);
    if (s->mode == HC_PINCH) {
        if (now - s->pinchTime < .32) return output(s, HC_NO_EVENT);
        s->mode = HC_DRAG;
        s->hasClick = 0;
        s->dragOrigin = palm;
        return output(s, HC_DOWN);
    }
    HCRange r = s->range;
    HCPoint target = {clamp(s->anchor.x + (palm.x - s->dragOrigin.x) / (r.right - r.left), 0, 1),
                      clamp(s->anchor.y + (palm.y - s->dragOrigin.y) / (r.bottom - r.top), 0, 1)};
    s->cursor = smooth(s, target, dt);
    return output(s, HC_DRAGGED);
}
HCOutput hc_frame(HCState *s, const HCPoint p[21], double aspect, double now) {
    if (!p || !isfinite(aspect) || aspect <= 0 || !isfinite(now)) return hc_lost(s);
    const int essential[] = {0, 4, 5, 8, 9, 17};
    for (unsigned i = 0; i < sizeof essential / sizeof *essential; ++i)
        if (!visible(p[essential[i]])) return hc_lost(s);
    int left = s->mode == HC_PINCH || s->mode == HC_DRAG;
    int activePinch = left || s->mode == HC_RIGHT_PINCH;
    if (!left && !visible(p[12])) return hc_lost(s);
    double palm = fmax(distance(p[0], p[9], aspect), distance(p[5], p[17], aspect));
    if (palm < .035) return hc_lost(s);
    double dt = clamp(now - s->lastTime, .001, .15);
    if ((s->hasCursor || s->dwelling) && (now - s->lastTime > .4 || now < s->lastTime)) {
        s->lastTime = now;
        return hc_lost(s);
    }
    if (activePinch && now - s->lastTime > .15) {
        s->pendingRelease = NO_RELEASE;
        if (s->mode != HC_DRAG) {
            s->pinchTime = now;
            s->pinchConfirmed = 0;
        }
    }
    s->lastTime = now;
    double reach[4];
    int fullPose = 1;
    for (int i = 0; i < 4; ++i) {
        HCPoint tip = p[8 + 4*i], pip = p[6 + 4*i];
        reach[i] = visible(tip) && visible(pip)
                   ? distance(tip, p[0], aspect) / fmax(.01, distance(pip, p[0], aspect)) : NAN;
        if (!isfinite(reach[i])) fullPose = 0;
    }
    if (s->calStage) return calibrate(s, p[8], reach[0] > 1.15, aspect, now);

    double indexGap = distance(p[4], p[8], aspect) / palm;
    double middleGap = visible(p[12]) ? distance(p[4], p[12], aspect) / palm : NAN;
    int open = indexGap > pinchOpen && middleGap > pinchOpen;
    if (!(activePinch && s->pinchConfirmed) && !fullPose && !(open && reach[0] > 1.15 && reach[1] < 1.12))
        return hc_lost(s);
    int fist = reach[0] < 1.03 && reach[1] < 1.03 && reach[2] < 1.03 && reach[3] < 1.03;
    if (fist) {
        // A still-visible pinch can look curled for one frame. Freeze at once,
        // but require a second fist observation before cancelling a held button.
        if (activePinch && s->pinchConfirmed && (left ? indexGap : middleGap) < pinchOpen &&
            !confirmed_release(s, FIST_RELEASE, now)) return output(s, HC_NO_EVENT);
        HCOutput out = hc_lost(s);
        out.mode = s->mode = HC_PAUSE;
        return out;
    }
    if (s->needOpen) {
        if (!open) {
            s->mode = HC_PAUSE;
            return output(s, HC_NO_EVENT);
        }
        s->needOpen = 0;
        s->mode = HC_MOVE;
    }
    if (!s->hasCursor) {
        s->cursor = mapped(s, p[8]);
        s->hasCursor = 1;
    }
    HCPoint center = {(p[0].x + p[5].x + p[9].x + p[17].x) / 4,
                      (p[0].y + p[5].y + p[9].y + p[17].y) / 4};
    if (activePinch) return track_pinch(s, left ? indexGap : middleGap, open, center, dt, now);
    double gap = fmin(indexGap, middleGap);
    if (gap <= pinchClose) {
        s->mode = indexGap <= middleGap ? HC_PINCH : HC_RIGHT_PINCH;
        s->pinchTime = now;
        s->pinchConfirmed = s->approachArmed = 0;
        s->pendingRelease = NO_RELEASE;
        s->aimUntil = 0;
        s->anchor = s->cursor;
        reset_motion(s);
        if (s->mode == HC_RIGHT_PINCH) s->hasClick = 0;
        return output(s, HC_NO_EVENT);
    }
    if (reach[0] > 1.15 && reach[1] > 1.15 && reach[2] < 1.12 && reach[3] < 1.12) {
        double y = (p[8].y + p[12].y) * .5;
        HCOutput out = output(s, HC_NO_EVENT);
        if (s->mode == HC_SCROLL) {
            double pixels = s->scrollRest + clamp((s->scrollY - y) * 1500, -180, 180);
            out.wheel = (int)pixels;
            s->scrollRest = pixels - out.wheel;
            if (out.wheel) out.action = HC_WHEEL;
        } else {
            s->scrollRest = 0;
            s->approachArmed = 0;
            s->aimUntil = 0;
            reset_motion(s);
        }
        s->scrollY = y;
        s->hasClick = 0;
        out.mode = s->mode = HC_SCROLL;
        return out;
    }
    s->mode = HC_MOVE;
    // Freeze the last aim before the fingers touch. Cap the hold so hovering
    // with the thumb nearby cannot indefinitely disable pointing.
    if (gap >= .70) {
        s->approachArmed = 1;
        s->aimUntil = 0;
    } else if (gap <= .60 && s->approachArmed) {
        s->approachArmed = 0;
        s->aimUntil = now + .16;
    }
    if (now < s->aimUntil) return output(s, HC_NO_EVENT);
    s->cursor = smooth(s, pointing_target(s, p[8], center), dt);
    return output(s, HC_POINTER);
}
