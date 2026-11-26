#include "Tracker.hpp"
#include <cstdio>
#include <fstream>
#include <memory>
#include <stdexcept>

static void check(MpStatus status, char *message, const char *operation) {
    if (status == kMpOk) return;
    std::string detail = message ? message : "unknown tracker error";
    if (message) MpErrorFree(message);
    throw std::runtime_error(std::string(operation) + ": " + detail);
}

Tracker::Tracker(const std::filesystem::path &model) {
    std::ifstream file(model, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error("Missing hand_landmarker.task. Extract the entire app folder.");
    auto size = file.tellg();
    if (size <= 0 || size > 100 * 1024 * 1024) throw std::runtime_error("Invalid hand model file.");
    modelBytes.resize(static_cast<size_t>(size));
    file.seekg(0);
    if (!file.read(modelBytes.data(), size)) throw std::runtime_error("Could not read the hand model.");
    HandLandmarkerOptions options{};
    options.base_options.model_asset_buffer = modelBytes.data();
    options.base_options.model_asset_buffer_count = static_cast<unsigned int>(modelBytes.size());
    options.running_mode = VIDEO;
    options.num_hands = 1;
    char *message = nullptr;
    auto status = MpHandLandmarkerCreate(&options, &handle, &message);
    check(status, message, "Opening hand model");
}

Tracker::~Tracker() {
    if (!handle) return;
    char *message = nullptr;
    if (MpHandLandmarkerClose(handle, &message) != kMpOk) {
        std::fprintf(stderr, "Closing hand tracker: %s\n", message ? message : "unknown error");
        if (message) MpErrorFree(message);
    }
}

bool Tracker::detect(const uint8_t *rgb, int width, int height, int64_t milliseconds, std::array<HCPoint,21> &points) {
    MpImagePtr raw = nullptr;
    char *message = nullptr;
    auto status = MpImageCreateFromUint8Data(kMpImageFormatSrgb, width, height, rgb, width * height * 3, &raw, &message);
    check(status, message, "Reading video image");
    std::unique_ptr<MpImageInternal, decltype(&MpImageFree)> image(raw, MpImageFree);
    HandLandmarkerResult result{};
    status = MpHandLandmarkerDetectForVideo(handle, image.get(), nullptr, milliseconds, &result, &message);
    check(status, message, "Tracking hand");
    bool found = result.hand_landmarks_count > 0 && result.hand_landmarks[0].landmarks_count == points.size();
    if (found) {
        for (size_t i = 0; i < points.size(); ++i) {
            const auto &p = result.hand_landmarks[0].landmarks[i];
            points[i] = {1.0 - p.x, p.y};
        }
    }
    MpHandLandmarkerCloseResult(&result);
    return found;
}
