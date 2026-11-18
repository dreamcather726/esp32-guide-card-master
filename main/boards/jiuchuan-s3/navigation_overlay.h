#pragma once
#include "position_data.h"
#include <lvgl.h>
#include <algorithm>
#include <array>
#include <cstdio>

// Board-specific four-corner telemetry. All methods run under DisplayLockGuard.
// LVGL owns the widgets; the display owns this controller and its blink timer.
class NavigationOverlay {
public:
    NavigationOverlay(lv_obj_t* screen, int width, int height) {
        layer_ = lv_obj_create(screen);
        lv_obj_remove_style_all(layer_);
        lv_obj_set_size(layer_, width, height);
        lv_obj_remove_flag(layer_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(layer_, LV_OBJ_FLAG_CLICKABLE);
        auto* signal = Panel(0, 0, 38, 20);
        for (int i = 0; i < 4; ++i) {
            bars_[i] = lv_obj_create(signal);
            lv_obj_remove_style_all(bars_[i]);
            lv_obj_set_size(bars_[i], 4, 5 + i * 3);
            lv_obj_set_pos(bars_[i], 3 + i * 7, 17 - (5 + i * 3));
            lv_obj_set_style_bg_color(bars_[i], lv_color_black(), 0);
            lv_obj_set_style_bg_opa(bars_[i], LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(bars_[i], 1, 0);
        }
        SetSignal(-1);

        auto* battery_panel = Panel(width - 43, 0, 43, 20);
        auto* battery = lv_obj_create(battery_panel);
        lv_obj_remove_style_all(battery);
        lv_obj_set_size(battery, 33, 16);
        lv_obj_set_pos(battery, 3, 2);
        lv_obj_set_style_border_width(battery, 1, 0);
        lv_obj_set_style_border_color(battery, lv_color_black(), 0);
        lv_obj_set_style_radius(battery, 2, 0);
        auto* nub = lv_obj_create(battery_panel);
        lv_obj_remove_style_all(nub);
        lv_obj_set_size(nub, 3, 6);
        lv_obj_set_pos(nub, 36, 7);
        lv_obj_set_style_bg_color(nub, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(nub, LV_OPA_COVER, 0);
        battery_label_ = Label(battery, "--");
        lv_obj_center(battery_label_);

        auto* satellite_panel = Panel(0, height - 20, 60, 20);
        satellite_image_ = lv_image_create(satellite_panel);
        lv_obj_remove_style_all(satellite_image_);
        lv_image_set_src(satellite_image_, SatelliteIcon());
        lv_obj_set_pos(satellite_image_, 2, 0);
        lv_obj_set_style_image_recolor(satellite_image_, lv_color_black(), 0);
        satellite_label_ = Label(satellite_panel, "--");
        lv_obj_set_pos(satellite_label_, 26, 2);

        auto* scene_panel = Panel(width - 84, height - 20, 84, 20);
        auto* ring = lv_obj_create(scene_panel);
        lv_obj_remove_style_all(ring);
        lv_obj_set_size(ring, 11, 11);
        lv_obj_set_pos(ring, 2, 1);
        lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(ring, 2, 0);
        lv_obj_set_style_border_color(ring, lv_color_black(), 0);
        auto* center = lv_obj_create(ring);
        lv_obj_remove_style_all(center);
        lv_obj_set_size(center, 3, 3);
        lv_obj_set_pos(center, 2, 2);
        lv_obj_set_style_radius(center, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(center, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(center, LV_OPA_COVER, 0);
        auto* tip = lv_line_create(scene_panel);
        lv_line_set_points(tip, pin_points_.data(), pin_points_.size());
        lv_obj_set_pos(tip, 2, 8);
        lv_obj_set_style_line_width(tip, 2, 0);
        lv_obj_set_style_line_color(tip, lv_color_black(), 0);
        scene_label_ = Label(scene_panel, "--");
        lv_obj_set_pos(scene_label_, 18, 2);
        lv_obj_set_width(scene_label_, 64);
        lv_label_set_long_mode(scene_label_, LV_LABEL_LONG_DOT);

        blink_timer_ = lv_timer_create([](lv_timer_t* timer) {
            auto* self = static_cast<NavigationOverlay*>(lv_timer_get_user_data(timer));
            if (self->has_fix_) return;
            self->blink_visible_ = !self->blink_visible_;
            lv_obj_set_style_opa(self->satellite_image_, self->blink_visible_ ? LV_OPA_COVER : LV_OPA_30, 0);
        }, 1000, this);
    }

    ~NavigationOverlay() {
        if (blink_timer_) lv_timer_delete(blink_timer_);
        lv_obj_delete(layer_);
    }

    void SetPosition(const NavigationStatus& status) {
        has_fix_ = status.has_fix;
        blink_visible_ = true;
        lv_obj_set_style_opa(satellite_image_, LV_OPA_COVER, 0);
        char satellites[8] = "--";
        if (status.satellites >= 0 && status.satellites <= 99)
            std::snprintf(satellites, sizeof(satellites), "%d", status.satellites);
        lv_label_set_text(satellite_label_, satellites);
        lv_label_set_text(scene_label_, status.scene.empty() ? "--" : status.scene.c_str());
    }

    void SetSignal(int csq) {
        const int count = position::SignalBars(csq);
        for (int i = 0; i < 4; ++i) {
            // Empty grey outlines make unknown/no service visible without
            // inventing signal bars. Only measured levels fill solid black.
            lv_obj_set_style_bg_color(bars_[i], i < count ? lv_color_black() : lv_color_white(), 0);
            lv_obj_set_style_border_color(bars_[i], i < count ? lv_color_black() : lv_color_hex(0xb0b0b0), 0);
        }
    }

    void SetBattery(int percent) {
        char text[8] = "--";
        if (percent >= 0) std::snprintf(text, sizeof(text), "%d", std::clamp(percent, 0, 100));
        lv_label_set_text(battery_label_, text);
    }

private:
    lv_obj_t* layer_ = nullptr;
    std::array<lv_obj_t*, 4> bars_{};
    std::array<lv_point_precise_t, 3> pin_points_{{{1, 0}, {5, 8}, {9, 0}}};
    lv_obj_t* battery_label_ = nullptr;
    lv_obj_t* satellite_image_ = nullptr;
    lv_obj_t* satellite_label_ = nullptr;
    lv_obj_t* scene_label_ = nullptr;
    lv_timer_t* blink_timer_ = nullptr;
    bool has_fix_ = false, blink_visible_ = true;

    static const lv_image_dsc_t* SatelliteIcon() {
        static constexpr int width = 21, height = 19;
        static constexpr char silhouette[height][width + 1] = {
            "..........#..........",
            ".........###.........",
            "........#####........",
            ".#####...###...#####.",
            "#######.#####.#######",
            "#######.#####.#######",
            "#####################",
            "#######.#####.#######",
            "#######.#####.#######",
            ".#####...###...#####.",
            "........#####........",
            "..........#..........",
            "..........#..........",
            "........#####........",
            ".......##...##.......",
            ".......##...##.......",
            "........#####........",
            ".........###.........",
            "..........#..........",
        };
        alignas(LV_DRAW_BUF_ALIGN) static constexpr auto pixels = [] {
            std::array<uint8_t, width * height> alpha{};
            for (int y = 0; y < height; ++y) {
                for (int x = 0; x < width; ++x) {
                    alpha[y * width + x] = silhouette[y][x] == '#' ? LV_OPA_COVER : LV_OPA_TRANSP;
                }
            }
            return alpha;
        }();
        static const lv_image_dsc_t image = [] {
            lv_image_dsc_t descriptor{};
            descriptor.header.magic = LV_IMAGE_HEADER_MAGIC;
            descriptor.header.cf = LV_COLOR_FORMAT_A8;
            descriptor.header.w = width;
            descriptor.header.h = height;
            descriptor.header.stride = width;
            descriptor.data_size = pixels.size();
            descriptor.data = pixels.data();
            return descriptor;
        }();
        return &image;
    }

    lv_obj_t* Panel(int x, int y, int width, int height) {
        auto* object = lv_obj_create(layer_);
        lv_obj_remove_style_all(object);
        lv_obj_set_pos(object, x, y);
        lv_obj_set_size(object, width, height);
        lv_obj_set_style_bg_color(object, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
        lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(object, LV_OBJ_FLAG_CLICKABLE);
        return object;
    }
    static lv_obj_t* Label(lv_obj_t* parent, const char* text) {
        auto* label = lv_label_create(parent);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(label, lv_color_black(), 0);
        lv_label_set_text(label, text);
        return label;
    }
};
