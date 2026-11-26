#include "GestureCore.h"
#include <math.h>

static double clamp(double x, double lo, double hi) { return fmin(hi, fmax(x, lo)); }
static double distance(HCPoint a, HCPoint b, double aspect) {
    return hypot((a.x - b.x) * aspect, a.y - b.y);
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
static HCPoint smooth(HCPoint from, HCPoint to, double dt) {
    // Error is measured in fractions of the calibrated screen. Keep a nonzero
    // response even at rest so deliberate subpixel adjustments are never lost.
    const double steadyError = .0035, sweepError = .045;
    double u = clamp((distance(from, to, 1) - steadyError) / (sweepError - steadyError), 0, 1);
    double blend = u * u * (3 - 2 * u);
    double rate = (1 - blend) / .120 + blend / .008;
    double alpha = dt * rate / (1 + dt * rate);
    return (HCPoint){from.x + alpha * (to.x - from.x), from.y + alpha * (to.y - from.y)};
}
HCOutput hc_frame(HCState *s, const HCPoint p[21], double aspect, double now) {
    if (!p || !isfinite(aspect) || aspect <= 0 || !isfinite(now)) return hc_lost(s);
    const int used[] = {0, 4, 5, 6, 8, 9, 10, 12, 14, 16, 17, 18, 20};
    for (unsigned i = 0; i < sizeof used / sizeof *used; ++i) {
        HCPoint v = p[used[i]];
        if (!isfinite(v.x) || !isfinite(v.y) || v.x < 0 || v.y < 0 || v.x > 1 || v.y > 1)
            return hc_lost(s);
    }
    double palm = fmax(distance(p[0], p[9], aspect), distance(p[5], p[17], aspect));
    if (palm < .035) return hc_lost(s);
    double dt = clamp(now - s->lastTime, .001, .15);
    if ((s->hasCursor || s->dwelling) && (now - s->lastTime > .4 || now < s->lastTime)) {
        s->lastTime = now;
        return hc_lost(s);
    }
    s->lastTime = now;
    double reach[4];
    for (int i = 0; i < 4; ++i)
        reach[i] = distance(p[8 + 4*i], p[0], aspect) /
                   fmax(.01, distance(p[6 + 4*i], p[0], aspect));
    if (s->calStage) return calibrate(s, p[8], reach[0] > 1.15, aspect, now);

    double indexGap = distance(p[4], p[8], aspect) / palm;
    double middleGap = distance(p[4], p[12], aspect) / palm;
    int open = indexGap > .46 && middleGap > .46;
    int fist = reach[0] < 1.03 && reach[1] < 1.03 && reach[2] < 1.03 && reach[3] < 1.03;
    if (fist) {
        HCOutput out = hc_lost(s);
        out.mode = s->mode = HC_PAUSE;
        return out;
    }
    HCPoint target = mapped(s, p[8]);
    if (!s->hasCursor) {
        s->cursor = target;
        s->hasCursor = 1;
    }
    if (s->needOpen) {
        if (!open) {
            s->mode = HC_PAUSE;
            return output(s, HC_NO_EVENT);
        }
        s->needOpen = 0;
        s->mode = HC_MOVE;
    }
    if (s->mode == HC_PINCH || s->mode == HC_DRAG) {
        if (indexGap >= .46) {
            int dragging = s->mode == HC_DRAG;
            s->mode = HC_MOVE;
            s->needOpen = !open;
            if (dragging) return output(s, HC_UP);
            if (now - s->pinchTime < .035) return output(s, HC_NO_EVENT);
            HCOutput out = output(s, HC_CLICK);
            if (s->hasClick && now - s->clickTime <= s->doubleInterval &&
                distance(s->anchor, s->lastClick, 1) <= .028) {
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
        if (s->mode == HC_PINCH) {
            if (now - s->pinchTime < .32) return output(s, HC_NO_EVENT);
            s->mode = HC_DRAG;
            s->hasClick = 0;
            return output(s, HC_DOWN);
        }
        s->cursor = smooth(s->cursor, target, dt);
        return output(s, HC_DRAGGED);
    }
    if (s->mode == HC_RIGHT_PINCH) {
        if (middleGap < .46) return output(s, HC_NO_EVENT);
        s->mode = HC_MOVE;
        s->needOpen = !open;
        return output(s, now - s->pinchTime >= .035 ? HC_RIGHT_CLICK : HC_NO_EVENT);
    }
    if (fmin(indexGap, middleGap) <= .30) {
        s->mode = indexGap <= middleGap ? HC_PINCH : HC_RIGHT_PINCH;
        s->pinchTime = now;
        s->anchor = s->cursor;
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
        }
        s->scrollY = y;
        s->hasClick = 0;
        out.mode = s->mode = HC_SCROLL;
        return out;
    }
    s->mode = HC_MOVE;
    s->cursor = smooth(s->cursor, target, dt);
    return output(s, HC_POINTER);
}
