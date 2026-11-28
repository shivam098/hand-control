#include "GestureCore.h"
#include <math.h>
#include <stddef.h>

static double clamp(double x, double lo, double hi) { return fmin(hi, fmax(x, lo)); }
static double distance(HCPoint a, HCPoint b, double aspect) {
    return hypot((a.x - b.x) * aspect, a.y - b.y);
}
static double distance3(HCPoint3D a, HCPoint3D b) {
    double dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return sqrt(dx * dx + dy * dy + dz * dz);
}
static int visible(HCPoint p) {
    return isfinite(p.x) && isfinite(p.y) && p.x >= 0 && p.x <= 1 && p.y >= 0 && p.y <= 1;
}
static int finite3(HCPoint3D p) {
    return isfinite(p.x) && isfinite(p.y) && isfinite(p.z);
}
static void reset_motion(HCState *s) {
    s->hasTipHistory = 0;
    s->hasPrevGap = 0;
    s->quietDirection = (HCPoint){0, 0};
}
static HCOutput output_ex(const HCState *s, HCAction action, double pinchLevel, double quality, int locked) {
    return (HCOutput){
        .mode = s->mode, .action = action, .position = s->cursor, .clickCount = 1,
        .pinchLevel = clamp(pinchLevel, 0, 1), .quality = clamp(quality, 0, 1), .locked = locked
    };
}
static HCOutput output(const HCState *s, HCAction action) {
    return output_ex(s, action, 0, 1, 0);
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
    s->hasJointHistory = s->hasFingerOffset = 0;
    s->scrollRest = 0;
    s->approachArmed = 0;
    s->aimUntil = s->brakeStart = 0;
    s->pinchConfirmed = s->pinchFrames = s->pendingRelease = 0;
    reset_motion(s);
    return output_ex(s, release, 0, 0, 0);
}
HCOutput hc_coast(HCState *s, double now) {
    if (!isfinite(now) || !s->hasCursor || s->calStage || now < s->lastTime ||
        now - s->lastValidTime > .100) {
        if (isfinite(now)) s->lastTime = now;
        return hc_lost(s);
    }
    s->lastTime = now;
    double quality = clamp(1.0 - (now - s->lastValidTime) / .125, .25, .85);
    double pinch = (s->mode == HC_PINCH || s->mode == HC_RIGHT_PINCH ||
                    s->mode == HC_DOUBLE_PINCH || s->mode == HC_DRAG) ? 1.0 : 0.0;
    return output_ex(s, HC_NO_EVENT, pinch, quality, s->mode == HC_DRAG);
}
HCOutput hc_stop(HCState *s) {
    s->calStage = 0;
    return hc_lost(s);
}
HCOutput hc_begin_calibration(HCState *s) {
    s->calStage = 1;
    return hc_lost(s);
}
static HCOutput calibrate(HCState *s, HCPoint tip, int pointing, double aspect, double now, double quality) {
    s->mode = s->calStage == 1 ? HC_CAL_FIRST : HC_CAL_SECOND;
    HCOutput out = output_ex(s, HC_NO_EVENT, 0, quality, 0);
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
    return output_ex(s, HC_NO_EVENT, 0, quality, 0);
}
static HCPoint mapped(const HCState *s, HCPoint tip) {
    return (HCPoint){clamp((tip.x - s->range.left) / (s->range.right - s->range.left), 0, 1),
                     clamp((tip.y - s->range.top) / (s->range.bottom - s->range.top), 0, 1)};
}
static double median3(double a, double b, double c) {
    return fmax(fmin(a, b), fmin(fmax(a, b), c));
}
static HCPoint knuckle_hub(const HCPoint p[21], const double conf[21]) {
    static const int ids[] = {5, 9, 13, 17, 0};
    static const double weights[] = {.50, .24, .14, .07, .05};
    double sx = 0, sy = 0, sw = 0;
    for (unsigned i = 0; i < sizeof ids / sizeof *ids; ++i) {
        int id = ids[i];
        double minC = id == 0 ? .28 : .12;
        if (visible(p[id]) && conf[id] >= minC) {
            double w = weights[i] * clamp(conf[id], .2, 1.0);
            sx += p[id].x * w;
            sy += p[id].y * w;
            sw += w;
        }
    }
    if (sw <= 0) return p[5];
    return (HCPoint){sx / sw, sy / sw};
}
static HCPoint pointing_target(HCState *s, HCPoint tip, HCPoint hub, double gap, double dt) {
    // Anchor translation to the knuckle/palm hub while stabilizing open-hand finger offset.
    // Whenever fingers approach a pinch (gap < 0.72), freeze the finger offset so thumb-index
    // flexion cannot pull the pointer away from the target.
    HCPoint offset = {tip.x - hub.x, tip.y - hub.y};
    if (!s->hasTipHistory) {
        s->tipOffsets[0] = s->tipOffsets[1] = offset;
        s->hasTipHistory = 1;
    }
    HCPoint a = s->tipOffsets[0], b = s->tipOffsets[1];
    s->tipOffsets[0] = b;
    s->tipOffsets[1] = offset;
    HCPoint med = {median3(a.x, b.x, offset.x), median3(a.y, b.y, offset.y)};
    if (!s->hasFingerOffset) {
        s->stableFingerOffset = med;
        s->hasFingerOffset = 1;
    } else if (gap >= .72) {
        double shift = hypot(med.x - s->stableFingerOffset.x, med.y - s->stableFingerOffset.y);
        double u = clamp((shift - .005) / .012, 0, 1);
        double blend = u * u * (3 - 2 * u);
        double rate = (1 - blend) * 1.4 + blend * 120.0;
        double alpha = dt * rate / (1 + dt * rate);
        s->stableFingerOffset.x += alpha * (med.x - s->stableFingerOffset.x);
        s->stableFingerOffset.y += alpha * (med.y - s->stableFingerOffset.y);
    }
    HCPoint aim = {hub.x + s->stableFingerOffset.x, hub.y + s->stableFingerOffset.y};
    HCPoint target = mapped(s, aim);
    if (gap < .72) return target;
    HCPoint raw = mapped(s, tip);
    return (HCPoint){clamp(target.x, fmin(s->cursor.x, raw.x), fmax(s->cursor.x, raw.x)),
                     clamp(target.y, fmin(s->cursor.y, raw.y), fmax(s->cursor.y, raw.y))};
}
static HCPoint smooth(HCState *s, HCPoint to, double dt) {
    HCPoint from = s->cursor, delta = {to.x - from.x, to.y - from.y};
    const double steadyError = .0042, sweepError = .032;
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
    // 5th-order smootherstep precision acceleration curve: low gain for pixel-accurate
    // micro-positioning, ramping seamlessly to <8 ms latency on sweeps.
    double blend = u * u * u * (u * (u * 6 - 15) + 10);
    double rate = (1 - blend) * 5.2 + blend * 140.0;
    double alpha = dt * rate / (1 + dt * rate);
    return (HCPoint){from.x + alpha * (to.x - from.x), from.y + alpha * (to.y - from.y)};
}

