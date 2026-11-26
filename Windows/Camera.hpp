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

struct CameraFrame {
    unsigned width{}, height{};
    std::vector<uint32_t> mirroredBGRA;
    std::array<HCPoint,21> points{};
    double time{};
    bool found{};
};

class Camera {
public:
    explicit Camera(std::filesystem::path model);
    ~Camera();
    void stop();
    bool finished() const { return done.load(); }
    std::shared_ptr<CameraFrame> take(std::string &error);
private:
    void capture(const std::filesystem::path &model);
    HANDLE stopEvent{};
    std::atomic<bool> done{false};
    std::mutex guard;
    std::shared_ptr<CameraFrame> latest;
    std::string failure;
    std::thread worker;
};
