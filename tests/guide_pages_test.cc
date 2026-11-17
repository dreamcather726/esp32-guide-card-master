#include "../main/boards/jiuchuan-s3/guide_pages.h"
#include "../main/boards/jiuchuan-s3/navigation_overlay.h"
#include <array>
#include <cassert>
#include <fstream>
#include <iostream>
#include <vector>
LV_FONT_DECLARE(font_puhui_basic_20_4);
LV_FONT_DECLARE(font_puhui_20_4);
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
    std::vector<std::array<uint16_t, 240 * 135>> frames;
    {
        GuidePages pages(screen, 240, 135, &font_puhui_basic_20_4);
        pages.SetFont(&font_puhui_20_4);
        for (char32_t ch : U"安全提示播放请关联当前用户聆听中斜梁先生…：") {
            if (!ch) continue;
            lv_font_glyph_dsc_t glyph{};
            assert(lv_font_get_glyph_dsc(&font_puhui_20_4, &glyph, ch, 0) && !glyph.is_placeholder);
        }
        NavigationOverlay navigation(screen, 240, 135);
        navigation.SetSignal(28); navigation.SetBattery(76);
        NavigationStatus status; status.has_fix = true; status.satellites = 24; status.scene = "24812";
        navigation.SetPosition(status);
        auto* root = lv_obj_get_child(screen, 0);
        auto* title = lv_obj_get_child(root, 0);
        auto* speaker = lv_obj_get_child(root, 1);
        auto* viewport = lv_obj_get_child(root, 2);
        auto* body = lv_obj_get_child(viewport, 0);
        auto* image = lv_obj_get_child(viewport, 1);
        auto* number = lv_obj_get_child(root, 3);
        auto* hint = lv_obj_get_child(root, 4);
        auto* toast = lv_obj_get_child(root, 5);
        auto render = [&]() {
            lv_obj_update_layout(screen); lv_refr_now(display);
            assert(framebuffer[4 * 240 + 24] == 0); // Signal stays on top.
            frames.push_back(framebuffer);
        };
        GuideInput input; input.startup_pending = false; input.state = kDeviceStateIdle; input.clock = "08:43";
        pages.SetView(ResolveGuideView(input)); render();
        assert(std::string(lv_label_get_text(number)) == "08:43");
        assert(!lv_obj_has_flag(hint, LV_OBJ_FLAG_HIDDEN));
        assert(lv_obj_get_style_text_font(number, 0)->line_height > 40);
        lv_area_t clock_area, hint_area;
        lv_obj_get_coords(number, &clock_area); lv_obj_get_coords(hint, &hint_area);
        assert(clock_area.y1 >= 20 && clock_area.y2 < hint_area.y1 && hint_area.y2 < 115);
        input.local_playback = true; input.playback.kind = GuideAudioKind::Safety;
        pages.SetView(ResolveGuideView(input)); render();
        assert(std::string(lv_label_get_text(title)) == "安全提示");
        assert(!lv_obj_has_flag(speaker, LV_OBJ_FLAG_HIDDEN));
        assert(lv_obj_has_flag(number, LV_OBJ_FLAG_HIDDEN) && lv_obj_has_flag(hint, LV_OBJ_FLAG_HIDDEN));
        input.playback = {GuideAudioKind::Narration, "斜梁先生",
            "平遥古城龟城本体拟人专属数字导游，守护古城千年的青灰少年，是全域主角。后面的内容需要逐行滚动，不能超出四角图标。", "24812"};
        pages.SetView(ResolveGuideView(input)); render();
        assert(lv_obj_get_height(body) > lv_obj_get_height(viewport));
        lv_tick_inc(1800); lv_timer_handler();
        assert(lv_obj_get_y(body) < 0);
        const int old_y = lv_obj_get_y(body);
        pages.SetView(ResolveGuideView(input));
        assert(lv_obj_get_y(body) == old_y); // One-second refresh must not reset scrolling.
        input.playback.text.clear();
        pages.SetView(ResolveGuideView(input)); render();
        assert(!lv_obj_has_flag(number, LV_OBJ_FLAG_HIDDEN));
        assert(std::string(lv_label_get_text(number)) == "24812");
        // Borrowed images remain inside the center region and are detached.
        std::array<uint16_t, 32 * 32> pixels{};
        lv_image_dsc_t descriptor{};
        descriptor.header.magic = LV_IMAGE_HEADER_MAGIC;
        descriptor.header.cf = LV_COLOR_FORMAT_RGB565;
        descriptor.header.w = descriptor.header.h = 32; descriptor.header.stride = 64;
        descriptor.data_size = pixels.size() * 2; descriptor.data = reinterpret_cast<uint8_t*>(pixels.data());
        pages.SetImage(&descriptor); lv_obj_update_layout(screen);
        assert(!lv_obj_has_flag(image, LV_OBJ_FLAG_HIDDEN) && lv_obj_has_flag(number, LV_OBJ_FLAG_HIDDEN));
        pages.SetImage(nullptr);
        input.local_playback = false; input.state = kDeviceStateListening;
        pages.SetView(ResolveGuideView(input)); render();
        assert(std::string(lv_label_get_text(title)) == "聆听中…");
        assert(std::string(lv_label_get_text(body)) == "请说话");
        input.state = kDeviceStateIdle; input.binding = GuideBinding::Ready;
        pages.SetView(ResolveGuideView(input)); render();
        assert(lv_obj_has_flag(hint, LV_OBJ_FLAG_HIDDEN));
        pages.Notify("电量低 请充电", 3000);
        assert(!lv_obj_has_flag(toast, LV_OBJ_FLAG_HIDDEN));
        lv_tick_inc(3000); lv_timer_handler();
        assert(lv_obj_has_flag(toast, LV_OBJ_FLAG_HIDDEN));
        input.startup_pending = true; pages.SetView(ResolveGuideView(input));
        assert(lv_obj_has_flag(root, LV_OBJ_FLAG_HIDDEN));
    }
    if (argc > 1) {
        std::ofstream file(argv[1], std::ios::binary);
        file << "P6\n720 270\n255\n";
        for (int row = 0; row < 2; ++row) for (int y = 0; y < 135; ++y)
            for (int col = 0; col < 3; ++col) for (int x = 0; x < 240; ++x) {
                uint16_t pixel = frames[row * 3 + col][y * 240 + x];
                const char rgb[3] = {static_cast<char>(((pixel >> 11) & 31) * 255 / 31),
                    static_cast<char>(((pixel >> 5) & 63) * 255 / 63), static_cast<char>((pixel & 31) * 255 / 31)};
                file.write(rgb, 3);
            }
    }
    lv_display_delete(display); lv_deinit();
    std::cout << "PASS real LVGL Chinese guide pages, large clock, binding hint, four-corner layering, scrolling, image detach, notifications and startup priority\n";
}