static const double pinchClose = .30, tripleClose = .34, pinchOpen = .46, dragOpen = .52;
static const double magneticDoubleRadius = .0175;
enum { NO_RELEASE, OPEN_RELEASE, FIST_RELEASE };

static double pinch_level_from_gap(double gap) {
    if (!isfinite(gap)) return 0;
    return clamp((.70 - gap) / (.70 - pinchClose), 0, 1);
}
static int confirmed_release(HCState *s, int observation, double now) {
    if (s->pendingRelease != observation) {
        s->pendingRelease = observation;
        s->releaseSince = now;
        return 0;
    }
    return now - s->releaseSince >= .025;
}

static HCOutput track_pinch(HCState *s, double indexGap, double middleGap, int open,
                            HCPoint hub, double dt, double now, double quality) {
    int right = s->mode == HC_RIGHT_PINCH;
    int triple = s->mode == HC_DOUBLE_PINCH;
    // Allow a 2-finger pinch to upgrade to a 3-finger double-click pinch during its initial window
    // if thumb, index, and middle all come together within 120 ms before a drag begins.
    if (!triple && s->mode != HC_DRAG && now - s->pinchTime <= .12 &&
        isfinite(middleGap) && indexGap <= tripleClose && middleGap <= tripleClose) {
        s->mode = HC_DOUBLE_PINCH;
        triple = 1;
        right = 0;
    }
    double activeGap = triple ? fmax(indexGap, isfinite(middleGap) ? middleGap : indexGap)
                              : (right ? middleGap : indexGap);
    if (!s->pinchConfirmed) {
        double cancelGap = triple ? .40 : .36;
        if (!isfinite(activeGap) || activeGap > cancelGap) {
            s->mode = HC_MOVE;
            s->needOpen = !open;
            return output_ex(s, HC_NO_EVENT, pinch_level_from_gap(activeGap), quality, 0);
        }
        s->pinchFrames += 1;
        if (now - s->pinchTime < .025)
            return output_ex(s, HC_NO_EVENT, 1.0, quality, 1);
        s->pinchConfirmed = 1;
    } else {
        s->pinchFrames += 1;
    }

    double releaseThreshold = s->mode == HC_DRAG ? dragOpen : pinchOpen;
    int released = triple ? (indexGap >= pinchOpen && (!isfinite(middleGap) || middleGap >= pinchOpen))
                          : (!isfinite(activeGap) || activeGap >= releaseThreshold);
    if (released) {
        if (!confirmed_release(s, OPEN_RELEASE, now))
            return output_ex(s, HC_NO_EVENT, .6, quality, 1);
        int dragging = s->mode == HC_DRAG;
        s->mode = HC_MOVE;
        s->needOpen = !open;
        reset_motion(s);
        if (dragging) return output_ex(s, HC_UP, 0, quality, 0);
        if (right) return output_ex(s, HC_RIGHT_CLICK, 0, quality, 0);
        if (triple) {
            HCOutput out = output_ex(s, HC_CLICK, 0, quality, 0);
            out.clickCount = 2;
            out.synthesized = 1;
            out.position = s->cursor = s->anchor;
            s->hasClick = 0;
            return out;
        }
        HCOutput out = output_ex(s, HC_CLICK, 0, quality, 0);
        if (s->hasClick && now - s->clickTime <= s->doubleInterval &&
            distance(s->anchor, s->lastClick, 1) <= magneticDoubleRadius) {
            out.clickCount = 2;
            out.position = s->cursor = s->lastClick;
            s->hasClick = 0;
        } else {
            out.position = s->cursor = s->anchor;
            s->lastClick = s->anchor;
            s->clickTime = now;
            s->hasClick = 1;
        }
        return out;
    }

    s->pendingRelease = NO_RELEASE;
    if (right || triple) return output_ex(s, HC_NO_EVENT, 1.0, quality, 1);

    HCRange r = s->range;
    double palmTravel = hypot((hub.x - s->dragOrigin.x) / (r.right - r.left),
                              (hub.y - s->dragOrigin.y) / (r.bottom - r.top));
    if (s->mode == HC_PINCH) {
        int instantSelect = now - s->pinchTime >= .030 && s->pinchFrames >= 2 && palmTravel >= .014;
        int holdDrag = now - s->pinchTime >= .32;
        if (!instantSelect && !holdDrag)
            return output_ex(s, HC_NO_EVENT, 1.0, quality, 1);
        s->mode = HC_DRAG;
        s->hasClick = 0;
        if (holdDrag && palmTravel < .014) s->dragOrigin = hub;
        s->cursor = s->anchor;
        return output_ex(s, HC_DOWN, 1.0, quality, 1);
    }
    HCPoint target = {clamp(s->anchor.x + (hub.x - s->dragOrigin.x) / (r.right - r.left), 0, 1),
                      clamp(s->anchor.y + (hub.y - s->dragOrigin.y) / (r.bottom - r.top), 0, 1)};
    s->cursor = smooth(s, target, dt);
    return output_ex(s, HC_DRAGGED, 1.0, quality, 1);
}

