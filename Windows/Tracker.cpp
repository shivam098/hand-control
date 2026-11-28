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
    options.min_hand_detection_confidence = 0.35f;
    options.min_hand_presence_confidence = 0.35f;
    options.min_tracking_confidence = 0.32f;
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

bool Tracker::detect(const uint8_t *rgb, int width, int height, int64_t milliseconds,
                     std::array<HCPoint,21> &points, std::array<double,21> &confidence,
                     std::array<HCPoint3D,21> &worldPoints, bool &hasWorld) {
    MpImagePtr raw = nullptr;
    char *message = nullptr;
    auto status = MpImageCreateFromUint8Data(kMpImageFormatSrgb, width, height, rgb, width * height * 3, &raw, &message);
    check(status, message, "Reading video image");
    std::unique_ptr<MpImageInternal, decltype(&MpImageFree)> image(raw, MpImageFree);
    HandLandmarkerResult result{};
    status = MpHandLandmarkerDetectForVideo(handle, image.get(), nullptr, milliseconds, &result, &message);
    check(status, message, "Tracking hand");
    bool found = result.hand_landmarks_count > 0 && result.hand_landmarks[0].landmarks_count == points.size();
    hasWorld = false;
    if (found) {
        double baseScore = 0.85;
        if (result.handedness_count > 0 && result.handedness[0].categories_count > 0) {
            float s = result.handedness[0].categories[0].score;
            if (s > 0.05f && s <= 1.0f) baseScore = s;
        }
        for (size_t i = 0; i < points.size(); ++i) {
            const auto &p = result.hand_landmarks[0].landmarks[i];
            double mx = 1.0 - p.x, my = p.y;
            points[i] = {mx, my};
            double c = baseScore;
            if (p.has_presence) c = std::min(c, static_cast<double>(p.presence));
            if (p.has_visibility) c = std::min(c, static_cast<double>(p.visibility));
            // De-weight MediaPipe border-extrapolated joints so wrist-optional knuckle geometry
            // takes over cleanly when the wrist or palm base sits below the bottom frame edge.
            if (mx < 0.0 || mx > 1.0 || my < 0.0 || my > 1.0) c = 0.0;
            else if (mx < 0.02 || mx > 0.98 || my < 0.02 || my > 0.96) c = std::min(c, 0.16);
            confidence[i] = std::max(0.0, std::min(1.0, c));
        }
        if (result.hand_world_landmarks_count > 0 &&
            result.hand_world_landmarks[0].landmarks_count == worldPoints.size()) {
            hasWorld = true;
            for (size_t i = 0; i < worldPoints.size(); ++i) {
                const auto &w = result.hand_world_landmarks[0].landmarks[i];
                worldPoints[i] = {-static_cast<double>(w.x), static_cast<double>(w.y), static_cast<double>(w.z)};
            }
        }
    }
    MpHandLandmarkerCloseResult(&result);
    return found;
}
