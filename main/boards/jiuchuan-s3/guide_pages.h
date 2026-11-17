#pragma once
#include "../../display/guide_state.h"
#include <lvgl.h>
#include <algorithm>

// Created below NavigationOverlay and the startup QR. The controller owns only
// its widgets/timers; preview descriptors are borrowed from the display cache.
class GuidePages {
public:
    GuidePages(lv_obj_t* screen, int width, int height, const lv_font_t* text_font)
        : width_(width), height_(height) {
        root_ = lv_obj_create(screen);
        lv_obj_remove_style_all(root_);
        lv_obj_set_size(root_, width, height);
        lv_obj_set_style_bg_color(root_, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(root_, lv_color_black(), 0);
        lv_obj_set_style_text_font(root_, text_font, 0);
        lv_obj_remove_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
        title_ = Label(root_);
        lv_obj_set_size(title_, width - 92, 25);
        lv_obj_set_pos(title_, 42, 0);
        lv_label_set_long_mode(title_, LV_LABEL_LONG_DOT);
        speaker_ = Label(root_);
        lv_obj_set_style_text_font(speaker_, LV_FONT_DEFAULT, 0);
        lv_label_set_text(speaker_, LV_SYMBOL_VOLUME_MAX);
        lv_obj_set_pos(speaker_, 39, 3);
        viewport_ = lv_obj_create(root_);
        lv_obj_remove_style_all(viewport_);
        lv_obj_set_size(viewport_, width - 12,
            std::max(1, (height - 48) / int(text_font->line_height)) * int(text_font->line_height));
        lv_obj_set_pos(viewport_, 6, 26);
        lv_obj_remove_flag(viewport_, LV_OBJ_FLAG_SCROLLABLE);
        body_ = Label(viewport_);
        lv_obj_set_width(body_, width - 12);
        lv_label_set_long_mode(body_, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(body_, LV_TEXT_ALIGN_LEFT, 0);
        number_ = Label(root_);
        lv_obj_set_style_text_font(number_, &lv_font_montserrat_48, 0);
        lv_obj_set_width(number_, width - 12);
        lv_label_set_long_mode(number_, LV_LABEL_LONG_DOT);
        image_ = lv_image_create(viewport_);
        hint_ = Label(root_);
        lv_obj_set_style_bg_color(hint_, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(hint_, LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(hint_, lv_color_white(), 0);
        lv_obj_set_style_pad_hor(hint_, 5, 0);
        lv_obj_set_style_radius(hint_, 3, 0);
        lv_obj_align(hint_, LV_ALIGN_BOTTOM_MID, 0, -23);
        toast_ = Label(root_);
        lv_obj_set_width(toast_, width - 12);
        lv_obj_set_style_bg_color(toast_, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(toast_, LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(toast_, lv_color_white(), 0);
        lv_obj_align(toast_, LV_ALIGN_BOTTOM_MID, 0, -21);
        Hide(toast_, true);
        scroll_timer_ = lv_timer_create([](lv_timer_t* timer) {
            static_cast<GuidePages*>(lv_timer_get_user_data(timer))->Scroll();
        }, 1800, this);
        toast_timer_ = lv_timer_create([](lv_timer_t* timer) {
            auto* self = static_cast<GuidePages*>(lv_timer_get_user_data(timer));
            Hide(self->toast_, true);
            lv_timer_pause(timer);
        }, 3000, this);
        lv_timer_pause(toast_timer_);
        lv_timer_pause(scroll_timer_);
        Hide(root_, true);
    }

    ~GuidePages() {
        lv_timer_delete(scroll_timer_);
        lv_timer_delete(toast_timer_);
        lv_obj_delete(root_);
    }

    void SetFont(const lv_font_t* font) {
        // Assets replace the boot subset with the complete Chinese font.
        // Explicit root styles must follow that replacement as well.
        lv_obj_set_style_text_font(root_, font, 0);
        lv_obj_set_height(viewport_, std::max(1, (height_ - 48) / int(font->line_height)) * int(font->line_height));
        if (has_view_) LayoutContent();
    }

    void SetView(const GuideView& view) {
        if (has_view_ && view == view_) return;
        const bool changed_body = !has_view_ || view.page != view_.page || view.text != view_.text;
        view_ = view;
        has_view_ = true;
        Hide(root_, view.page == GuidePage::Legacy);
        lv_label_set_text(title_, view.title.c_str());
        Hide(speaker_, !view.speaker);
        // Leave enough room for the speaker between the signal and title.
        lv_obj_set_pos(title_, view.speaker ? 58 : 42, 0);
        lv_obj_set_width(title_, width_ - (view.speaker ? 108 : 92));
        lv_label_set_text(hint_, view.hint.c_str());
        Hide(hint_, view.hint.empty());
        lv_obj_align(hint_, LV_ALIGN_BOTTOM_MID, 0, -23);
        lv_label_set_text(number_, view.number.empty() ? "--" : view.number.c_str());
        lv_obj_align(number_, LV_ALIGN_CENTER, 0, view.hint.empty() ? 1 : -8);
        lv_label_set_text(body_, view.text.c_str());
        lv_obj_set_style_text_align(body_, view.page == GuidePage::Narration ? LV_TEXT_ALIGN_LEFT : LV_TEXT_ALIGN_CENTER, 0);
        if (changed_body) {
            scroll_y_ = 0;
            lv_obj_align(body_, LV_ALIGN_TOP_LEFT, 0, 0);
            lv_timer_reset(scroll_timer_);
        }
        LayoutContent();
    }

    void SetImage(const lv_image_dsc_t* image) {
        image_src_ = image;
        // Detach before the caller frees the old descriptor/pixels.
        lv_image_set_src(image_, image);
        if (has_view_) LayoutContent();
    }

    void Notify(const char* text, int duration_ms) {
        if (!text || !*text) return;
        lv_label_set_text(toast_, text);
        Hide(toast_, false);
        lv_obj_align(toast_, LV_ALIGN_BOTTOM_MID, 0, -21);
        lv_timer_set_period(toast_timer_, std::max(1, duration_ms));
        lv_timer_reset(toast_timer_);
        lv_timer_resume(toast_timer_);
    }

private:
    int width_, height_, scroll_y_ = 0, scroll_max_ = 0;
    bool has_view_ = false;
    GuideView view_;
    const lv_image_dsc_t* image_src_ = nullptr;
    lv_obj_t *root_, *title_, *speaker_, *viewport_, *body_, *number_, *hint_, *image_, *toast_;
    lv_timer_t *scroll_timer_, *toast_timer_;
    static void Hide(lv_obj_t* obj, bool hidden) {
        if (hidden) lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
    static lv_obj_t* Label(lv_obj_t* parent) {
        auto* label = lv_label_create(parent);
        lv_label_set_text(label, "");
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        return label;
    }
    void LayoutContent() {
        const bool narration = view_.page == GuidePage::Narration;
        const bool picture = narration && image_src_ && image_src_->header.w && image_src_->header.h;
        const bool number = view_.page == GuidePage::Idle || (narration && view_.text.empty() && !picture);
        Hide(image_, !picture);
        Hide(number_, !number);
        Hide(body_, number || picture || view_.page == GuidePage::Legacy);
        if (picture) {
            const int scale = std::max(1, std::min((width_ - 12) * 256 / int(image_src_->header.w),
                                                   (height_ - 48) * 256 / int(image_src_->header.h)));
            lv_image_set_scale(image_, scale);
            lv_obj_center(image_);
        }
        lv_obj_update_layout(viewport_);
        scroll_max_ = std::max(0, int(lv_obj_get_height(body_) - lv_obj_get_height(viewport_)));
        if (!number && !picture && view_.page != GuidePage::Legacy && scroll_max_ > 0) {
            lv_obj_align(body_, LV_ALIGN_TOP_LEFT, 0, -std::min(scroll_y_, scroll_max_));
            lv_timer_resume(scroll_timer_);
        } else {
            lv_obj_align(body_, LV_ALIGN_CENTER, 0, 0);
            lv_timer_pause(scroll_timer_);
        }
    }
    void Scroll() {
        if (scroll_max_ <= 0) return;
        const auto* font = lv_obj_get_style_text_font(body_, 0);
        const int line = font->line_height + lv_obj_get_style_text_line_space(body_, 0);
        scroll_y_ = scroll_y_ >= scroll_max_ ? 0 : std::min(scroll_y_ + line, scroll_max_);
        lv_obj_set_y(body_, -scroll_y_);
    }
};
