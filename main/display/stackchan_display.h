#ifndef STACKCHAN_DISPLAY_H
#define STACKCHAN_DISPLAY_H

#include "lcd_display.h"

#include <esp_timer.h>

class StackChanDisplay : public SpiLcdDisplay {
public:
    StackChanDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                     int width, int height, int offset_x, int offset_y,
                     bool mirror_x, bool mirror_y, bool swap_xy);
    ~StackChanDisplay();

    void SetupUI() override;
    void SetEmotion(const char* emotion) override;
    void SetChatMessage(const char* role, const char* content) override;
    void ClearChatMessages() override;
    void SetStatus(const char* status) override;
    void ShowNotification(const char* notification, int duration_ms = 3000) override;
    void SetPreviewImage(std::unique_ptr<LvglImage> image) override;
    void SetTheme(Theme* theme) override;

private:
    enum class StackChanEmotion {
        Neutral,
        Happy,
        Angry,
        Sad,
        Doubt,
        Sleepy,
    };

    struct EyeObjects {
        lv_obj_t* container = nullptr;
        lv_obj_t* eye = nullptr;
        lv_obj_t* eyelid = nullptr;
        bool is_left = true;
    };

    lv_obj_t* face_panel_ = nullptr;
    EyeObjects left_eye_;
    EyeObjects right_eye_;
    lv_obj_t* mouth_ = nullptr;
    lv_obj_t* speech_bubble_ = nullptr;
    lv_obj_t* speech_label_ = nullptr;

    esp_timer_handle_t speaking_timer_ = nullptr;
    esp_timer_handle_t notification_hide_timer_ = nullptr;
    bool speaking_ = false;
    bool speaking_mouth_open_ = false;

    lv_color_t primary_color_ = lv_color_white();
    lv_color_t secondary_color_ = lv_color_black();

    static int MapRange(int value, int in_min, int in_max, int out_min, int out_max);
    static StackChanEmotion ParseEmotion(const char* emotion);

    lv_coord_t ScaleX(int value) const;
    lv_coord_t ScaleY(int value) const;
    lv_coord_t ScaleMin(int value) const;

    void CreateStatusBars(lv_obj_t* screen, const lv_font_t* text_font, const lv_font_t* icon_font);
    void CreateAvatar(lv_obj_t* screen, const lv_font_t* text_font);
    void CreateEye(EyeObjects& eye, bool is_left);
    void ApplyEyeStyle(EyeObjects& eye, int weight, int rotation);
    void ApplyMouthWeight(int weight);
    void ApplyEmotion(StackChanEmotion emotion);
    void StartSpeaking();
    void StopSpeaking();
    void AnimateSpeakingMouth();
    void UpdateSpeechBubble(const char* content);
    void ApplyThemeColors();
};

#endif  // STACKCHAN_DISPLAY_H
