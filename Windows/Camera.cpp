#include "Camera.hpp"
#include "Tracker.hpp"
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <utility>

using Microsoft::WRL::ComPtr;
constexpr DWORD stream = MF_SOURCE_READER_FIRST_VIDEO_STREAM;

static void checked(HRESULT hr, const char *action) {
    if (SUCCEEDED(hr)) return;
    char code[16]{};
    std::snprintf(code, sizeof(code), "0x%08lX", static_cast<unsigned long>(hr));
    throw std::runtime_error(std::string(action) + " (" + code + ")");
}

struct Packet {
    HRESULT hr{};
    DWORD flags{};
    ComPtr<IMFSample> sample;
    double time{};
};

class Callback final : public IMFSourceReaderCallback {
    std::atomic<ULONG> references{1};
    std::mutex guard;
    Packet packet;
public:
    HANDLE ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    Callback() { if (!ready) throw std::runtime_error("Could not create camera notification."); }
    ~Callback() { CloseHandle(ready); }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
    ULONG STDMETHODCALLTYPE Release() override {
        auto remaining = --references;
        if (!remaining) delete this;
        return remaining;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **out) override {
        if (!out) return E_POINTER;
        if (id == IID_IUnknown || id == __uuidof(IMFSourceReaderCallback)) {
            *out = static_cast<IMFSourceReaderCallback *>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE OnReadSample(HRESULT hr, DWORD, DWORD flags, LONGLONG, IMFSample *sample) override {
        std::lock_guard<std::mutex> lock(guard);
        packet.hr = hr;
        packet.flags = flags;
        packet.sample = sample;
        packet.time = secondsNow();
        SetEvent(ready);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnEvent(DWORD, IMFMediaEvent *) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnFlush(DWORD) override { return S_OK; }
    Packet take() {
        std::lock_guard<std::mutex> lock(guard);
        return std::exchange(packet, Packet{});
    }
};

struct Devices {
    IMFActivate **items{};
    UINT32 count{};
    ~Devices() {
        for (UINT32 i = 0; i < count; ++i) items[i]->Release();
        CoTaskMemFree(items);
    }
};
struct Source {
    ComPtr<IMFMediaSource> media;
    ~Source() { if (media) media->Shutdown(); }
};

static std::wstring getAttrString(IMFActivate *item, const GUID &guid) {
    WCHAR *raw = nullptr;
    UINT32 len = 0;
    if (FAILED(item->GetAllocatedString(guid, &raw, &len)) || !raw) return {};
    std::wstring value(raw, len);
    CoTaskMemFree(raw);
    return value;
}

std::vector<CameraDeviceInfo> Camera::listDevices() {
    std::vector<CameraDeviceInfo> list;
    auto com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool uninit = SUCCEEDED(com);
    if (SUCCEEDED(MFStartup(MF_VERSION))) {
        ComPtr<IMFAttributes> attrs;
        if (SUCCEEDED(MFCreateAttributes(&attrs, 1)) &&
            SUCCEEDED(attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID))) {
            Devices devices;
            if (SUCCEEDED(MFEnumDeviceSources(attrs.Get(), &devices.items, &devices.count))) {
                for (UINT32 i = 0; i < devices.count; ++i) {
                    auto name = getAttrString(devices.items[i], MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME);
                    auto id = getAttrString(devices.items[i], MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK);
                    if (name.empty()) name = L"Camera " + std::to_wstring(i + 1);
                    list.push_back({std::move(id), std::move(name)});
                }
            }
        }
        MFShutdown();
    }
    if (uninit) CoUninitialize();
    return list;
}

static void selectVideoFormat(IMFSourceReader *reader) {
    ComPtr<IMFMediaType> best;
    double bestScore = 1e30;
    for (DWORD i = 0; ; ++i) {
        ComPtr<IMFMediaType> type;
        auto hr = reader->GetNativeMediaType(stream, i, &type);
        if (hr == MF_E_NO_MORE_TYPES) break;
        checked(hr, "Listing webcam formats");
        UINT32 w{}, h{}, n{}, d{};
        if (FAILED(MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &w, &h)) || !w || !h) continue;
        if (w > 1920 || h > 1440) continue;
        double fps = SUCCEEDED(MFGetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, &n, &d)) && d ? double(n) / d : 30;
        double resScore = std::abs(std::log(double(w) / 1280.0)) + std::abs(std::log(double(h) / 720.0));
        double aspectPenalty = std::abs((double(w) / h) - (16.0 / 9.0)) * 0.30;
        double fpsBonus = (fps >= 58 && fps <= 62) ? -0.18 : (std::abs(fps - 30.0) * .008 + (fps < 15 ? 3 : 0));
        double score = resScore + aspectPenalty + fpsBonus;
        if (score < bestScore) { bestScore = score; best = type; }
    }
    if (!best) throw std::runtime_error("The webcam has no usable video formats.");
    checked(reader->SetCurrentMediaType(stream, nullptr, best.Get()), "Selecting camera resolution");
    ComPtr<IMFMediaType> rgb;
    checked(MFCreateMediaType(&rgb), "Creating video format");
    checked(rgb->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), "Selecting video");
    checked(rgb->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32), "Selecting RGB");
    checked(reader->SetCurrentMediaType(stream, nullptr, rgb.Get()), "Requesting RGB video");
}

