#pragma once
#include "GestureCore.h"
#include "mediapipe/tasks/c/vision/hand_landmarker/hand_landmarker.h"
#include <array>
#include <filesystem>
#include <vector>

class Tracker {
public:
    explicit Tracker(const std::filesystem::path &model);
    ~Tracker();
    Tracker(const Tracker &) = delete;
    Tracker &operator=(const Tracker &) = delete;
    bool detect(const uint8_t *rgb, int width, int height, int64_t milliseconds,
                std::array<HCPoint,21> &points, std::array<double,21> &confidence,
                std::array<HCPoint3D,21> &worldPoints, bool &hasWorld);
private:
    std::vector<char> modelBytes;
    MpHandLandmarkerPtr handle{};
};
