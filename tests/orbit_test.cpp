#include "orbit.hpp"
#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>

int main() {
    try {
        lv_init();
        static std::array<std::uint16_t, 320 * 170> pixels{};
        auto *display = lv_display_create(320, 170);
        lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
        lv_display_set_buffers(display, pixels.data(), nullptr, sizeof(pixels),
                               LV_DISPLAY_RENDER_MODE_FULL);
        lv_display_set_flush_cb(display, [](lv_display_t *d, const lv_area_t *, uint8_t *) {
            lv_display_flush_ready(d);
        });
        auto *root = lv_display_get_screen_active(display);
        const auto frame = [&](unsigned ms) {
            lv_tick_inc(ms);
            lv_obj_invalidate(root);
            lv_timer_handler();
            lv_refr_now(display);
            return pixels;
        };
        const auto check = [](bool pass, const char *message) {
            if (!pass)
                throw std::runtime_error(message);
        };
        okou::RecordingWaveform mic;
        okou::PlaybackMeter playback;
        {
            okou::PocketOrbit art(root, mic, playback);
            art.show(okou::OrbitScene::welcome);
            auto welcome = frame(400);
            check(std::count_if(welcome.begin(), welcome.end(),
                                [](uint16_t rgb) {
                                    return (rgb >> 11) > 26 && ((rgb >> 5) & 63) > 27 &&
                                           ((rgb >> 5) & 63) < 49 && (rgb & 31) < 6;
                                }) > 90,
                  "The native welcome screen must contain the orange avatar");
            check(welcome != frame(250), "The welcome satellite must move");
            art.show(okou::OrbitScene::record);
            auto quiet = frame(400);
            std::array<int16_t, 1024> speech{};
            speech.fill(13000);
            mic.append(speech.data(), speech.size());
            check(quiet != frame(0),
                  "Microphone amplitude must change the orbit at the same instant");
            art.show(okou::OrbitScene::send);
            auto departed = frame(120);
            check(departed != frame(900), "Submitting must move the satellite out");
            auto arrived = frame(0);
            check(arrived == frame(500), "The outgoing satellite must stay at the exit");
            art.show(okou::OrbitScene::wait);
            auto accepted = frame(400);
            check(accepted != frame(350), "Waiting for a reply must remain animated");
            for (auto scene :
                 {okou::OrbitScene::pairing, okou::OrbitScene::bookmark, okou::OrbitScene::pause}) {
                art.show(scene);
                auto settled = frame(400);
                check(settled == frame(980), "Pairing, reading and paused audio must settle");
            }
        }
        lv_display_delete(display);
        lv_deinit();
        return 0;
    } catch (const std::exception &ex) {
        std::cerr << ex.what() << '\n';
        return 1;
    }
}
