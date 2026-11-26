#include "Camera.hpp"
#include "DesktopMouse.hpp"
#include "resource.h"
#include <dwmapi.h>
#include <shellapi.h>
#include <algorithm>
#include <cmath>
#include <iterator>
#include <stdexcept>

static std::wstring wide(const std::string &text) {
    auto n = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), n);
    return result;
}

class HandWindow {
    HWND window{}, start{}, calibrate{}, settings{};
    HFONT heading{}, body{}, emphasis{};
    int dpi = 96;
    bool glassShelf{}, highContrast{};
    bool active{}, saved{}, received{};
    double lastFrame{};
    HCState core{};
    DesktopMouse mouse;
    std::unique_ptr<Camera> camera;
    std::shared_ptr<CameraFrame> preview;
    std::filesystem::path model;
    std::wstring status = L"Ready. Start the camera to see your hand.";
    static constexpr wchar_t key[] = L"Software\\HandControl";
    enum { Start = 101, Calibrate, Settings };
    static constexpr int shelfHeight = 60;

    int px(int logical) const { return MulDiv(logical, dpi, 96); }
    HFONT font(int size, int weight) const {
        return CreateFontW(-px(size), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    }
    HWND button(const wchar_t *text, int id) {
        auto handle = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                      0, 0, 0, 0, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                      GetModuleHandleW(nullptr), nullptr);
        if (!handle) throw std::runtime_error("Could not create window controls.");
        SendMessageW(handle, WM_SETFONT, reinterpret_cast<WPARAM>(body), TRUE);
        return handle;
    }
    void updateMaterial() {
        HIGHCONTRASTW contrast{sizeof(HIGHCONTRASTW)};
        highContrast = !SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0)
                    || (contrast.dwFlags & HCF_HIGHCONTRASTON);
        BOOL reduceEffects = FALSE;
        bool useBackdrop = !highContrast
            && SystemParametersInfoW(SPI_GETDISABLEOVERLAPPEDCONTENT, 0, &reduceEffects, 0) && !reduceEffects;
        auto backdrop = useBackdrop ? DWMSBT_MAINWINDOW : DWMSBT_NONE;
        glassShelf = SUCCEEDED(DwmSetWindowAttribute(window, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop)))
                  && useBackdrop;
        MARGINS margins{0, 0, 0, glassShelf ? px(shelfHeight) : 0};
        if (FAILED(DwmExtendFrameIntoClientArea(window, &margins))) glassShelf = false;
        InvalidateRect(window, nullptr, FALSE);
    }
    void created() {
        HDC dc = GetDC(window);
        dpi = GetDeviceCaps(dc, LOGPIXELSX);
        ReleaseDC(window, dc);
        updateMaterial();
        heading = font(28, FW_SEMIBOLD); body = font(15, FW_NORMAL); emphasis = font(16, FW_SEMIBOLD);
        start = button(L"Start Camera", Start);
        calibrate = button(L"Calibrate Reach", Calibrate);
        settings = button(L"Camera Settings", Settings);
        EnableWindow(calibrate, FALSE);
        HCRange range{};
        DWORD bytes = sizeof(range);
        auto result = RegGetValueW(HKEY_CURRENT_USER, key, L"ReachV1", RRF_RT_REG_BINARY, nullptr, &range, &bytes);
        saved = result == ERROR_SUCCESS && bytes == sizeof(range) && hc_valid_range(range);
        if (!saved && result != ERROR_FILE_NOT_FOUND)
            status = L"Saved reach could not be read. Using default reach; calibrate to replace it.";
        hc_init(&core, saved ? range : hc_default_range(), GetDoubleClickTime() / 1000.0);
        if (!SetTimer(window, 1, 16, nullptr)) throw std::runtime_error("Could not start the desktop update timer.");
    }
    void layout() {
        RECT r{};
        GetClientRect(window, &r);
        int y = r.bottom - (px(shelfHeight) + px(32)) / 2;
        MoveWindow(start, px(20), y, px(132), px(32), TRUE);
        MoveWindow(calibrate, px(162), y, px(158), px(32), TRUE);
        MoveWindow(settings, px(330), y, px(150), px(32), TRUE);
    }
    void stop(std::wstring text) {
        active = false;
        hc_stop(&core);
        if (!mouse.release()) text += L" Windows blocked mouse release; click the physical mouse once.";
        if (camera) camera->stop();
        preview.reset();
        status = std::move(text);
        SetWindowTextW(start, L"Start Camera");
        EnableWindow(start, !camera);
        SetWindowTextW(calibrate, L"Calibrate Reach");
        EnableWindow(calibrate, FALSE);
        InvalidateRect(window, nullptr, FALSE);
    }
    void command(int id) {
        if (id == Start) {
            if (active) { stop(L"Camera off. Desktop control is stopped."); return; }
            if (camera) return;
            camera = std::make_unique<Camera>(model);
            received = false;
            active = true;
            lastFrame = secondsNow();
            status = L"Starting camera and hand tracker...";
            SetWindowTextW(start, L"Stop Camera");
        } else if (id == Calibrate && active) {
            if (core.calStage) {
                hc_stop(&core);
                status = L"Calibration cancelled. Previous reach retained.";
                SetWindowTextW(calibrate, L"Calibrate Reach");
            } else {
                show(hc_begin_calibration(&core));
                SetWindowTextW(calibrate, L"Cancel Calibration");
            }
        } else if (id == Settings) {
            auto result = ShellExecuteW(window, L"open", L"ms-settings:privacy-webcam", nullptr, nullptr, SW_SHOWNORMAL);
            if (reinterpret_cast<INT_PTR>(result) <= 32)
                status = L"Open Settings > Privacy & security > Camera and allow desktop apps.";
        }
        InvalidateRect(window, nullptr, FALSE);
    }
    void show(HCOutput out) {
        mouse.apply(out);
        switch (out.mode) {
        case HC_MOVE: status = L"Move"; break;
        case HC_PINCH: status = L"Pinch: release to click or hold to drag"; break;
        case HC_RIGHT_PINCH: status = L"Middle pinch: release to right-click"; break;
        case HC_DRAG: status = L"Dragging: move your hand, then open the pinch to drop"; break;
        case HC_SCROLL: status = L"Scroll"; break;
        case HC_PAUSE: status = L"Paused: open your hand and separate your fingertips"; break;
        case HC_CAL_FIRST: status = L"1 of 2: point at your comfortable UPPER LEFT and hold still"; break;
        case HC_CAL_SECOND: status = L"2 of 2: point farther DOWN and RIGHT and hold still"; break;
        case HC_CAL_DONE: {
            auto err = RegSetKeyValueW(HKEY_CURRENT_USER, key, L"ReachV1", REG_BINARY, &core.range, sizeof(core.range));
            saved = err == ERROR_SUCCESS;
            SetWindowTextW(calibrate, L"Calibrate Reach");
            if (!saved) throw std::runtime_error("Could not save your calibration in Windows settings.");
            status = L"Calibration saved";
            break;
        }
        default: status = L"Looking for a hand: show your palm and fingertips"; break;
        }
        if (core.calStage) status += L"   " + std::to_wstring(int(out.progress * 100)) + L"%";
        else {
            if (out.action == HC_CLICK) status = out.clickCount == 2 ? L"Double-click" : L"Click";
            if (out.action == HC_RIGHT_CLICK) status = L"Right-click";
            status += saved ? L"   |   Calibrated" : L"   |   Default reach";
        }
    }
    void tick() {
        if (!camera) return;
        bool done = camera->finished();
        std::string error;
        auto frame = camera->take(error);
        if (active && !error.empty()) stop(wide(error));
        if (active) {
            double now = secondsNow();
            if (frame && now - frame->time < .4) {
                received = true;
                lastFrame = frame->time;
                preview = std::move(frame);
                EnableWindow(calibrate, TRUE);
                show(preview->found ? hc_frame(&core, preview->points.data(), double(preview->width) / preview->height, lastFrame)
                                    : hc_lost(&core));
                if (!IsIconic(window)) InvalidateRect(window, nullptr, FALSE);
            } else if (now - lastFrame > .45 && received) {
                mouse.apply(hc_lost(&core));
                received = false;
                preview.reset();
                status = L"Waiting for fresh camera frames. Desktop control is paused.";
                InvalidateRect(window, nullptr, FALSE);
            }
        }
        if (done) {
            if (active) stop(L"The camera stopped. Start again to reconnect.");
            camera.reset();
            EnableWindow(start, TRUE);
        }
    }
    static void fill(HDC dc, RECT rect, COLORREF color) {
        SetDCBrushColor(dc, color);
        FillRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    }
    void label(HDC dc, RECT rect, const wchar_t *text, HFONT f, COLORREF color, UINT flags) {
        SelectObject(dc, f); SetTextColor(dc, color);
        DrawTextW(dc, text, -1, &rect, flags | DT_NOPREFIX);
    }
    void drawFrame(HDC dc, RECT area) {
        fill(dc, area, !preview && highContrast ? GetSysColor(COLOR_WINDOW) : RGB(13,20,30));
        if (!preview) {
            int size = px(48), top = (area.top + area.bottom - size - px(30)) / 2;
            auto icon = reinterpret_cast<HICON>(GetClassLongPtrW(window, GCLP_HICON));
            DrawIconEx(dc, (area.left + area.right - size) / 2, top, icon, size, size, 0, nullptr, DI_NORMAL);
            area.top = top + size + px(8);
            label(dc, area, L"Camera preview", emphasis, highContrast ? GetSysColor(COLOR_WINDOWTEXT) : RGB(165,180,197),
                  DT_CENTER | DT_SINGLELINE);
            return;
        }
        const auto &f = *preview;
        double scale = std::min(double(area.right - area.left) / f.width, double(area.bottom - area.top) / f.height);
        int w = int(f.width * scale), h = int(f.height * scale);
        int x = (area.left + area.right - w)/2, y = (area.top + area.bottom - h)/2;
        BITMAPINFO bmp{};
        auto &hdr = bmp.bmiHeader;
        hdr.biSize = sizeof(hdr); hdr.biWidth = f.width; hdr.biHeight = -static_cast<LONG>(f.height);
        hdr.biPlanes = 1; hdr.biBitCount = 32; hdr.biCompression = BI_RGB;
        SetStretchBltMode(dc, COLORONCOLOR);
        StretchDIBits(dc, x, y, w, h, 0, 0, f.width, f.height, f.mirroredBGRA.data(), &bmp, DIB_RGB_COLORS, SRCCOPY);
        auto at = [=](HCPoint p) -> POINT { return {x + LONG(p.x*w), y + LONG(p.y*h)}; };
        auto dot = [&](HCPoint p, int radius, COLORREF color) {
            auto c = at(p);
            SetDCBrushColor(dc, color);
            Ellipse(dc, c.x-radius, c.y-radius, c.x+radius+1, c.y+radius+1);
        };
        int savedDC = SaveDC(dc);
        IntersectClipRect(dc, x, y, x+w, y+h);
        SelectObject(dc, GetStockObject(NULL_BRUSH));
        auto dashed = CreatePen(PS_DASH, 1, RGB(235,240,245));
        SelectObject(dc, dashed);
        auto a = at({core.range.left,core.range.top}), b = at({core.range.right,core.range.bottom});
        Rectangle(dc, a.x, a.y, b.x, b.y);
        auto pen = CreatePen(PS_SOLID, px(2), RGB(72,235,184));
        SelectObject(dc, pen);
        if (f.found) {
            static const int edges[][2] = {{0,1},{1,2},{2,3},{3,4},{0,5},{5,6},{6,7},{7,8},
                {5,9},{9,10},{10,11},{11,12},{9,13},{13,14},{14,15},{15,16},{13,17},{17,18},{18,19},{19,20},{17,0}};
            for (auto &edge : edges) {
                auto p = at(f.points[edge[0]]), q = at(f.points[edge[1]]);
                MoveToEx(dc, p.x, p.y, nullptr); LineTo(dc, q.x, q.y);
            }
        }
        SelectObject(dc, GetStockObject(NULL_PEN)); SelectObject(dc, GetStockObject(DC_BRUSH));
        if (f.found) for (int i=0; i<21; ++i) {
            bool tip = i==4 || i==8 || i==12;
            dot(f.points[i], px(tip ? 5 : 3), tip ? RGB(255,215,73) : RGB(250,250,250));
        }
        if (core.calStage == 2) dot(core.firstCorner, px(6), RGB(255,215,73));
        RestoreDC(dc, savedDC);
        DeleteObject(dashed); DeleteObject(pen);
    }
    void paint() {
        PAINTSTRUCT ps{};
        HDC screen = BeginPaint(window, &ps);
        RECT r{}; GetClientRect(window, &r);
        HDC dc = CreateCompatibleDC(screen);
        HBITMAP image = CreateCompatibleBitmap(screen, r.right, r.bottom);
        auto previous = SelectObject(dc, image);
        SetBkMode(dc, TRANSPARENT);
        auto ink = GetSysColor(COLOR_WINDOWTEXT);
        auto secondary = highContrast ? ink : RGB(69,85,101);
        fill(dc, r, highContrast ? GetSysColor(COLOR_WINDOW) : RGB(245,247,250));
        RECT shelf{0, r.bottom - px(shelfHeight), r.right, r.bottom};
        // DWM composites the system material through black GDI pixels in the extended frame.
        fill(dc, shelf, glassShelf ? RGB(0,0,0) : GetSysColor(COLOR_BTNFACE));
        RECT rule{shelf.left, shelf.top - 1, shelf.right, shelf.top};
        fill(dc, rule, GetSysColor(COLOR_3DSHADOW));
        auto rect = [&](int top, int bottom) -> RECT { return {px(20), top, r.right - px(20), bottom}; };
        label(dc, rect(px(12),px(49)), L"Hand Control", heading, highContrast ? ink : RGB(25,39,53), DT_SINGLELINE);
        label(dc, rect(px(51),px(78)), L"Control your desktop with one hand. Video stays on this PC.", body, secondary, DT_SINGLELINE);
        drawFrame(dc, rect(px(80),r.bottom-px(190)));
        label(dc, rect(r.bottom-px(180),r.bottom-px(136)), status.c_str(), emphasis, highContrast ? ink : RGB(24,62,70), DT_WORDBREAK);
        label(dc, rect(r.bottom-px(134),shelf.top-px(5)),
              L"Point to move. Pinch thumb + index to click; pinch twice to double-click, or hold to drag. "
              L"Pinch thumb + middle to right-click. Raise only index + middle and move vertically to scroll. "
              L"Make a fist to pause; open your hand to resume.",
              body, secondary, DT_WORDBREAK);
        BitBlt(screen, 0, 0, r.right, r.bottom, dc, 0, 0, SRCCOPY);
        SelectObject(dc, previous); DeleteObject(image); DeleteDC(dc);
        EndPaint(window, &ps);
    }
