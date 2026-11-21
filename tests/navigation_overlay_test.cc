#include "../main/boards/jiuchuan-s3/navigation_overlay.h"
#include <cassert>
#include <fstream>
#include <iostream>

static std::array<uint16_t, 240 * 135> framebuffer{}, buffer{};
static void Flush(lv_display_t* display, const lv_area_t* area, uint8_t* data) {
    auto* pixels = reinterpret_cast<uint16_t*>(data);
    for (int y = area->y1; y <= area->y2; ++y)
        for (int x = area->x1; x <= area->x2; ++x) framebuffer[y * 240 + x] = *pixels++;
    lv_display_flush_ready(display);
}
int main(int argc, char** argv) {
    lv_init();
    auto* display = lv_display_create(240, 135);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, buffer.data(), nullptr, buffer.size() * 2, LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(display, Flush);
    auto* screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_white(), 0);
    lv_obj_set_style_text_color(screen, lv_color_black(), 0);
    auto* title = lv_label_create(screen); lv_label_set_text(title, "Idle");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 3);
    auto* content = lv_label_create(screen); lv_label_set_text(content, "24812");
    lv_obj_set_style_text_font(content, &lv_font_montserrat_32, 0); lv_obj_center(content);
    {
        NavigationOverlay overlay(screen, 240, 135);
        auto* root = lv_obj_get_child(screen, 2);
        auto* signal = lv_obj_get_child(root, 0);
        auto* battery = lv_obj_get_child(root, 1);
        auto* satellite = lv_obj_get_child(root, 2);
        auto* scene = lv_obj_get_child(root, 3);
        auto* satellite_image = lv_obj_get_child(satellite, 0);
        auto* satellite_label = lv_obj_get_child(satellite, 1);
        auto* scene_label = lv_obj_get_child(scene, 2);
        auto* battery_label = lv_obj_get_child(lv_obj_get_child(battery, 0), 0);
        for (int csq : {-1, 0, 1, 7, 8, 15, 16, 26, 27, 28, 31, 99}) {
            overlay.SetSignal(csq);
            for (int i = 0; i < 4; ++i) {
                auto* bar = lv_obj_get_child(signal, i);
                assert(lv_obj_has_flag(bar, LV_OBJ_FLAG_HIDDEN) == (position::SignalBars(csq) == 0));
                assert(lv_color_eq(lv_obj_get_style_bg_color(bar, 0),
                    i < position::SignalBars(csq) ? lv_color_black() : lv_color_white()));
            }
        }
        overlay.SetBattery(76); overlay.SetSignal(28);
        assert(std::string(lv_label_get_text(battery_label)) == "76");
        overlay.SetBattery(100); assert(std::string(lv_label_get_text(battery_label)) == "100");
        overlay.SetBattery(-1); assert(std::string(lv_label_get_text(battery_label)) == "--");
        overlay.SetBattery(76);
        NavigationStatus status; status.has_fix = true; status.satellites = 15; status.scene = "24812";
        overlay.SetPosition(status);
        lv_obj_update_layout(screen);
        assert(std::string(lv_label_get_text(satellite_label)) == "15");
        if (std::string(lv_label_get_text(scene_label)) != "24812")
            std::cerr << "scene label=" << lv_label_get_text(scene_label) << " width=" << lv_obj_get_width(scene_label) << '\n';
        assert(std::string(lv_label_get_text(scene_label)) == "24812");
        lv_obj_update_layout(screen); lv_refr_now(display);
        assert(lv_obj_get_x(signal) == 0 && lv_obj_get_y(signal) == 0);
        assert(lv_obj_get_x(battery) == 197 && lv_obj_get_y(battery) == 0);
        assert(lv_obj_get_y(satellite) == 115 && lv_obj_get_y(scene) == 115);
        assert(lv_obj_get_x(scene) == 156);
        assert(lv_obj_get_x(content) >= 38 && lv_obj_get_y(content) >= 20);
        assert(framebuffer[4 * 240 + 3] == 0); // Tallest signal bar.
        int satellite_ink = 0;
        for (int y = 115; y < 135; ++y) for (int x = 2; x < 16; ++x)
            if (framebuffer[y * 240 + x] != 0xffff) ++satellite_ink;
        assert(satellite_ink > 10); // Satellite must be visible on white, distinct from the position pin.
        if (argc > 1) {
            std::ofstream file(argv[1], std::ios::binary);
            file << "P6\n240 135\n255\n";
            for (uint16_t pixel : framebuffer) {
                const char rgb[3] = {static_cast<char>(((pixel >> 11) & 31) * 255 / 31),
                    static_cast<char>(((pixel >> 5) & 63) * 255 / 63), static_cast<char>((pixel & 31) * 255 / 31)};
                file.write(rgb, 3);
            }
        }
        // Startup overlay is later in the screen hierarchy and covers every
        // telemetry corner. Closing it restores the existing live data.
        auto* splash = lv_obj_create(screen); lv_obj_remove_style_all(splash);
        lv_obj_set_size(splash, 240, 135);
        lv_obj_set_style_bg_color(splash, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(splash, LV_OPA_COVER, 0);
        lv_refr_now(display); assert(framebuffer[4 * 240 + 3] == 0xffff);
        lv_obj_delete(splash); lv_refr_now(display); assert(framebuffer[4 * 240 + 3] == 0);
        overlay.SetPosition(NavigationStatus{});
        assert(std::string(lv_label_get_text(satellite_label)) == "--");
        assert(std::string(lv_label_get_text(scene_label)) == "--");
        lv_tick_inc(1000); lv_timer_handler();
        assert(lv_obj_get_style_opa(satellite_image, 0) == LV_OPA_30);
        overlay.SetPosition(status); lv_tick_inc(1000); lv_timer_handler();
        assert(lv_obj_get_style_opa(satellite_image, 0) == LV_OPA_COVER);
    }
    lv_obj_clean(screen); lv_display_delete(display); lv_deinit();
    std::cout << "PASS real LVGL four-corner placement, signal intervals, battery digits, satellite/scene updates, stale placeholders, fix blink and startup overlay priority\n";
}