static double compute_palm_scale(const HCPoint p[21], const double conf[21], double aspect, HCPoint *wristOut) {
    int hasWrist = visible(p[0]) && conf[0] >= .18;
    double scale = 0;
    if (hasWrist && visible(p[9]) && conf[9] >= .12)
        scale = fmax(scale, distance(p[0], p[9], aspect));
    if (visible(p[5]) && conf[5] >= .12 && visible(p[17]) && conf[17] >= .12) {
        double span = distance(p[5], p[17], aspect);
        scale = fmax(scale, hasWrist ? span : span * 1.25);
    }
    if (scale < .035 && hasWrist && visible(p[5]) && conf[5] >= .12)
        scale = fmax(scale, distance(p[0], p[5], aspect) * 1.04);
    if (scale < .035 && visible(p[5]) && conf[5] >= .12 && visible(p[13]) && conf[13] >= .12)
        scale = fmax(scale, distance(p[5], p[13], aspect) * 1.90);
    if (scale < .035 && visible(p[5]) && conf[5] >= .12 && visible(p[9]) && conf[9] >= .12)
        scale = fmax(scale, distance(p[5], p[9], aspect) * 3.15);

    if (hasWrist) {
        *wristOut = p[0];
    } else {
        double sx = 0, sy = 0, n = 0;
        const int knuckles[] = {5, 9, 13, 17};
        for (unsigned i = 0; i < sizeof knuckles / sizeof *knuckles; ++i) {
            int k = knuckles[i];
            if (visible(p[k]) && conf[k] >= .12) { sx += p[k].x; sy += p[k].y; n += 1; }
        }
        *wristOut = n > 0 ? (HCPoint){sx / n, clamp(sy / n + .95 * scale, 0, 1.35)} : (HCPoint){.5, 1.0};
    }
    return scale;
}

