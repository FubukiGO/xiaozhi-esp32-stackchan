#include "stackchan_display.h"

#include "assets/lang_config.h"
#include "lvgl_theme.h"
#include "settings.h"

#include <algorithm>
#include <cstring>
#include <esp_err.h>
#include <esp_log.h>
#include <font_awesome.h>

#define TAG "StackChanDisplay"

namespace {
constexpr int kDesignWidth = 320;
constexpr int kDesignHeight = 240;
constexpr int kEyeBaseSize = 32;
constexpr int kEyeMinSize = 8;
constexpr int kEyeMaxSize = 32;
constexpr int kEyeBaseX = 70;
constexpr int kEyeBaseY = -16;
constexpr int kMouthBaseY = 26;
constexpr int kBubbleMinWidth = 90;
constexpr int kBubbleMaxWidth = 300;
constexpr int kBubbleHeight = 52;
constexpr int kBubbleTextMargin = 20;
}  // namespace

StackChanDisplay::StackChanDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                                   int width, int height, int offset_x, int offset_y,
                                   bool mirror_x, bool mirror_y, bool swap_xy)
    : SpiLcdDisplay(panel_io, panel, width, height, offset_x, offset_y, mirror_x, mirror_y, swap_xy) {
    esp_timer_create_args_t speaking_timer_args = {
        .callback = [](void* arg) {
            static_cast<StackChanDisplay*>(arg)->AnimateSpeakingMouth();
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "stackchan_mouth",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&speaking_timer_args, &speaking_timer_));

    esp_timer_create_args_t notification_timer_args = {
        .callback = [](void* arg) {
            auto* display = static_cast<StackChanDisplay*>(arg);
            DisplayLockGuard lock(display);
            if (display->notification_label_ != nullptr) {
                lv_obj_add_flag(display->notification_label_, LV_OBJ_FLAG_HIDDEN);
            }
            if (display->status_label_ != nullptr) {
                lv_obj_remove_flag(display->status_label_, LV_OBJ_FLAG_HIDDEN);
            }
            display->UpdateSpeechBubble("");
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "stackchan_notice",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&notification_timer_args, &notification_hide_timer_));
}

StackChanDisplay::~StackChanDisplay() {
    if (speaking_timer_ != nullptr) {
        esp_timer_stop(speaking_timer_);
        esp_timer_delete(speaking_timer_);
    }
    if (notification_hide_timer_ != nullptr) {
        esp_timer_stop(notification_hide_timer_);
        esp_timer_delete(notification_hide_timer_);
    }
    if (preview_timer_ != nullptr) {
        esp_timer_stop(preview_timer_);
        esp_timer_delete(preview_timer_);
        preview_timer_ = nullptr;
    }
    if (preview_image_ != nullptr) {
        lv_obj_del(preview_image_);
        preview_image_ = nullptr;
    }
    if (speech_bubble_ != nullptr) {
        lv_obj_del(speech_bubble_);
        speech_bubble_ = nullptr;
        speech_label_ = nullptr;
    }
    if (face_panel_ != nullptr) {
        lv_obj_del(face_panel_);
        face_panel_ = nullptr;
        left_eye_ = {};
        right_eye_ = {};
        mouth_ = nullptr;
    }
}

