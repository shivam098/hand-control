#pragma once
#include "GestureCore.h"
#include <windows.h>
#include <stdexcept>

class DesktopMouse {
    bool held = false;
    int wheelRest = 0;
    static INPUT input(DWORD flags) {
        INPUT e{};
        e.type = INPUT_MOUSE;
        e.mi.dwFlags = flags;
        return e;
    }
public:
    bool release() {
        wheelRest = 0;
        if (!held) return true;
        auto e = input(MOUSEEVENTF_LEFTUP);
        if (SendInput(1, &e, sizeof(e)) != 1) return false;
        held = false;
        return true;
    }
    void apply(HCOutput out) {
        if (out.mode != HC_SCROLL) wheelRest = 0;
        INPUT events[3]{};
        UINT n = 0;
        auto move = input(MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE);
        move.mi.dx = static_cast<LONG>(out.position.x * 65535 + .5);
        move.mi.dy = static_cast<LONG>(out.position.y * 65535 + .5);
        switch (out.action) {
        case HC_POINTER: case HC_DRAGGED: events[n++] = move; break;
        case HC_CLICK: case HC_RIGHT_CLICK:
            events[n++] = move;
            events[n++] = input(out.action == HC_CLICK ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_RIGHTDOWN);
            events[n++] = input(out.action == HC_CLICK ? MOUSEEVENTF_LEFTUP : MOUSEEVENTF_RIGHTUP);
            break;
        case HC_DOWN: events[n++] = move; events[n++] = input(MOUSEEVENTF_LEFTDOWN); break;
        case HC_UP:
            if (!release()) throw std::runtime_error("Windows blocked the mouse release.");
            return;
        case HC_WHEEL: {
            wheelRest += out.wheel;
            int steps = wheelRest / 48;
            if (!steps) return;
            wheelRest -= steps * 48;
            events[n] = input(MOUSEEVENTF_WHEEL);
            events[n++].mi.mouseData = static_cast<DWORD>(steps * WHEEL_DELTA);
            break;
        }
        default: return;
        }
        UINT sent = SendInput(n, events, sizeof(INPUT));
        if (sent != n) {
            if ((out.action == HC_CLICK || out.action == HC_RIGHT_CLICK) && sent == 2) {
                auto up = events[2];
                SendInput(1, &up, sizeof(up));
            }
            throw std::runtime_error("Windows blocked pointer input. Switch away from elevated or protected windows and start again.");
        }
        if (out.action == HC_DOWN) held = true;
    }
};
