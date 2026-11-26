#include "Tracker.hpp"
#include <fstream>
#include <iostream>

// Reads packed RGB frames exported from an actual local video; never posts input.
int main(int argc, char **argv) {
    if (argc != 5) { std::cerr << "usage: replay model width height frames.rgb\n"; return 2; }
    try {
        int w = std::stoi(argv[2]), h = std::stoi(argv[3]);
        if (w <= 0 || h <= 0 || int64_t(w)*h > 20000000) return 2;
        std::ifstream in(argv[4], std::ios::binary);
        if (!in) return 2;
        Tracker tracker(argv[1]);
        std::vector<uint8_t> rgb(size_t(w)*h*3);
        std::array<HCPoint,21> points{};
        int frames = 0, detected = 0, usable = 0;
        HCState state{};
        hc_init(&state, hc_default_range(), .5);
        while (in.read(reinterpret_cast<char *>(rgb.data()), rgb.size())) {
            double t = ++frames / 10.0;
            if (tracker.detect(rgb.data(), w, h, frames*100, points)) {
                ++detected;
                if (hc_frame(&state, points.data(), double(w)/h, t).mode != HC_SEARCH) ++usable;
            } else hc_lost(&state);
        }
        std::cout << usable << " usable hand frames, " << detected << " detections, " << frames << " decoded frames\n";
        return usable ? 0 : 1;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