int StackChanDisplay::MapRange(int value, int in_min, int in_max, int out_min, int out_max) {
    value = std::clamp(value, in_min, in_max);
    return (value - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}

StackChanDisplay::StackChanEmotion StackChanDisplay::ParseEmotion(const char* emotion) {
    if (emotion == nullptr) {
        return StackChanEmotion::Neutral;
    }
    if (strcmp(emotion, "happy") == 0 || strcmp(emotion, "laughing") == 0 || strcmp(emotion, "funny") == 0 ||
        strcmp(emotion, "loving") == 0 || strcmp(emotion, "relaxed") == 0 || strcmp(emotion, "delicious") == 0 ||
        strcmp(emotion, "kissy") == 0 || strcmp(emotion, "confident") == 0 || strcmp(emotion, "cool") == 0) {
        return StackChanEmotion::Happy;
    }
    if (strcmp(emotion, "angry") == 0) {
        return StackChanEmotion::Angry;
    }
    if (strcmp(emotion, "sad") == 0 || strcmp(emotion, "crying") == 0 || strcmp(emotion, "embarrassed") == 0) {
        return StackChanEmotion::Sad;
    }
    if (strcmp(emotion, "thinking") == 0 || strcmp(emotion, "confused") == 0 || strcmp(emotion, "surprised") == 0 ||
        strcmp(emotion, "shocked") == 0 || strcmp(emotion, "doubtful") == 0) {
        return StackChanEmotion::Doubt;
    }
    if (strcmp(emotion, "sleepy") == 0) {
        return StackChanEmotion::Sleepy;
    }
    return StackChanEmotion::Neutral;
}

lv_coord_t StackChanDisplay::ScaleX(int value) const {
    return value * width_ / kDesignWidth;
}

lv_coord_t StackChanDisplay::ScaleY(int value) const {
    return value * height_ / kDesignHeight;
}

lv_coord_t StackChanDisplay::ScaleMin(int value) const {
    return value * std::min(width_, height_) / kDesignHeight;
}

void StackChanDisplay::SetupUI() {
    if (setup_ui_called_) {
        ESP_LOGW(TAG, "SetupUI() called multiple times, skipping duplicate call");
        return;
    }

    Display::SetupUI();
    DisplayLockGuard lock(this);

    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    auto text_font = lvgl_theme->text_font()->font();
    auto icon_font = lvgl_theme->icon_font()->font();

    lv_obj_t* screen = lv_screen_active();
    lv_obj_set_style_text_font(screen, text_font, 0);
    lv_obj_set_style_text_color(screen, lvgl_theme->text_color(), 0);
    lv_obj_set_style_bg_color(screen, lvgl_theme->background_color(), 0);

    container_ = lv_obj_create(screen);
    lv_obj_set_size(container_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_radius(container_, 0, 0);
    lv_obj_set_style_pad_all(container_, 0, 0);
    lv_obj_set_style_border_width(container_, 0, 0);
    lv_obj_set_style_bg_color(container_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_bg_opa(container_, LV_OPA_TRANSP, 0);
    lv_obj_set_scrollbar_mode(container_, LV_SCROLLBAR_MODE_OFF);

    CreateAvatar(screen, text_font);
    CreateStatusBars(screen, text_font, icon_font);

    preview_image_ = lv_image_create(screen);
    lv_obj_set_size(preview_image_, width_, height_);
    lv_obj_align(preview_image_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);

    ApplyEmotion(StackChanEmotion::Neutral);
    ESP_LOGI(TAG, "StackChan avatar UI created");
}

void StackChanDisplay::CreateStatusBars(lv_obj_t* screen, const lv_font_t* text_font, const lv_font_t* icon_font) {
    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);

    top_bar_ = lv_obj_create(screen);
    lv_obj_set_size(top_bar_, LV_HOR_RES, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(top_bar_, 0, 0);
    lv_obj_set_style_bg_opa(top_bar_, LV_OPA_30, 0);
    lv_obj_set_style_bg_color(top_bar_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_border_width(top_bar_, 0, 0);
    lv_obj_set_style_pad_all(top_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_flex_flow(top_bar_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top_bar_, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(top_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_align(top_bar_, LV_ALIGN_TOP_MID, 0, 0);

    network_label_ = lv_label_create(top_bar_);
    lv_label_set_text(network_label_, "");
    lv_obj_set_style_text_font(network_label_, icon_font, 0);
    lv_obj_set_style_text_color(network_label_, lvgl_theme->text_color(), 0);

    lv_obj_t* right_icons = lv_obj_create(top_bar_);
    lv_obj_set_size(right_icons, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(right_icons, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(right_icons, 0, 0);
    lv_obj_set_style_pad_all(right_icons, 0, 0);
    lv_obj_set_flex_flow(right_icons, LV_FLEX_FLOW_ROW);

    mute_label_ = lv_label_create(right_icons);
    lv_label_set_text(mute_label_, "");
    lv_obj_set_style_text_font(mute_label_, icon_font, 0);
    lv_obj_set_style_text_color(mute_label_, lvgl_theme->text_color(), 0);

    battery_label_ = lv_label_create(right_icons);
    lv_label_set_text(battery_label_, "");
    lv_obj_set_style_text_font(battery_label_, icon_font, 0);
    lv_obj_set_style_text_color(battery_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_margin_left(battery_label_, lvgl_theme->spacing(2), 0);

    status_bar_ = lv_obj_create(screen);
    lv_obj_set_size(status_bar_, LV_HOR_RES, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(status_bar_, 0, 0);
    lv_obj_set_style_bg_opa(status_bar_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(status_bar_, 0, 0);
    lv_obj_set_style_pad_all(status_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_align(status_bar_, LV_ALIGN_TOP_MID, 0, 0);

    status_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(status_label_, LV_HOR_RES * 3 / 4);
    lv_label_set_long_mode(status_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_font(status_label_, text_font, 0);
    lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(status_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(status_label_, Lang::Strings::INITIALIZING);
    lv_obj_align(status_label_, LV_ALIGN_CENTER, 0, 0);

    notification_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(notification_label_, LV_HOR_RES * 3 / 4);
    lv_obj_set_style_text_font(notification_label_, text_font, 0);
    lv_obj_set_style_text_align(notification_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(notification_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(notification_label_, "");
    lv_obj_align(notification_label_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);
}

void StackChanDisplay::CreateAvatar(lv_obj_t* screen, const lv_font_t* text_font) {
    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    primary_color_ = lvgl_theme->text_color();
    secondary_color_ = lvgl_theme->background_color();

    face_panel_ = lv_obj_create(screen);
    lv_obj_set_size(face_panel_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_radius(face_panel_, 0, 0);
    lv_obj_set_style_border_width(face_panel_, 0, 0);
    lv_obj_set_style_pad_all(face_panel_, 0, 0);
    lv_obj_set_style_bg_color(face_panel_, secondary_color_, 0);
    lv_obj_set_scrollbar_mode(face_panel_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(face_panel_, LV_OBJ_FLAG_SCROLLABLE);

    CreateEye(left_eye_, true);
    CreateEye(right_eye_, false);

    mouth_ = lv_obj_create(face_panel_);
    lv_obj_set_style_border_width(mouth_, 0, 0);
    lv_obj_set_style_bg_color(mouth_, primary_color_, 0);
    lv_obj_remove_flag(mouth_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(mouth_, LV_ALIGN_CENTER, 0, ScaleY(kMouthBaseY));
    ApplyMouthWeight(0);

    speech_bubble_ = lv_obj_create(screen);
    lv_obj_set_size(speech_bubble_, ScaleX(kBubbleMinWidth), ScaleY(kBubbleHeight));
    lv_obj_set_style_radius(speech_bubble_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(speech_bubble_, 0, 0);
    lv_obj_set_style_bg_color(speech_bubble_, primary_color_, 0);
    lv_obj_set_style_pad_left(speech_bubble_, ScaleX(kBubbleTextMargin), 0);
    lv_obj_set_style_pad_right(speech_bubble_, ScaleX(kBubbleTextMargin), 0);
    lv_obj_set_scrollbar_mode(speech_bubble_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_align(speech_bubble_, LV_ALIGN_BOTTOM_MID, 0, -ScaleY(10));

    speech_label_ = lv_label_create(speech_bubble_);
    lv_obj_set_width(speech_label_, ScaleX(kBubbleMaxWidth - kBubbleTextMargin * 2));
    lv_obj_set_style_text_font(speech_label_, text_font, 0);
    lv_obj_set_style_text_color(speech_label_, secondary_color_, 0);
    lv_obj_set_style_text_align(speech_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(speech_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_label_set_text(speech_label_, "");
    lv_obj_center(speech_label_);
    lv_obj_add_flag(speech_bubble_, LV_OBJ_FLAG_HIDDEN);
}

void StackChanDisplay::CreateEye(EyeObjects& eye, bool is_left) {
    eye.is_left = is_left;
    eye.container = lv_obj_create(face_panel_);
    lv_obj_set_size(eye.container, ScaleMin(kEyeBaseSize), ScaleMin(kEyeBaseSize));
    lv_obj_set_style_radius(eye.container, 0, 0);
    lv_obj_set_style_bg_opa(eye.container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(eye.container, 0, 0);
    lv_obj_set_style_pad_all(eye.container, 0, 0);
    lv_obj_remove_flag(eye.container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(eye.container, LV_ALIGN_CENTER, ScaleX(is_left ? -kEyeBaseX : kEyeBaseX), ScaleY(kEyeBaseY));

    eye.eye = lv_obj_create(eye.container);
    lv_obj_set_style_radius(eye.eye, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(eye.eye, 0, 0);
    lv_obj_set_style_bg_color(eye.eye, primary_color_, 0);
    lv_obj_remove_flag(eye.eye, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_center(eye.eye);

    eye.eyelid = lv_obj_create(eye.container);
    lv_obj_set_style_radius(eye.eyelid, 0, 0);
    lv_obj_set_style_border_width(eye.eyelid, 0, 0);
    lv_obj_set_style_bg_color(eye.eyelid, secondary_color_, 0);
    lv_obj_remove_flag(eye.eyelid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_center(eye.eyelid);
}

void StackChanDisplay::ApplyEyeStyle(EyeObjects& eye, int weight, int rotation) {
    if (eye.container == nullptr || eye.eye == nullptr || eye.eyelid == nullptr) {
        return;
    }

    int eye_size = MapRange(0, -100, 100, kEyeMinSize, kEyeMaxSize);
    lv_coord_t scaled_eye_size = ScaleMin(eye_size);
    lv_obj_set_size(eye.eye, scaled_eye_size, scaled_eye_size);
    lv_obj_set_size(eye.eyelid, scaled_eye_size, scaled_eye_size);
    lv_obj_set_y(eye.eyelid, -MapRange(weight, 0, 100, 0, scaled_eye_size));
    lv_obj_set_style_transform_rotation(eye.container, eye.is_left ? rotation : -rotation, 0);
}

void StackChanDisplay::ApplyMouthWeight(int weight) {
    if (mouth_ == nullptr) {
        return;
    }

    int width = MapRange(weight, 0, 100, 90, 60);
    int height = MapRange(weight, 0, 100, 6, 50);
    int radius = MapRange(weight, 0, 100, 0, 16);
    lv_obj_set_size(mouth_, ScaleX(width), ScaleY(height));
    lv_obj_set_style_radius(mouth_, ScaleMin(radius), 0);
}

void StackChanDisplay::ApplyEmotion(StackChanEmotion emotion) {
    int weight = 100;
    int rotation = 0;
    switch (emotion) {
        case StackChanEmotion::Happy:
            weight = 72;
            rotation = 1550;
            break;
        case StackChanEmotion::Angry:
            weight = 70;
            rotation = 450;
            break;
        case StackChanEmotion::Sad:
            weight = 70;
            rotation = -400;
            break;
        case StackChanEmotion::Doubt:
            weight = 75;
            rotation = 0;
            break;
        case StackChanEmotion::Sleepy:
            weight = 35;
            rotation = -50;
            break;
        case StackChanEmotion::Neutral:
        default:
            weight = 100;
            rotation = 0;
            break;
    }

    ApplyEyeStyle(left_eye_, weight, rotation);
    ApplyEyeStyle(right_eye_, weight, rotation);
    if (!speaking_) {
        ApplyMouthWeight(emotion == StackChanEmotion::Sleepy ? 10 : 0);
    }
}

void StackChanDisplay::SetEmotion(const char* emotion) {
    if (!setup_ui_called_) {
        ESP_LOGW(TAG, "SetEmotion('%s') called before SetupUI() - emotion will not be displayed!", emotion);
        return;
    }

    DisplayLockGuard lock(this);
    auto stackchan_emotion = ParseEmotion(emotion);
    ApplyEmotion(stackchan_emotion);
    if (stackchan_emotion == StackChanEmotion::Sleepy) {
        UpdateSpeechBubble("Zzz…");
    }
}

void StackChanDisplay::SetChatMessage(const char* role, const char* content) {
    if (!setup_ui_called_) {
        ESP_LOGW(TAG, "SetChatMessage('%s', '%s') called before SetupUI() - message will be lost!", role, content);
        return;
    }
    if (role == nullptr || content == nullptr) {
        return;
    }

    DisplayLockGuard lock(this);
    if (strcmp(role, "assistant") == 0 || strcmp(role, "system") == 0) {
        UpdateSpeechBubble(content);
    } else if (strcmp(role, "user") == 0) {
        lv_obj_add_flag(speech_bubble_, LV_OBJ_FLAG_HIDDEN);
    }
}

void StackChanDisplay::ClearChatMessages() {
    DisplayLockGuard lock(this);
    UpdateSpeechBubble("");
}

void StackChanDisplay::SetStatus(const char* status) {
    if (status == nullptr) {
        return;
    }

    LvglDisplay::SetStatus(status);

    if (!setup_ui_called_) {
        return;
    }

    DisplayLockGuard lock(this);
    if (strcmp(status, Lang::Strings::SPEAKING) == 0) {
        StartSpeaking();
    } else {
        StopSpeaking();
        if (strcmp(status, Lang::Strings::STANDBY) == 0 || strcmp(status, Lang::Strings::LISTENING) == 0) {
            UpdateSpeechBubble("");
        } else if (strcmp(status, Lang::Strings::CONNECTING) != 0) {
            UpdateSpeechBubble(status);
        }
    }
}

void StackChanDisplay::ShowNotification(const char* notification, int duration_ms) {
    if (!setup_ui_called_) {
        ESP_LOGW(TAG, "ShowNotification('%s') called before SetupUI() - message will be lost!", notification);
        return;
    }

    DisplayLockGuard lock(this);
    if (notification_label_ == nullptr || status_label_ == nullptr) {
        return;
    }
    lv_label_set_text(notification_label_, notification != nullptr ? notification : "");
    lv_obj_remove_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
    UpdateSpeechBubble(notification);

    esp_timer_stop(notification_hide_timer_);
    ESP_ERROR_CHECK(esp_timer_start_once(notification_hide_timer_, duration_ms * 1000));
}

void StackChanDisplay::SetPreviewImage(std::unique_ptr<LvglImage> image) {
    DisplayLockGuard lock(this);
    if (preview_image_ == nullptr) {
        return;
    }

    if (image == nullptr) {
        esp_timer_stop(preview_timer_);
        lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
        preview_image_cached_.reset();
        return;
    }

    preview_image_cached_ = std::move(image);
    auto img_dsc = preview_image_cached_->image_dsc();
    lv_image_set_src(preview_image_, img_dsc);
    if (img_dsc->header.w > 0 && img_dsc->header.h > 0) {
        lv_coord_t zoom_w = width_ * 256 / img_dsc->header.w;
        lv_coord_t zoom_h = height_ * 256 / img_dsc->header.h;
        lv_image_set_scale(preview_image_, std::min(zoom_w, zoom_h));
    }

    lv_obj_remove_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(preview_image_);
    esp_timer_stop(preview_timer_);
    ESP_ERROR_CHECK(esp_timer_start_once(preview_timer_, PREVIEW_IMAGE_DURATION_MS * 1000));
}

void StackChanDisplay::SetTheme(Theme* theme) {
    DisplayLockGuard lock(this);
    current_theme_ = theme;
    Settings settings("display", true);
    settings.SetString("theme", theme->name());

    auto lvgl_theme = static_cast<LvglTheme*>(theme);
    auto text_font = lvgl_theme->text_font()->font();
    auto icon_font = lvgl_theme->icon_font()->font();
    lv_obj_t* screen = lv_screen_active();
    lv_obj_set_style_text_font(screen, text_font, 0);
    lv_obj_set_style_text_color(screen, lvgl_theme->text_color(), 0);
    lv_obj_set_style_bg_color(screen, lvgl_theme->background_color(), 0);

    if (container_ != nullptr) {
        lv_obj_set_style_bg_color(container_, lvgl_theme->background_color(), 0);
        lv_obj_set_style_bg_opa(container_, LV_OPA_TRANSP, 0);
    }
    if (top_bar_ != nullptr) {
        lv_obj_set_style_bg_color(top_bar_, lvgl_theme->background_color(), 0);
    }
    if (network_label_ != nullptr) {
        lv_obj_set_style_text_font(network_label_, icon_font, 0);
        lv_obj_set_style_text_color(network_label_, lvgl_theme->text_color(), 0);
    }
    if (status_label_ != nullptr) {
        lv_obj_set_style_text_font(status_label_, text_font, 0);
        lv_obj_set_style_text_color(status_label_, lvgl_theme->text_color(), 0);
    }
    if (notification_label_ != nullptr) {
        lv_obj_set_style_text_font(notification_label_, text_font, 0);
        lv_obj_set_style_text_color(notification_label_, lvgl_theme->text_color(), 0);
    }
    if (mute_label_ != nullptr) {
        lv_obj_set_style_text_font(mute_label_, icon_font, 0);
        lv_obj_set_style_text_color(mute_label_, lvgl_theme->text_color(), 0);
    }
    if (battery_label_ != nullptr) {
        lv_obj_set_style_text_font(battery_label_, icon_font, 0);
        lv_obj_set_style_text_color(battery_label_, lvgl_theme->text_color(), 0);
    }
    ApplyThemeColors();
}

void StackChanDisplay::StartSpeaking() {
    if (speaking_) {
        return;
    }
    speaking_ = true;
    speaking_mouth_open_ = false;
    esp_timer_stop(speaking_timer_);
    ESP_ERROR_CHECK(esp_timer_start_periodic(speaking_timer_, 180 * 1000));
}

void StackChanDisplay::StopSpeaking() {
    if (!speaking_) {
        return;
    }
    speaking_ = false;
    esp_timer_stop(speaking_timer_);
    ApplyMouthWeight(0);
}

void StackChanDisplay::AnimateSpeakingMouth() {
    if (!Lock(0)) {
        return;
    }
    speaking_mouth_open_ = !speaking_mouth_open_;
    ApplyMouthWeight(speaking_mouth_open_ ? 85 : 10);
    Unlock();
}

void StackChanDisplay::UpdateSpeechBubble(const char* content) {
    if (speech_bubble_ == nullptr || speech_label_ == nullptr) {
        return;
    }
    if (content == nullptr || content[0] == '\0') {
        lv_label_set_text(speech_label_, "");
        lv_obj_add_flag(speech_bubble_, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_label_set_text(speech_label_, content);
    lv_point_t text_size;
    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    lv_text_get_size(&text_size, content, lvgl_theme->text_font()->font(), 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    int bubble_width = std::clamp(static_cast<int>(text_size.x) + static_cast<int>(ScaleX(kBubbleTextMargin * 2)),
                                  static_cast<int>(ScaleX(kBubbleMinWidth)),
                                  static_cast<int>(ScaleX(kBubbleMaxWidth)));
    lv_obj_set_width(speech_bubble_, bubble_width);
    lv_obj_set_width(speech_label_, bubble_width - ScaleX(kBubbleTextMargin * 2));
    lv_obj_align(speech_bubble_, LV_ALIGN_BOTTOM_MID, 0, -ScaleY(10));
    lv_obj_remove_flag(speech_bubble_, LV_OBJ_FLAG_HIDDEN);
}

void StackChanDisplay::ApplyThemeColors() {
    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    primary_color_ = lvgl_theme->text_color();
    secondary_color_ = lvgl_theme->background_color();

    if (face_panel_ != nullptr) {
        lv_obj_set_style_bg_color(face_panel_, secondary_color_, 0);
    }
    for (auto* eye : {&left_eye_, &right_eye_}) {
        if (eye->eye != nullptr) {
            lv_obj_set_style_bg_color(eye->eye, primary_color_, 0);
        }
        if (eye->eyelid != nullptr) {
            lv_obj_set_style_bg_color(eye->eyelid, secondary_color_, 0);
        }
    }
    if (mouth_ != nullptr) {
        lv_obj_set_style_bg_color(mouth_, primary_color_, 0);
    }
    if (speech_bubble_ != nullptr) {
        lv_obj_set_style_bg_color(speech_bubble_, primary_color_, 0);
    }
    if (speech_label_ != nullptr) {
        lv_obj_set_style_text_color(speech_label_, secondary_color_, 0);
        lv_obj_set_style_text_font(speech_label_, lvgl_theme->text_font()->font(), 0);
    }
}