struct Format { UINT32 width, height; LONG stride; };
static Format getFormat(IMFSourceReader *reader) {
    ComPtr<IMFMediaType> type;
    checked(reader->GetCurrentMediaType(stream, &type), "Reading camera format");
    GUID subtype{};
    checked(type->GetGUID(MF_MT_SUBTYPE, &subtype), "Reading pixel format");
    if (subtype != MFVideoFormat_RGB32) throw std::runtime_error("Camera did not provide RGB32 frames.");
    Format f{};
    checked(MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &f.width, &f.height), "Reading camera dimensions");
    if (!f.width || !f.height || uint64_t(f.width) * f.height > 20000000) throw std::runtime_error("Unsupported camera image size.");
    UINT32 stride{};
    if (SUCCEEDED(type->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride))) f.stride = static_cast<LONG>(stride);
    else checked(MFGetStrideForBitmapInfoHeader(subtype.Data1, f.width, &f.stride), "Reading camera stride");
    return f;
}

struct LockedPixels {
    ComPtr<IMF2DBuffer> two;
    ComPtr<IMFMediaBuffer> one;
    BYTE *top{};
    LONG stride{};
    bool locked{};
    ~LockedPixels() { if (locked) { if (two) two->Unlock2D(); else one->Unlock(); } }
    void load(IMFSample *sample, Format f) {
        checked(sample->ConvertToContiguousBuffer(&one), "Reading camera buffer");
        if (SUCCEEDED(one.As(&two))) {
            checked(two->Lock2D(&top, &stride), "Locking video image");
            locked = true;
        } else {
            DWORD length{};
            checked(one->Lock(&top, nullptr, &length), "Locking video data");
            locked = true;
            stride = f.stride;
            auto span = std::abs(int64_t(stride)) * (f.height - 1);
            if (uint64_t(span) + uint64_t(f.width)*4 > length) throw std::runtime_error("Incomplete camera frame.");
            if (stride < 0) top += span;
        }
        if (!top || std::abs(int64_t(stride)) < int64_t(f.width)*4) throw std::runtime_error("Invalid camera row layout.");
    }
};

Camera::Camera(std::filesystem::path model, std::wstring preferredDeviceId)
    : stopEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {
    if (!stopEvent) throw std::runtime_error("Could not initialize camera control.");
    worker = std::thread([this, model = std::move(model), preferredDeviceId = std::move(preferredDeviceId)] {
        auto com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        bool mf = false;
        try {
            checked(com, "Initializing Windows camera services");
            checked(MFStartup(MF_VERSION), "Starting Media Foundation");
            mf = true;
            capture(model, preferredDeviceId);
        } catch (const std::exception &e) {
            std::lock_guard<std::mutex> lock(guard);
            failure = e.what();
        }
        if (mf) MFShutdown();
        if (SUCCEEDED(com)) CoUninitialize();
        done = true;
    });
}

Camera::~Camera() { stop(); worker.join(); CloseHandle(stopEvent); }
void Camera::stop() { SetEvent(stopEvent); }
std::shared_ptr<CameraFrame> Camera::take(std::string &error) {
    std::lock_guard<std::mutex> lock(guard);
    error = std::exchange(failure, {});
    return std::exchange(latest, {});
}