public:
    explicit HandWindow(std::filesystem::path path) : model(std::move(path)) {}
    ~HandWindow() { DeleteObject(heading); DeleteObject(body); DeleteObject(emphasis); }
    static LRESULT CALLBACK proc(HWND wnd, UINT message, WPARAM w, LPARAM l) {
        auto *self = reinterpret_cast<HandWindow *>(GetWindowLongPtrW(wnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<HandWindow *>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams);
            self->window = wnd;
            SetWindowLongPtrW(wnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) return DefWindowProcW(wnd, message, w, l);
        try {
            switch (message) {
            case WM_CREATE: self->created(); return 0;
            case WM_COMMAND: self->command(LOWORD(w)); return 0;
            case WM_TIMER: self->tick(); return 0;
            case WM_SETTINGCHANGE: case WM_THEMECHANGED: case WM_SYSCOLORCHANGE: case WM_DWMCOMPOSITIONCHANGED:
                self->updateMaterial(); break;
            case WM_SIZE: self->layout(); InvalidateRect(wnd, nullptr, FALSE); return 0;
            case WM_GETMINMAXINFO: {
                auto *limits = reinterpret_cast<MINMAXINFO *>(l);
                limits->ptMinTrackSize = {self->px(690), self->px(540)};
                return 0;
            }
            case WM_ERASEBKGND: return 1;
            case WM_PAINT: self->paint(); return 0;
            case WM_DESTROY:
                KillTimer(wnd, 1);
                self->stop(L"Stopped");
                self->camera.reset();
                PostQuitMessage(0);
                return 0;
            }
        } catch (const std::exception &error) {
            if (message == WM_CREATE) { MessageBoxW(wnd, wide(error.what()).c_str(), L"Hand Control", MB_ICONERROR); return -1; }
            self->stop(wide(error.what()));
            return 0;
        }
        return DefWindowProcW(wnd, message, w, l);
    }
};

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    SetProcessDPIAware();
    wchar_t path[32768];
    DWORD length = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    if (!length || length >= std::size(path)) {
        MessageBoxW(nullptr, L"Could not locate the app folder.", L"Hand Control", MB_ICONERROR);
        return 1;
    }
    HandWindow app(std::filesystem::path(path).parent_path() / L"hand_landmarker.task");
    WNDCLASSEXW cls{};
    cls.cbSize = sizeof(cls);
    cls.lpfnWndProc = HandWindow::proc; cls.hInstance = instance; cls.lpszClassName = L"HandControl.Main";
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    auto icon = MAKEINTRESOURCEW(IDI_HAND_CONTROL);
    cls.hIcon = LoadIconW(instance, icon);
    cls.hIconSm = static_cast<HICON>(LoadImageW(instance, icon, IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                               GetSystemMetrics(SM_CYSMICON), LR_SHARED));
    if (!RegisterClassExW(&cls)) return 1;
    auto window = CreateWindowExW(0, cls.lpszClassName, L"Hand Control", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                  CW_USEDEFAULT, CW_USEDEFAULT, 860, 710, nullptr, nullptr, instance, &app);
    if (!window) return 1;
    ShowWindow(window, show);
    MSG message{};
    BOOL result;
    while ((result = GetMessageW(&message, nullptr, 0, 0)) > 0) {
        if (!IsDialogMessageW(window, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
    }
    return result == -1 ? 1 : static_cast<int>(message.wParam);
}
