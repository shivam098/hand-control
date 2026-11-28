#pragma once
#include "GestureCore.h"
#include <windows.h>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

inline double secondsNow() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct CameraDeviceInfo {
    std::wstring id;
    std::wstring name;
};

struct CameraFrame {
    unsigned width{}, height{};
    std::vector<uint32_t> mirroredBGRA;
    std::array<HCPoint,21> points{};
    std::array<double,21> confidence{};
    std::array<HCPoint3D,21> worldPoints{};
    double time{};
    bool found{};
    bool hasWorld{};
};

class Camera {
public:
    static std::vector<CameraDeviceInfo> listDevices();
    explicit Camera(std::filesystem::path model, std::wstring preferredDeviceId = {});
    ~Camera();
    void stop();
    bool finished() const { return done.load(); }
    std::shared_ptr<CameraFrame> take(std::string &error);
private:
    void capture(const std::filesystem::path &model, const std::wstring &preferredDeviceId);
    HANDLE stopEvent{};
    std::atomic<bool> done{false};
    std::mutex guard;
    std::shared_ptr<CameraFrame> latest;
    std::string failure;
    std::thread worker;
};