void Camera::capture(const std::filesystem::path &model, const std::wstring &preferredDeviceId) {
    Tracker tracker(model);
    if (WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0) return;
    ComPtr<IMFAttributes> attrs;
    checked(MFCreateAttributes(&attrs, 1), "Preparing camera discovery");
    checked(attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID), "Selecting video cameras");
    Devices devices;
    checked(MFEnumDeviceSources(attrs.Get(), &devices.items, &devices.count), "Finding cameras");
    if (!devices.count) throw std::runtime_error("No webcam found. Connect a camera and allow desktop apps in Camera settings.");
    UINT32 chosen = 0;
    if (!preferredDeviceId.empty()) {
        for (UINT32 i = 0; i < devices.count; ++i) {
            if (getAttrString(devices.items[i], MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK) == preferredDeviceId) {
                chosen = i;
                break;
            }
        }
    }
    Source source;
    checked(devices.items[chosen]->ActivateObject(IID_PPV_ARGS(&source.media)), "Opening webcam; check Camera settings and close other camera apps");
    ComPtr<Callback> callback;
    callback.Attach(new Callback());
    ComPtr<IMFAttributes> readerAttrs;
    checked(MFCreateAttributes(&readerAttrs, 2), "Preparing video reader");
    checked(readerAttrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE), "Enabling video conversion");
    checked(readerAttrs->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK, callback.Get()), "Enabling camera notifications");
    ComPtr<IMFSourceReader> reader;
    checked(MFCreateSourceReaderFromMediaSource(source.media.Get(), readerAttrs.Get(), &reader), "Starting webcam");
    selectVideoFormat(reader.Get());
    Format f = getFormat(reader.Get());
    HANDLE waits[] = {stopEvent, callback->ready};
    int64_t lastStamp = -1;
    std::vector<uint8_t> rgb;
    while (WaitForSingleObject(stopEvent, 0) != WAIT_OBJECT_0) {
        checked(reader->ReadSample(stream, 0, nullptr, nullptr, nullptr, nullptr), "Requesting camera frame");
        auto wait = WaitForMultipleObjects(2, waits, FALSE, 5000);
        if (wait == WAIT_OBJECT_0) break;
        if (wait != WAIT_OBJECT_0 + 1) throw std::runtime_error("Camera stopped responding. Check Camera settings and start again.");
        auto packet = callback->take();
        checked(packet.hr, "Webcam disconnected or access was denied");
        if (packet.flags & (MF_SOURCE_READERF_ENDOFSTREAM | MF_SOURCE_READERF_ERROR)) throw std::runtime_error("The webcam stream ended.");
        if (packet.flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) f = getFormat(reader.Get());
        if (!packet.sample) continue;
        auto frame = std::make_shared<CameraFrame>();
        frame->width = f.width;
        frame->height = f.height;
        frame->time = packet.time;
        frame->mirroredBGRA.resize(size_t(f.width) * f.height);
        unsigned step = (f.width >= 1280 && f.height >= 720) ? 2u : 1u;
        unsigned tw = f.width / step, th = f.height / step;
        rgb.resize(size_t(tw) * th * 3);
        {
            LockedPixels video;
            video.load(packet.sample.Get(), f);
            for (unsigned y = 0; y < f.height; ++y) {
                const BYTE *row = video.top + int64_t(y) * video.stride;
                for (unsigned x = 0; x < f.width; ++x) {
                    const BYTE *b = row + x * 4;
                    frame->mirroredBGRA[size_t(y + 1) * f.width - 1 - x] =
                        uint32_t(b[0]) | uint32_t(b[1]) << 8 | uint32_t(b[2]) << 16;
                }
            }
            for (unsigned ty = 0; ty < th; ++ty) {
                const BYTE *row = video.top + int64_t(ty * step) * video.stride;
                for (unsigned tx = 0; tx < tw; ++tx) {
                    const BYTE *b = row + (tx * step) * 4;
                    size_t ti = (size_t(ty) * tw + tx) * 3;
                    rgb[ti] = b[2]; rgb[ti + 1] = b[1]; rgb[ti + 2] = b[0];
                }
            }
        }
        lastStamp = std::max(lastStamp + 1, static_cast<int64_t>(frame->time * 1000));
        frame->found = tracker.detect(rgb.data(), static_cast<int>(tw), static_cast<int>(th),
                                      lastStamp, frame->points, frame->confidence,
                                      frame->worldPoints, frame->hasWorld);
        std::lock_guard<std::mutex> lock(guard);
        latest = std::move(frame);
    }
}
