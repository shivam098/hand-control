#ifndef HAND_CONTROL_GESTURE_CORE_H
#define HAND_CONTROL_GESTURE_CORE_H
#ifdef __cplusplus
extern "C" {
#endif

typedef struct { double x, y; } HCPoint;
typedef struct { double x, y, z; } HCPoint3D;
typedef struct { double left, top, right, bottom; } HCRange;

typedef enum {
    HC_SEARCH, HC_MOVE, HC_PINCH, HC_RIGHT_PINCH, HC_DOUBLE_PINCH, HC_DRAG, HC_SCROLL, HC_PAUSE,
    HC_CAL_FIRST, HC_CAL_SECOND, HC_CAL_DONE
} HCMode;
typedef enum {
    HC_NO_EVENT, HC_POINTER, HC_CLICK, HC_RIGHT_CLICK, HC_DOWN, HC_DRAGGED, HC_UP, HC_WHEEL
} HCAction;
typedef struct {
    HCMode mode;
    HCAction action;
    HCPoint position;
    int clickCount, wheel;
    double progress;
    double pinchLevel, quality;
    int locked, synthesized;
} HCOutput;
typedef struct {
    HCRange range;
    HCMode mode;
    HCPoint cursor, anchor, lastClick, firstCorner, dwellAt;
    HCPoint tipOffsets[2], quietDirection, dragOrigin;
    HCPoint filteredJoints[21], stableFingerOffset;
    double jointConf[21];
    double doubleInterval, lastTime, lastValidTime, pinchTime, clickTime, scrollY, scrollRest, dwellTime;
    double quietSince, aimUntil, brakeStart, minApproachGap, prevGap, releaseSince;
    int hasCursor, needOpen, hasClick, calStage, dwelling;
    int hasTipHistory, hasJointHistory, hasFingerOffset, hasPrevGap;
    int approachArmed, pinchConfirmed, pinchFrames, pendingRelease;
} HCState;

HCRange hc_default_range(void);
int hc_valid_range(HCRange range);
void hc_init(HCState *s, HCRange range, double doubleClickSeconds);
HCOutput hc_begin_calibration(HCState *s);
HCOutput hc_lost(HCState *s);
HCOutput hc_coast(HCState *s, double seconds);
HCOutput hc_stop(HCState *s);
/* 21 landmarks in MediaPipe order, normalized in mirrored, top-left preview space. */
HCOutput hc_frame(HCState *s, const HCPoint points[21], double widthOverHeight, double seconds);
HCOutput hc_frame_weighted(HCState *s, const HCPoint points[21], const double confidence[21],
                           const HCPoint3D *worldPoints, double widthOverHeight, double seconds);

#ifdef __cplusplus
}
#endif
#endif