HCOutput hc_frame_weighted(HCState *s, const HCPoint rawPoints[21], const double confidence[21],
                           const HCPoint3D *worldPoints, double aspect, double now) {
    if (!s || !isfinite(aspect) || aspect <= 0 || !isfinite(now)) return s ? hc_lost(s) : (HCOutput){0};
    if ((s->hasCursor || s->dwelling) && (now - s->lastTime > .4 || now < s->lastTime)) {
        s->lastTime = now;
        return hc_lost(s);
    }
    if (!rawPoints) return confidence ? hc_coast(s, now) : hc_lost(s);

    HCPoint p[21];
    double conf[21];
    int weighted = confidence != NULL;
    double dt = s->lastTime > 0 ? clamp(now - s->lastTime, .001, .15) : (1.0 / 30.0);
    for (int i = 0; i < 21; ++i) {
        double c = weighted ? (isfinite(confidence[i]) ? clamp(confidence[i], 0, 1) : 0)
                            : (visible(rawPoints[i]) ? 1.0 : 0.0);
        if (!visible(rawPoints[i])) c = 0;
        if (weighted && s->hasJointHistory && visible(s->filteredJoints[i])) {
            if (c >= .08) {
                // Confidence-weighted landmark blend stabilizes noisy low-confidence Vision/MediaPipe joints.
                double trust = clamp((c - .05) / .55, .15, 1.0);
                p[i] = (HCPoint){s->filteredJoints[i].x + trust * (rawPoints[i].x - s->filteredJoints[i].x),
                                 s->filteredJoints[i].y + trust * (rawPoints[i].y - s->filteredJoints[i].y)};
                conf[i] = fmax(c, s->jointConf[i] * .65);
            } else if (now - s->lastValidTime <= .100 && s->jointConf[i] >= .18) {
                p[i] = s->filteredJoints[i];
                conf[i] = s->jointConf[i] * .75;
            } else {
                p[i] = (HCPoint){-1, -1};
                conf[i] = 0;
            }
        } else {
            p[i] = c > 0 ? rawPoints[i] : (HCPoint){-1, -1};
            conf[i] = c;
        }
    }

    int haveTips = visible(p[4]) && conf[4] >= .14 && visible(p[8]) && conf[8] >= .14;
    int haveKnuckle = visible(p[5]) && conf[5] >= .12 &&
                      ((visible(p[0]) && conf[0] >= .18) || (visible(p[9]) && conf[9] >= .12) ||
                       (visible(p[13]) && conf[13] >= .12) || (visible(p[17]) && conf[17] >= .12));
    if (!haveTips || !haveKnuckle)
        return weighted ? hc_coast(s, now) : hc_lost(s);

    HCPoint baseWrist;
    double palm = compute_palm_scale(p, conf, aspect, &baseWrist);
    if (palm < .035)
        return weighted ? hc_coast(s, now) : hc_lost(s);

    for (int i = 0; i < 21; ++i) {
        s->filteredJoints[i] = p[i];
        s->jointConf[i] = conf[i];
    }
    s->hasJointHistory = 1;
    double rawEssential = fmin(weighted ? confidence[4] : 1.0, weighted ? confidence[8] : 1.0);
    if (!weighted || (rawEssential >= .16 && visible(rawPoints[4]) && visible(rawPoints[8])))
        s->lastValidTime = now;
    double quality = weighted ? clamp((conf[4] + conf[8] + conf[5]) / 2.2, .30, 1.0) : 1.0;

    int left = s->mode == HC_PINCH || s->mode == HC_DRAG;
    int activePinch = left || s->mode == HC_RIGHT_PINCH || s->mode == HC_DOUBLE_PINCH;
    if (activePinch && now - s->lastTime > .15) {
        s->pendingRelease = NO_RELEASE;
        if (s->mode != HC_DRAG) {
            s->pinchTime = now;
            s->pinchConfirmed = 0;
            s->pinchFrames = 0;
        }
    }
    s->lastTime = now;

    double reach[4];
    int visibleFingers = 0, curledFingers = 0;
    for (int i = 0; i < 4; ++i) {
        HCPoint tip = p[8 + 4 * i], pip = p[6 + 4 * i];
        double tc = conf[8 + 4 * i], pc = conf[6 + 4 * i];
        if (visible(tip) && visible(pip) && tc >= .12 && pc >= .12) {
            double dTip = hypot((tip.x - baseWrist.x) * aspect, tip.y - baseWrist.y);
            double dPip = fmax(.01, hypot((pip.x - baseWrist.x) * aspect, pip.y - baseWrist.y));
            reach[i] = dTip / dPip;
            visibleFingers += 1;
            if (reach[i] < 1.03) curledFingers += 1;
        } else {
            reach[i] = NAN;
        }
    }
    if (s->calStage) return calibrate(s, p[8], isfinite(reach[0]) && reach[0] > 1.15, aspect, now, quality);

    double indexGap = distance(p[4], p[8], aspect) / palm;
    int hasMiddle = visible(p[12]) && conf[12] >= .14;
    double middleGap = hasMiddle ? distance(p[4], p[12], aspect) / palm : NAN;
    if (worldPoints && finite3(worldPoints[4]) && finite3(worldPoints[8])) {
        double wScale = 0;
        if (finite3(worldPoints[0]) && finite3(worldPoints[9]))
            wScale = fmax(wScale, distance3(worldPoints[0], worldPoints[9]));
        if (finite3(worldPoints[5]) && finite3(worldPoints[17]))
            wScale = fmax(wScale, distance3(worldPoints[5], worldPoints[17]) * 1.25);
        if (wScale > .03) {
            double idx3 = distance3(worldPoints[4], worldPoints[8]) / wScale;
            indexGap = .55 * indexGap + .45 * idx3;
            if (hasMiddle && finite3(worldPoints[12])) {
                double mid3 = distance3(worldPoints[4], worldPoints[12]) / wScale;
                middleGap = .55 * middleGap + .45 * mid3;
            }
        }
    }

    int open = indexGap > pinchOpen && (!isfinite(middleGap) || middleGap > pinchOpen);
    int fist = isfinite(reach[0]) && reach[0] < 1.03 && visibleFingers >= 2 && curledFingers == visibleFingers;
    if (fist) {
        double heldGap = s->mode == HC_RIGHT_PINCH ? middleGap : indexGap;
        if (activePinch && s->pinchConfirmed && isfinite(heldGap) && heldGap < pinchOpen &&
            !confirmed_release(s, FIST_RELEASE, now))
            return output_ex(s, HC_NO_EVENT, 1.0, quality, 1);
        HCOutput out = hc_lost(s);
        out.mode = s->mode = HC_PAUSE;
        out.quality = quality;
        return out;
    }
    if (s->needOpen) {
        if (!open) {
            s->mode = HC_PAUSE;
            return output_ex(s, HC_NO_EVENT, pinch_level_from_gap(indexGap), quality, 0);
        }
        s->needOpen = 0;
        s->mode = HC_MOVE;
    }

    HCPoint hub = knuckle_hub(p, conf);
    if (!s->hasCursor) {
        HCPoint initTarget = pointing_target(s, p[8], hub, 1.0, dt);
        s->cursor = initTarget;
        s->hasCursor = 1;
    }
    if (activePinch)
        return track_pinch(s, indexGap, middleGap, open, hub, dt, now, quality);

    double gap = isfinite(middleGap) ? fmin(indexGap, middleGap) : indexGap;
    double gapRate = s->hasPrevGap ? (gap - s->prevGap) / dt : 0;
    s->prevGap = gap;
    s->hasPrevGap = 1;
    double pLevel = pinch_level_from_gap(gap);

    if (isfinite(middleGap) && indexGap <= tripleClose && middleGap <= tripleClose) {
        s->mode = HC_DOUBLE_PINCH;
        s->pinchTime = now;
        s->pinchConfirmed = s->pinchFrames = s->approachArmed = 0;
        s->pendingRelease = NO_RELEASE;
        s->aimUntil = s->brakeStart = 0;
        s->anchor = s->cursor;
        s->dragOrigin = hub;
        reset_motion(s);
        return output_ex(s, HC_NO_EVENT, 1.0, quality, 1);
    }
    if (gap <= pinchClose) {
        s->mode = (!isfinite(middleGap) || indexGap <= middleGap) ? HC_PINCH : HC_RIGHT_PINCH;
        s->pinchTime = now;
        s->pinchConfirmed = s->pinchFrames = s->approachArmed = 0;
        s->pendingRelease = NO_RELEASE;
        s->aimUntil = s->brakeStart = 0;
        if (s->mode == HC_PINCH && s->hasClick && now - s->clickTime <= s->doubleInterval &&
            distance(s->cursor, s->lastClick, 1) <= magneticDoubleRadius) {
            s->cursor = s->lastClick;
        }
        s->anchor = s->cursor;
        s->dragOrigin = hub;
        reset_motion(s);
        if (s->mode == HC_RIGHT_PINCH) s->hasClick = 0;
        return output_ex(s, HC_NO_EVENT, 1.0, quality, 1);
    }
    if (isfinite(reach[0]) && isfinite(reach[1]) && reach[0] > 1.15 && reach[1] > 1.15 &&
        (!isfinite(reach[2]) || reach[2] < 1.12) && (!isfinite(reach[3]) || reach[3] < 1.12)) {
        double y = (p[8].y + p[12].y) * .5;
        HCOutput out = output_ex(s, HC_NO_EVENT, 0, quality, 0);
        if (s->mode == HC_SCROLL) {
            double pixels = s->scrollRest + clamp((s->scrollY - y) * 1500, -180, 180);
            out.wheel = (int)pixels;
            s->scrollRest = pixels - out.wheel;
            if (out.wheel) out.action = HC_WHEEL;
        } else {
            s->scrollRest = 0;
            s->approachArmed = 0;
            s->aimUntil = s->brakeStart = 0;
            reset_motion(s);
        }
        s->scrollY = y;
        s->hasClick = 0;
        out.mode = s->mode = HC_SCROLL;
        return out;
    }

    s->mode = HC_MOVE;
    // Velocity-based pre-pinch aim brake: freeze the target whenever fingers are actively
    // closing toward a pinch from any starting gap, capped at 380 ms so stationary hover
    // never permanently disables pointing.
    if (gap >= .68 || gapRate > .35) {
        s->approachArmed = 1;
        s->aimUntil = s->brakeStart = 0;
    } else if (gap < .68 && (gapRate < -.55 || (s->approachArmed && gap <= .60))) {
        if (s->brakeStart == 0) {
            s->brakeStart = now;
            s->minApproachGap = gap;
        }
        if (gap <= s->minApproachGap + .02 && now - s->brakeStart <= .38) {
            s->minApproachGap = fmin(s->minApproachGap, gap);
            s->aimUntil = fmax(s->aimUntil, now + .14);
        }
        s->approachArmed = 0;
    }
    if (now < s->aimUntil)
        return output_ex(s, HC_NO_EVENT, pLevel, quality, 1);

    HCPoint desired = pointing_target(s, p[8], hub, gap, dt);
    // Magnetic double-click lock: keep cursor snapped onto lastClick within doubleInterval
    // unless intentional whole-hand travel moves past magneticDoubleRadius.
    if (s->hasClick && now - s->clickTime <= s->doubleInterval) {
        if (distance(desired, s->lastClick, 1) <= magneticDoubleRadius) {
            s->cursor = s->lastClick;
            return output_ex(s, HC_POINTER, pLevel, quality, 1);
        }
        s->hasClick = 0;
    }
    s->cursor = smooth(s, desired, dt);
    return output_ex(s, HC_POINTER, pLevel, quality, 0);
}

HCOutput hc_frame(HCState *s, const HCPoint p[21], double aspect, double now) {
    return hc_frame_weighted(s, p, NULL, NULL, aspect, now);
}
