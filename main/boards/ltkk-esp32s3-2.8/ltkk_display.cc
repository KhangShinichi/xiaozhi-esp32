#include "ltkk_display.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <wifi_manager.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>

#include "application.h"
#include "assets/lang_config.h"
#include "audio_codec.h"
#include "backlight.h"
#include "board.h"
#include "display/lvgl_display/lvgl_theme.h"
#include "ltkk_brand.h"
#include "ltkk_music.h"
#include "ltkk_ui_images.h"
#include "ltkk_weather.h"
#include "settings.h"

#define TAG "LtkkDisplay"

extern "C" {
extern const lv_image_dsc_t ltkk_background;    // ltkk_background.c
extern const lv_image_dsc_t ltkk_splash_image;  // ltkk_splash_image.c

// Thư viện QR (Nayuki qrcodegen) đã có sẵn trong Xiaozhi qua esp_emote_gfx
bool qrcodegen_encodeText(const char* text, uint8_t temp_buffer[], uint8_t qrcode[], int ecl,
                          int min_version, int max_version, int mask, bool boost_ecl);
int qrcodegen_getSize(const uint8_t qrcode[]);
bool qrcodegen_getModule(const uint8_t qrcode[], int x, int y);
}

namespace {

constexpr uint32_t kCyan = 0x38C8F5;
constexpr uint32_t kSoft = 0xC8DCFF;
constexpr uint32_t kTrack = 0x2A3F75;
constexpr uint32_t kTile = 0x14306E;

const lv_image_dsc_t* const kDigits[10] = {
    &ltkk_digit_0, &ltkk_digit_1, &ltkk_digit_2, &ltkk_digit_3, &ltkk_digit_4,
    &ltkk_digit_5, &ltkk_digit_6, &ltkk_digit_7, &ltkk_digit_8, &ltkk_digit_9,
};
const lv_image_dsc_t* const kWeekdays[7] = {
    &ltkk_wday_0, &ltkk_wday_1, &ltkk_wday_2, &ltkk_wday_3,
    &ltkk_wday_4, &ltkk_wday_5, &ltkk_wday_6,  // Thứ Hai ... Chủ Nhật
};
const lv_image_dsc_t* const kWeather[6] = {
    &ltkk_wx_0, &ltkk_wx_1, &ltkk_wx_2, &ltkk_wx_3, &ltkk_wx_4, &ltkk_wx_5,
};
const lv_image_dsc_t* const kStyleNames[LtkkDisplay::kStandbyStyles] = {
    &ltkk_style_0, &ltkk_style_1, &ltkk_style_2, &ltkk_style_3,
};

struct EmotionMap {
    const char* name;
    const lv_image_dsc_t* eyes;
};
const EmotionMap kEmotions[] = {
    {"neutral", &ltkk_eyes_neutral},    {"happy", &ltkk_eyes_happy},
    {"laughing", &ltkk_eyes_happy},     {"funny", &ltkk_eyes_happy},
    {"delicious", &ltkk_eyes_happy},    {"confident", &ltkk_eyes_happy},
    {"cool", &ltkk_eyes_happy},         {"relaxed", &ltkk_eyes_happy},
    {"embarrassed", &ltkk_eyes_happy},  {"sad", &ltkk_eyes_sad},
    {"crying", &ltkk_eyes_sad},         {"angry", &ltkk_eyes_angry},
    {"loving", &ltkk_eyes_love},        {"kissy", &ltkk_eyes_love},
    {"surprised", &ltkk_eyes_surprise}, {"shocked", &ltkk_eyes_surprise},
    {"thinking", &ltkk_eyes_think},     {"confused", &ltkk_eyes_think},
    {"winking", &ltkk_eyes_wink},       {"silly", &ltkk_eyes_wink},
    {"sleepy", &ltkk_eyes_sleepy},
};

inline int32_t Triangle(int32_t v, int32_t range) { return v <= range ? v : 2 * range - v; }

void WaveExec(void* var, int32_t v) {
    lv_obj_set_height(static_cast<lv_obj_t*>(var), 6 + Triangle(v, 30));
}
void BlinkExec(void* var, int32_t v) {
    lv_image_set_scale_y(static_cast<lv_obj_t*>(var), 256 - Triangle(v, 228));
}
void BreatheExec(void* var, int32_t v) {
    lv_obj_set_style_translate_y(static_cast<lv_obj_t*>(var), Triangle(v, 8) - 4, 0);
}

lv_obj_t* Box(lv_obj_t* parent) {
    lv_obj_t* obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    return obj;
}

lv_obj_t* Row(lv_obj_t* parent, int gap) {
    lv_obj_t* obj = Box(parent);
    lv_obj_set_size(obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(obj, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(obj, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(obj, gap, 0);
    return obj;
}

lv_obj_t* Column(lv_obj_t* parent, int gap) {
    lv_obj_t* obj = Box(parent);
    lv_obj_set_size(obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(obj, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(obj, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(obj, gap, 0);
    return obj;
}

lv_obj_t* Image(lv_obj_t* parent, const lv_image_dsc_t* src) {
    lv_obj_t* img = lv_image_create(parent);
    lv_image_set_src(img, src);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
    return img;
}

lv_obj_t* ScaledImage(lv_obj_t* parent, const lv_image_dsc_t* src, float scale) {
    lv_obj_t* img = Image(parent, src);
    lv_obj_set_size(img, (int32_t)(src->header.w * scale), (int32_t)(src->header.h * scale));
    lv_image_set_inner_align(img, LV_IMAGE_ALIGN_STRETCH);
    return img;
}

lv_obj_t* Label(lv_obj_t* parent, uint32_t color) {
    lv_obj_t* label = lv_label_create(parent);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_label_set_text(label, "");
    return label;
}

void SetHidden(lv_obj_t* obj, bool hidden) {
    if (obj == nullptr) {
        return;
    }
    if (hidden) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

bool IsHidden(lv_obj_t* obj) { return obj == nullptr || lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN); }

}  // namespace

LtkkDisplay::LtkkDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                         int width, int height, int offset_x, int offset_y, bool mirror_x,
                         bool mirror_y, bool swap_xy)
    : SpiLcdDisplay(panel_io, panel, width, height, offset_x, offset_y, mirror_x, mirror_y,
                    swap_xy) {
    Settings settings("ltkk", false);
    int style = settings.GetInt("standby", 0);
    standby_style_ = (style >= 0 && style < kStandbyStyles) ? style : 0;
}

LtkkDisplay::~LtkkDisplay() {
    if (tick_timer_ != nullptr) {
        lv_timer_delete(tick_timer_);
    }
}

void LtkkDisplay::SetupUI() {
    if (setup_ui_called_) {
        return;
    }
    SpiLcdDisplay::SetupUI();  // dựng giao diện gốc của Xiaozhi trước

    DisplayLockGuard lock(this);
    CreateFace();
    CreateWave();
    CreateSubtitle();
    CreateStandby();
    CreateWifiPanel();
    CreateSettings();
    CreateTouch();
    StyleLowBattery();
    tick_timer_ = lv_timer_create(TickCallback, 200, this);
    ESP_LOGI(TAG, "Lu UI ready");
}

void LtkkDisplay::PlaceUnderChrome(lv_obj_t* obj) {
    if (top_bar_ != nullptr && lv_obj_get_parent(top_bar_) == lv_obj_get_parent(obj)) {
        lv_obj_move_to_index(obj, lv_obj_get_index(top_bar_));
    } else {
        lv_obj_move_foreground(obj);
    }
}

// Đánh dấu đối tượng bấm được: cảm ứng tìm đối tượng có cờ USER_1 dưới ngón tay
void LtkkDisplay::MakeInteractive(lv_obj_t* obj) {
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_USER_1);
}

lv_obj_t* LtkkDisplay::MakeButton(lv_obj_t* parent, int w, int h, uint32_t bg, bool outline) {
    lv_obj_t* btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_radius(btn, h / 2 > 12 ? 12 : h / 2, 0);
    if (outline) {
        lv_obj_set_style_border_width(btn, 2, 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_border_opa(btn, LV_OPA_60, 0);
    } else {
        lv_obj_set_style_bg_color(btn, lv_color_hex(bg), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    }
    lv_obj_set_style_bg_color(btn, lv_color_hex(kCyan), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_50, LV_STATE_PRESSED);
    lv_obj_set_ext_click_area(btn, 6);
    MakeInteractive(btn);
    return btn;
}

// ------------------------------------------------------------------ khuôn mặt
void LtkkDisplay::CreateFace() {
    if (emoji_box_ == nullptr) {
        return;  // kiểu tin nhắn WeChat: không có khuôn mặt
    }
    SetHidden(emoji_label_, true);
    SetHidden(emoji_image_, true);
    face_ = lv_image_create(emoji_box_);
    lv_obj_center(face_);
    SetEyes(&ltkk_eyes_neutral);
    speaking_eyes_ = &ltkk_eyes_neutral;
    next_blink_ms_ = lv_tick_get() + 3000;
}

void LtkkDisplay::SetEyes(const lv_image_dsc_t* eyes) {
    if (face_ == nullptr || eyes == current_eyes_) {
        return;
    }
    current_eyes_ = eyes;
    lv_anim_delete(face_, BlinkExec);
    lv_image_set_scale_y(face_, 256);
    lv_image_set_src(face_, eyes);
    lv_obj_set_size(face_, eyes->header.w, eyes->header.h);
    lv_image_set_pivot(face_, eyes->header.w / 2, eyes->header.h / 2);
    blink_enabled_ = (eyes == &ltkk_eyes_neutral || eyes == &ltkk_eyes_listen ||
                      eyes == &ltkk_eyes_think || eyes == &ltkk_eyes_surprise);
}

void LtkkDisplay::Blink() {
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, face_);
    lv_anim_set_exec_cb(&a, BlinkExec);
    lv_anim_set_values(&a, 0, 456);
    lv_anim_set_duration(&a, 220);
    lv_anim_start(&a);
}

bool LtkkDisplay::SetEmotion(const char* emotion) {
    if (face_ == nullptr) {
        return SpiLcdDisplay::SetEmotion(emotion);
    }
    if (emotion == nullptr) {
        return false;
    }
    const lv_image_dsc_t* eyes = &ltkk_eyes_neutral;
    for (const auto& e : kEmotions) {
        if (strcmp(e.name, emotion) == 0) {
            eyes = e.eyes;
            break;
        }
    }
    DisplayLockGuard lock(this);
    speaking_eyes_ = eyes;
    auto state = Application::GetInstance().GetDeviceState();
    if (state != kDeviceStateListening && state != kDeviceStateConnecting) {
        SetEyes(eyes);
    }
    return true;
}

// ------------------------------------------------------------------ sóng âm
void LtkkDisplay::CreateWave() {
    lv_obj_t* screen = lv_screen_active();
    wave_box_ = Box(screen);
    lv_obj_set_size(wave_box_, LV_PCT(100), 40);
    lv_obj_align(wave_box_, LV_ALIGN_CENTER, 0, 34);
    lv_obj_set_flex_flow(wave_box_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(wave_box_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(wave_box_, 5, 0);
    for (int i = 0; i < kWaveBars; i++) {
        lv_obj_t* bar = Box(wave_box_);
        lv_obj_set_size(bar, 6, 10);
        lv_obj_set_style_radius(bar, 3, 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(bar, lv_color_hex(i % 3 == 0 ? LTKK_COLOR_ORANGE : kCyan), 0);
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, bar);
        lv_anim_set_exec_cb(&a, WaveExec);
        lv_anim_set_values(&a, 0, 60);
        lv_anim_set_duration(&a, 520 + (i * 137) % 420);
        lv_anim_set_delay(&a, (i * 89) % 400);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_start(&a);
    }
    listening_label_ = Image(screen, &ltkk_txt_listening);
    lv_obj_align(listening_label_, LV_ALIGN_CENTER, 0, 72);
    PlaceUnderChrome(wave_box_);
    PlaceUnderChrome(listening_label_);
    SetHidden(wave_box_, true);
    SetHidden(listening_label_, true);
}

// ------------------------------------------------------------------ phụ đề
void LtkkDisplay::CreateSubtitle() {
    if (bottom_bar_ == nullptr || chat_message_label_ == nullptr) {
        return;
    }
    lv_obj_set_width(bottom_bar_, LV_HOR_RES - 16);
    lv_obj_align(bottom_bar_, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_set_style_radius(bottom_bar_, 14, 0);
    lv_obj_set_style_bg_color(bottom_bar_, lv_color_hex(LTKK_COLOR_NAVY), 0);
    lv_obj_set_style_bg_opa(bottom_bar_, 210, 0);
    lv_obj_set_style_border_width(bottom_bar_, 1, 0);
    lv_obj_set_style_border_color(bottom_bar_, lv_color_hex(kCyan), 0);
    lv_obj_set_style_border_opa(bottom_bar_, LV_OPA_50, 0);
    lv_obj_set_style_pad_hor(bottom_bar_, 12, 0);
    lv_obj_set_style_pad_ver(bottom_bar_, 6, 0);
    lv_obj_set_style_text_color(chat_message_label_, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_width(chat_message_label_, LV_HOR_RES - 16 - 24);
#if CONFIG_USE_MULTILINE_CHAT_MESSAGE
    chat_tag_ = Image(bottom_bar_, &ltkk_tag_lu);
    lv_obj_add_flag(chat_tag_, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_align(chat_tag_, LV_ALIGN_TOP_LEFT, 0, -13);
    SetHidden(chat_tag_, true);
#endif
}

void LtkkDisplay::SetChatMessage(const char* role, const char* content) {
    SpiLcdDisplay::SetChatMessage(role, content);
    if (chat_tag_ == nullptr || role == nullptr) {
        return;
    }
    DisplayLockGuard lock(this);
    bool assistant = strcmp(role, "assistant") == 0;
    bool user = strcmp(role, "user") == 0;
    if (assistant || user) {
        lv_image_set_src(chat_tag_, assistant ? &ltkk_tag_lu : &ltkk_tag_user);
        SetHidden(chat_tag_, false);
        lv_obj_set_style_pad_top(bottom_bar_, 16, 0);
    } else {
        SetHidden(chat_tag_, true);
        lv_obj_set_style_pad_top(bottom_bar_, 6, 0);
    }
}

void LtkkDisplay::SetTheme(Theme* theme) {
    SpiLcdDisplay::SetTheme(theme);
    DisplayLockGuard lock(this);
    StyleLowBattery();
    if (bottom_bar_ != nullptr && chat_message_label_ != nullptr) {
        lv_obj_set_style_bg_color(bottom_bar_, lv_color_hex(LTKK_COLOR_NAVY), 0);
        lv_obj_set_style_bg_opa(bottom_bar_, 210, 0);
        lv_obj_set_style_text_color(chat_message_label_, lv_color_hex(0xFFFFFF), 0);
    }
    if (settings_ != nullptr) {
        RefreshSettings();
    }
}

void LtkkDisplay::StyleLowBattery() {
    if (low_battery_popup_ == nullptr) {
        return;
    }
    lv_obj_set_style_bg_color(low_battery_popup_, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(low_battery_popup_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(low_battery_popup_, 14, 0);
    lv_obj_set_style_border_width(low_battery_popup_, 3, 0);
    lv_obj_set_style_border_side(low_battery_popup_, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_color(low_battery_popup_, lv_color_hex(LTKK_COLOR_ORANGE), 0);
    if (low_battery_label_ != nullptr) {
        lv_obj_set_style_text_color(low_battery_label_, lv_color_hex(LTKK_COLOR_NAVY), 0);
    }
}

// ------------------------------------------------------------------ màn hình chờ
void LtkkDisplay::CreateStandby() {
    standby_ = Box(lv_screen_active());
    lv_obj_set_size(standby_, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(standby_, lv_color_hex(LTKK_COLOR_NAVY), 0);
    lv_obj_set_style_bg_opa(standby_, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_image_src(standby_, &ltkk_background, 0);

    for (int i = 0; i < kStandbyStyles; i++) {
        style_panels_[i] = Box(standby_);
        lv_obj_set_size(style_panels_[i], LV_PCT(100), LV_PCT(100));
        SetHidden(style_panels_[i], true);
    }
    CreateStyleClock(style_panels_[0]);
    CreateStyleAnalog(style_panels_[1]);
    CreateStyleSleepy(style_panels_[2]);
    CreateStyleCard(style_panels_[3]);
    CreateTimer(standby_);
    CreateMusic(standby_);

    timeup_img_ = Image(standby_, &ltkk_txt_timeup);
    lv_obj_align(timeup_img_, LV_ALIGN_TOP_MID, 0, 34);
    SetHidden(timeup_img_, true);

    // Gợi ý gọi Lu (góc dưới trái)
    hint_pill_ = Box(standby_);
    lv_obj_set_size(hint_pill_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(hint_pill_, lv_color_hex(LTKK_COLOR_ORANGE), 0);
    lv_obj_set_style_bg_opa(hint_pill_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(hint_pill_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_hor(hint_pill_, 12, 0);
    lv_obj_set_style_pad_ver(hint_pill_, 7, 0);
    lv_obj_align(hint_pill_, LV_ALIGN_BOTTOM_LEFT, 10, -10);
    Image(hint_pill_, &ltkk_txt_hint);

    // Nút cài đặt (góc dưới phải) - luôn có trên màn hình chờ
    gear_btn_ = MakeButton(standby_, 42, 42, kTile, false);
    lv_obj_set_style_radius(gear_btn_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(gear_btn_, 2, 0);
    lv_obj_set_style_border_color(gear_btn_, lv_color_hex(kCyan), 0);
    lv_obj_set_style_border_opa(gear_btn_, LV_OPA_60, 0);
    lv_obj_set_ext_click_area(gear_btn_, 12);
    lv_obj_align(gear_btn_, LV_ALIGN_BOTTOM_RIGHT, -10, -8);
    lv_obj_center(Image(gear_btn_, &ltkk_icon_gear_btn));

    // Thanh màu thương hiệu ở mép dưới
    lv_obj_t* bar = Box(standby_);
    lv_obj_set_size(bar, LV_PCT(100), 3);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(LTKK_COLOR_BLUE), 0);
    lv_obj_set_style_bg_grad_color(bar, lv_color_hex(LTKK_COLOR_ORANGE), 0);
    lv_obj_set_style_bg_grad_dir(bar, LV_GRAD_DIR_HOR, 0);

    PlaceUnderChrome(standby_);
    SetHidden(standby_, true);
}

lv_obj_t* LtkkDisplay::CreateDigits(lv_obj_t* parent, lv_obj_t** slots, float scale) {
    lv_obj_t* row = Row(parent, 0);
    for (int i = 0; i < 5; i++) {
        const lv_image_dsc_t* src = i == 2 ? &ltkk_digit_colon : &ltkk_digit_0;
        slots[i] = scale >= 0.99f ? Image(row, src) : ScaledImage(row, src, scale);
    }
    return row;
}

// centered = thứ và ngày cùng một dòng (kiểu căn giữa); false = xếp 2 dòng (cột bên phải)
void LtkkDisplay::CreateInfo(lv_obj_t* parent, InfoWidgets& info, bool centered) {
    lv_obj_t* date_row = centered ? Row(parent, 6) : Column(parent, 2);
    info.weekday = Image(date_row, kWeekdays[0]);
    info.date = Label(date_row, kSoft);

    info.weather = Row(parent, 5);
    lv_obj_set_style_bg_color(info.weather, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(info.weather, 26, 0);
    lv_obj_set_style_radius(info.weather, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_hor(info.weather, 9, 0);
    lv_obj_set_style_pad_ver(info.weather, 3, 0);
    lv_obj_set_style_margin_top(info.weather, 4, 0);
    Image(info.weather, &ltkk_icon_cloud);
    info.weather_img = Image(info.weather, kWeather[1]);
    info.weather_temp = Label(info.weather, 0xFFFFFF);
    Image(info.weather, &ltkk_txt_degc);
    SetHidden(info.weather, true);
}

// Kiểu 0: Đồng hồ số lớn
void LtkkDisplay::CreateStyleClock(lv_obj_t* panel) {
    lv_obj_t* col = Column(panel, 4);
    lv_obj_set_width(col, LV_PCT(100));
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_align(col, LV_ALIGN_CENTER, 0, -12);
    CreateDigits(col, clock_digits_[0], 1.0f);
    CreateInfo(col, info_[0], true);
}

// Kiểu 1: Đồng hồ kim với vòng màu LTKK (xanh -> cam)
void LtkkDisplay::CreateStyleAnalog(lv_obj_t* panel) {
    constexpr int kDial = 146;
    lv_obj_t* dial = Box(panel);
    lv_obj_set_size(dial, kDial, kDial);
    lv_obj_align(dial, LV_ALIGN_LEFT_MID, 10, 6);
    lv_obj_set_style_radius(dial, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dial, lv_color_hex(0x081738), 0);
    lv_obj_set_style_bg_opa(dial, 200, 0);

    lv_obj_t* ring = lv_arc_create(dial);
    lv_obj_set_size(ring, kDial, kDial);
    lv_obj_center(ring);
    lv_obj_remove_style(ring, nullptr, LV_PART_KNOB);
    lv_obj_remove_flag(ring, LV_OBJ_FLAG_CLICKABLE);
    lv_arc_set_bg_angles(ring, 0, 360);
    lv_arc_set_rotation(ring, 300);
    lv_arc_set_angles(ring, 0, 110);
    lv_obj_set_style_arc_width(ring, 5, LV_PART_MAIN);
    lv_obj_set_style_arc_width(ring, 5, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(ring, lv_color_hex(LTKK_COLOR_BLUE), LV_PART_MAIN);
    lv_obj_set_style_arc_color(ring, lv_color_hex(LTKK_COLOR_ORANGE), LV_PART_INDICATOR);

    const int c = kDial / 2;
    for (int i = 0; i < 12; i++) {
        double a = i * M_PI / 6;
        int r = (i % 3 == 0) ? 4 : 2;
        lv_obj_t* tick = Box(dial);
        lv_obj_set_size(tick, r * 2, r * 2);
        lv_obj_set_style_radius(tick, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(tick, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(tick, lv_color_hex(i % 3 == 0 ? kCyan : kSoft), 0);
        lv_obj_set_pos(tick, c + (int)(58 * sin(a)) - r, c - (int)(58 * cos(a)) - r);
    }

    auto hand = [&](int width, uint32_t color) {
        lv_obj_t* line = lv_line_create(dial);
        lv_obj_set_style_line_width(line, width, 0);
        lv_obj_set_style_line_color(line, lv_color_hex(color), 0);
        lv_obj_set_style_line_rounded(line, true, 0);
        lv_obj_set_pos(line, 0, 0);
        lv_obj_set_size(line, kDial, kDial);
        return line;
    };
    analog_hour_ = hand(6, 0xFFFFFF);
    analog_minute_ = hand(4, kCyan);
    analog_second_ = hand(2, LTKK_COLOR_ORANGE);
    lv_obj_t* hub = Box(dial);
    lv_obj_set_size(hub, 12, 12);
    lv_obj_set_style_radius(hub, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(hub, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(hub, lv_color_hex(LTKK_COLOR_ORANGE), 0);
    lv_obj_center(hub);

    lv_obj_t* right = Column(panel, 6);
    lv_obj_align(right, LV_ALIGN_LEFT_MID, 170, 4);
    CreateDigits(right, clock_digits_[1], 0.48f);
    CreateInfo(right, info_[1], false);
}

void LtkkDisplay::UpdateAnalog(const struct tm& t) {
    if (analog_hour_ == nullptr) {
        return;
    }
    const lv_value_precise_t c = 73;
    auto set = [&](lv_obj_t* line, int idx, double angle, float len) {
        hand_points_[idx][0] = {c, c};
        hand_points_[idx][1] = {(lv_value_precise_t)(73 + len * sin(angle)),
                                (lv_value_precise_t)(73 - len * cos(angle))};
        lv_line_set_points(line, hand_points_[idx], 2);
    };
    double sec = t.tm_sec;
    double min = t.tm_min + sec / 60.0;
    double hour = (t.tm_hour % 12) + min / 60.0;
    set(analog_hour_, 0, hour * M_PI / 6, 34);
    set(analog_minute_, 1, min * M_PI / 30, 50);
    set(analog_second_, 2, sec * M_PI / 30, 54);
}

// Kiểu 2: Lu ngủ - đôi mắt thở nhẹ, giờ nhỏ bên dưới
void LtkkDisplay::CreateStyleSleepy(lv_obj_t* panel) {
    sleepy_face_ = Image(panel, &ltkk_eyes_sleepy);
    lv_obj_align(sleepy_face_, LV_ALIGN_CENTER, 0, -44);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, sleepy_face_);
    lv_anim_set_exec_cb(&a, BreatheExec);
    lv_anim_set_values(&a, 0, 16);
    lv_anim_set_duration(&a, 3200);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);

    lv_obj_t* col = Column(panel, 4);
    lv_obj_set_width(col, LV_PCT(100));
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_align(col, LV_ALIGN_CENTER, 0, 22);
    CreateDigits(col, clock_digits_[2], 0.5f);
    CreateInfo(col, info_[2], true);
    SetHidden(info_[2].weather, true);
    info_[2].weather = nullptr;  // kiểu Lu ngủ: gọn, không hiện thời tiết
}

// Kiểu 3: Thẻ LTKK - logo "Học để làm" + giờ
void LtkkDisplay::CreateStyleCard(lv_obj_t* panel) {
    lv_obj_t* logo = ScaledImage(panel, &ltkk_splash_image, 0.62f);
    lv_obj_align(logo, LV_ALIGN_LEFT_MID, 8, 4);
    lv_obj_t* right = Column(panel, 6);
    lv_obj_align(right, LV_ALIGN_LEFT_MID, 156, 4);
    CreateDigits(right, clock_digits_[3], 0.55f);
    CreateInfo(right, info_[3], false);
}

void LtkkDisplay::SetDigits(lv_obj_t* const* slots, int a, int b) {
    if (slots[0] == nullptr) {
        return;
    }
    lv_image_set_src(slots[0], kDigits[(a / 10) % 10]);
    lv_image_set_src(slots[1], kDigits[a % 10]);
    lv_image_set_src(slots[3], kDigits[(b / 10) % 10]);
    lv_image_set_src(slots[4], kDigits[b % 10]);
}

void LtkkDisplay::SetStandbyStyle(int style) {
    style = ((style % kStandbyStyles) + kStandbyStyles) % kStandbyStyles;
    standby_style_ = style;
    Settings settings("ltkk", true);
    settings.SetInt("standby", style);
}

void LtkkDisplay::ShowStandbyStyle(int style) {
    for (int i = 0; i < kStandbyStyles; i++) {
        SetHidden(style_panels_[i], i != style);
    }
    if (style != shown_style_) {
        shown_style_ = style;
        last_minute_ = -1;
        last_second_ = -1;
        last_weather_key_ = -1000;
        UpdateClock(true);
        UpdateWeather();
    }
}

void LtkkDisplay::UpdateClock(bool force) {
    time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);
    bool valid = (t.tm_year + 1900) >= 2025;  // đã đồng bộ giờ từ máy chủ
    int style = shown_style_ < 0 ? 0 : shown_style_;
    if (!valid) {
        SetHidden(style_panels_[style], true);
        return;
    }
    SetHidden(style_panels_[style], false);

    if (style == 1 && (force || t.tm_sec != last_second_)) {
        last_second_ = t.tm_sec;
        UpdateAnalog(t);
    }
    int minute_key = t.tm_hour * 60 + t.tm_min;
    if (!force && minute_key == last_minute_) {
        return;
    }
    last_minute_ = minute_key;
    SetDigits(clock_digits_[style], t.tm_hour, t.tm_min);
    InfoWidgets& info = info_[style];
    lv_image_set_src(info.weekday, kWeekdays[t.tm_wday == 0 ? 6 : t.tm_wday - 1]);
    lv_label_set_text_fmt(info.date, "%02d/%02d/%04d", t.tm_mday, t.tm_mon + 1, t.tm_year + 1900);
}

void LtkkDisplay::UpdateWeather() {
    auto& weather = LtkkWeather::GetInstance();
    InfoWidgets& info = info_[shown_style_ < 0 ? 0 : shown_style_];
    if (info.weather == nullptr) {
        return;
    }
    if (!weather.valid()) {
        SetHidden(info.weather, true);
        return;
    }
    int cond = weather.condition();
    int temp = weather.temperature();
    int key = cond * 1000 + temp;
    if (key != last_weather_key_) {
        last_weather_key_ = key;
        lv_image_set_src(info.weather_img, kWeather[cond >= 0 && cond < 6 ? cond : 1]);
        lv_label_set_text_fmt(info.weather_temp, "%d", temp);
    }
    SetHidden(info.weather, false);
}

// ------------------------------------------------------------------ hẹn giờ
void LtkkDisplay::CreateTimer(lv_obj_t* parent) {
    timer_panel_ = Box(parent);
    lv_obj_set_size(timer_panel_, LV_PCT(100), LV_PCT(100));

    timer_arc_ = lv_arc_create(timer_panel_);
    lv_obj_set_size(timer_arc_, 128, 128);
    lv_obj_align(timer_arc_, LV_ALIGN_LEFT_MID, 16, 0);
    lv_arc_set_rotation(timer_arc_, 270);
    lv_arc_set_bg_angles(timer_arc_, 0, 360);
    lv_arc_set_range(timer_arc_, 0, 1000);
    lv_arc_set_value(timer_arc_, 1000);
    lv_obj_remove_style(timer_arc_, nullptr, LV_PART_KNOB);
    lv_obj_remove_flag(timer_arc_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(timer_arc_, 10, LV_PART_MAIN);
    lv_obj_set_style_arc_width(timer_arc_, 10, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(timer_arc_, lv_color_hex(kTrack), LV_PART_MAIN);
    lv_obj_set_style_arc_color(timer_arc_, lv_color_hex(LTKK_COLOR_ORANGE), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(timer_arc_, true, LV_PART_INDICATOR);

    lv_obj_t* center = Column(timer_arc_, 2);
    lv_obj_set_flex_align(center, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_center(center);
    CreateDigits(center, timer_digits_, 0.42f);
    Image(center, &ltkk_txt_remaining);

    lv_obj_t* right = Column(timer_panel_, 8);
    lv_obj_align(right, LV_ALIGN_LEFT_MID, 158, -12);
    lv_obj_t* title = Row(right, 6);
    Image(title, &ltkk_icon_timer);
    Image(title, &ltkk_txt_timer);
    Image(right, &ltkk_txt_timer_sub);
    lv_obj_t* buttons = Row(right, 8);
    lv_obj_set_style_margin_top(buttons, 6, 0);
    timer_pause_btn_ = MakeButton(buttons, 86, 36, 0xFFFFFF, false);
    timer_pause_img_ = Image(timer_pause_btn_, &ltkk_btn_pause);
    lv_obj_center(timer_pause_img_);
    timer_cancel_btn_ = MakeButton(buttons, 58, 36, 0, true);
    lv_obj_center(Image(timer_cancel_btn_, &ltkk_btn_cancel));
    SetHidden(timer_panel_, true);
}

void LtkkDisplay::StartStudyTimer(int minutes) {
    int64_t total = (int64_t)minutes * 60 * 1000000LL;
    timer_total_us_ = total;
    timer_paused_left_us_ = 0;
    timer_end_us_ = esp_timer_get_time() + total;
}

void LtkkDisplay::CancelStudyTimer() {
    timer_end_us_ = 0;
    timer_paused_left_us_ = 0;
}

void LtkkDisplay::PauseResumeStudyTimer() {
    int64_t paused = timer_paused_left_us_.load();
    if (paused > 0) {
        timer_end_us_ = esp_timer_get_time() + paused;
        timer_paused_left_us_ = 0;
    } else {
        int64_t end = timer_end_us_.load();
        if (end > 0) {
            int64_t left = end - esp_timer_get_time();
            timer_paused_left_us_ = left > 0 ? left : 1;
            timer_end_us_ = 0;
        }
    }
}

bool LtkkDisplay::IsStudyTimerRunning() const {
    return timer_end_us_.load() > 0 || timer_paused_left_us_.load() > 0;
}

int LtkkDisplay::StudyTimerRemainingSeconds() const {
    int64_t paused = timer_paused_left_us_.load();
    if (paused > 0) {
        return (int)((paused + 999999) / 1000000);
    }
    int64_t end = timer_end_us_.load();
    if (end <= 0) {
        return 0;
    }
    int64_t left = end - esp_timer_get_time();
    return left > 0 ? (int)((left + 999999) / 1000000) : 0;
}

void LtkkDisplay::UpdateTimer(int64_t now_us) {
    int64_t end = timer_end_us_.load();
    if (end > 0 && now_us >= end) {
        timer_end_us_ = 0;
        timeup_until_us_ = now_us + 15 * 1000000LL;
        SetEyes(&ltkk_eyes_happy);
        if (!LtkkMusic::GetInstance().IsActive()) {
            Application::GetInstance().Schedule([]() {
                Application::GetInstance().PlaySound(Lang::Sounds::OGG_POPUP);
            });
        }
    }
    SetHidden(timeup_img_, now_us >= timeup_until_us_);
    if (!IsStudyTimerRunning()) {
        last_timer_sec_ = -1;
        return;
    }
    int left = StudyTimerRemainingSeconds();
    bool paused = timer_paused_left_us_.load() > 0;
    if (left != last_timer_sec_ || paused != last_timer_paused_) {
        last_timer_sec_ = left;
        last_timer_paused_ = paused;
        int mm = left / 60 > 99 ? 99 : left / 60;
        SetDigits(timer_digits_, mm, left % 60);
        int64_t total = timer_total_us_.load();
        int64_t left_us = (int64_t)left * 1000000LL;
        lv_arc_set_value(timer_arc_, total > 0 ? (int32_t)(left_us * 1000 / total) : 0);
        lv_image_set_src(timer_pause_img_, paused ? &ltkk_btn_resume : &ltkk_btn_pause);
    }
}

// ------------------------------------------------------------------ nhạc / radio
void LtkkDisplay::CreateMusic(lv_obj_t* parent) {
    music_panel_ = Box(parent);
    lv_obj_set_size(music_panel_, LV_PCT(100), LV_PCT(100));

    lv_obj_t* art = Box(music_panel_);
    lv_obj_set_size(art, 96, 96);
    lv_obj_align(art, LV_ALIGN_TOP_LEFT, 16, 38);
    lv_obj_set_style_radius(art, 18, 0);
    lv_obj_set_style_bg_opa(art, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(art, lv_color_hex(LTKK_COLOR_BLUE), 0);
    lv_obj_set_style_bg_grad_color(art, lv_color_hex(LTKK_COLOR_ORANGE), 0);
    lv_obj_set_style_bg_grad_dir(art, LV_GRAD_DIR_VER, 0);
    music_art_icon_ = Image(art, &ltkk_icon_note);
    lv_obj_center(music_art_icon_);

    lv_obj_t* info = Column(music_panel_, 6);
    lv_obj_align(info, LV_ALIGN_TOP_LEFT, 126, 46);
    music_kind_img_ = Image(info, &ltkk_txt_music);
    music_title_ = Label(info, 0xFFFFFF);
    lv_obj_set_width(music_title_, 180);
    lv_label_set_long_mode(music_title_, LV_LABEL_LONG_MODE_DOTS);
    music_sub_img_ = Image(info, &ltkk_txt_sdcard);
    music_bar_ = lv_bar_create(info);
    lv_obj_set_size(music_bar_, 180, 6);
    lv_bar_set_range(music_bar_, 0, 1000);
    lv_obj_set_style_bg_color(music_bar_, lv_color_hex(kTrack), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(music_bar_, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(music_bar_, lv_color_hex(LTKK_COLOR_ORANGE), LV_PART_INDICATOR);
    lv_obj_set_style_margin_top(music_bar_, 6, 0);

    lv_obj_t* controls = Row(music_panel_, 18);
    lv_obj_align(controls, LV_ALIGN_BOTTOM_MID, -24, -12);
    music_prev_btn_ = MakeButton(controls, 44, 44, kTile, false);
    music_play_btn_ = MakeButton(controls, 56, 56, LTKK_COLOR_ORANGE, false);
    music_next_btn_ = MakeButton(controls, 44, 44, kTile, false);
    music_stop_btn_ = MakeButton(controls, 44, 44, 0, true);
    for (lv_obj_t* b : {music_prev_btn_, music_play_btn_, music_next_btn_, music_stop_btn_}) {
        lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    }
    lv_obj_center(Image(music_prev_btn_, &ltkk_icon_prev));
    music_play_img_ = Image(music_play_btn_, &ltkk_icon_pause);
    lv_obj_center(music_play_img_);
    lv_obj_center(Image(music_next_btn_, &ltkk_icon_next));
    lv_obj_center(Image(music_stop_btn_, &ltkk_icon_close));
    SetHidden(music_panel_, true);
}

void LtkkDisplay::UpdateMusic() {
    auto& music = LtkkMusic::GetInstance();
    // 0 = thẻ nhớ, 1 = radio, 2 = bài hát trực tuyến; +10 khi đang kết nối
    int mode = music.IsRadio() ? 1 : (music.IsSong() ? 2 : 0);
    int key = mode + (music.IsBuffering() ? 10 : 0);
    if (key != music_mode_shown_) {
        music_mode_shown_ = key;
        lv_image_set_src(music_kind_img_, mode == 0   ? &ltkk_txt_music
                                          : mode == 1 ? &ltkk_txt_radio
                                                      : &ltkk_txt_online);
        lv_image_set_src(music_sub_img_, music.IsBuffering() ? &ltkk_txt_connecting
                                         : mode == 0         ? &ltkk_txt_sdcard
                                         : mode == 1         ? &ltkk_txt_live
                                                             : &ltkk_txt_server);
        lv_image_set_src(music_art_icon_, mode == 1 ? &ltkk_icon_radio : &ltkk_icon_note);
        lv_image_set_scale(music_art_icon_, mode == 1 ? 420 : 256);
    }
    std::string title = music.CurrentTitle();
    if (title != music_title_text_) {
        music_title_text_ = title;
        lv_label_set_text(music_title_, title.c_str());
    }
    lv_bar_set_value(music_bar_, mode == 0 ? music.ProgressPermille() : 1000, LV_ANIM_OFF);
    bool paused = music.IsPaused();
    if (paused != music_paused_shown_) {
        music_paused_shown_ = paused;
        lv_image_set_src(music_play_img_, paused ? &ltkk_icon_play : &ltkk_icon_pause);
    }
}

// ------------------------------------------------------------------ cài Wi-Fi
void LtkkDisplay::CreateWifiPanel() {
    wifi_panel_ = Box(lv_screen_active());
    lv_obj_set_size(wifi_panel_, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(wifi_panel_, lv_color_hex(LTKK_COLOR_NAVY), 0);
    lv_obj_set_style_bg_opa(wifi_panel_, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_image_src(wifi_panel_, &ltkk_background, 0);

    lv_obj_t* left = Column(wifi_panel_, 6);
    lv_obj_set_flex_align(left, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_align(left, LV_ALIGN_LEFT_MID, 12, 0);
    lv_obj_t* qr_box = Box(left);
    lv_obj_set_size(qr_box, 116, 116);
    lv_obj_set_style_bg_color(qr_box, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(qr_box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(qr_box, 12, 0);
    wifi_qr_ = lv_image_create(qr_box);
    lv_obj_center(wifi_qr_);
    Image(left, &ltkk_txt_wifi_scan);

    lv_obj_t* right = Column(wifi_panel_, 9);
    lv_obj_align(right, LV_ALIGN_LEFT_MID, 142, 0);
    lv_obj_t* title = Row(right, 6);
    Image(title, &ltkk_icon_wifi);
    Image(title, &ltkk_txt_wifi_title);

    auto step = [&](const char* num, const lv_image_dsc_t* text, lv_obj_t** value_label,
                    const lv_image_dsc_t* sub) {
        lv_obj_t* row = Row(right, 8);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_START);
        lv_obj_t* dot = Box(row);
        lv_obj_set_size(dot, 22, 22);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(LTKK_COLOR_ORANGE), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_t* n = Label(dot, 0xFFFFFF);
        lv_label_set_text(n, num);
        lv_obj_center(n);
        lv_obj_t* col = Column(row, 1);
        Image(col, text);
        if (value_label != nullptr) {
            *value_label = Label(col, kCyan);
            lv_obj_set_width(*value_label, 140);
            lv_label_set_long_mode(*value_label, LV_LABEL_LONG_MODE_DOTS);
        }
        if (sub != nullptr) {
            Image(col, sub);
        }
    };
    step("1", &ltkk_txt_wifi_s1, &wifi_ssid_label_, nullptr);
    step("2", &ltkk_txt_wifi_s2, &wifi_url_label_, nullptr);
    step("3", &ltkk_txt_wifi_s3, nullptr, &ltkk_txt_wifi_s3b);

    lv_obj_move_foreground(wifi_panel_);
    SetHidden(wifi_panel_, true);
}

void LtkkDisplay::ShowWifiPanel(bool show) {
    if (!show) {
        SetHidden(wifi_panel_, true);
        return;
    }
    auto& wifi = WifiManager::GetInstance();
    std::string ssid = wifi.GetApSsid();
    if (ssid.empty()) {
        return;  // BluFi: dùng hướng dẫn mặc định của Xiaozhi
    }
    lv_label_set_text(wifi_ssid_label_, ssid.c_str());
    lv_label_set_text(wifi_url_label_, wifi.GetApWebUrl().c_str());
    DrawQrCode("WIFI:T:nopass;S:" + ssid + ";;");
    lv_obj_move_foreground(wifi_panel_);
    SetHidden(wifi_panel_, false);
}

void LtkkDisplay::DrawQrCode(const std::string& text) {
    if (qr_pixels_ != nullptr && text == qr_text_) {
        return;
    }
    qr_text_ = text;
    constexpr int kBufLen = 3918;
    std::unique_ptr<uint8_t[]> temp(new uint8_t[kBufLen]);
    std::unique_ptr<uint8_t[]> qr(new uint8_t[kBufLen]);
    if (!qrcodegen_encodeText(text.c_str(), temp.get(), qr.get(), 1, 1, 10, -1, true)) {
        ESP_LOGW(TAG, "QR encode failed");
        return;
    }
    int modules = qrcodegen_getSize(qr.get());
    int scale = 104 / modules;
    if (scale < 1) {
        scale = 1;
    }
    int side = modules * scale;
    if (qr_pixels_ != nullptr) {
        lv_image_set_src(wifi_qr_, nullptr);
        free(qr_pixels_);
    }
    qr_pixels_ = (uint8_t*)heap_caps_malloc(side * side * 2, MALLOC_CAP_SPIRAM);
    if (qr_pixels_ == nullptr) {
        return;
    }
    uint16_t dark = lv_color_to_u16(lv_color_hex(LTKK_COLOR_NAVY));
    uint16_t* px = reinterpret_cast<uint16_t*>(qr_pixels_);
    for (int y = 0; y < side; y++) {
        for (int x = 0; x < side; x++) {
            px[y * side + x] = qrcodegen_getModule(qr.get(), x / scale, y / scale) ? dark : 0xFFFF;
        }
    }
    qr_dsc_ = {};
    qr_dsc_.header.magic = LV_IMAGE_HEADER_MAGIC;
    qr_dsc_.header.cf = LV_COLOR_FORMAT_RGB565;
    qr_dsc_.header.w = side;
    qr_dsc_.header.h = side;
    qr_dsc_.header.stride = side * 2;
    qr_dsc_.data_size = side * side * 2;
    qr_dsc_.data = qr_pixels_;
    lv_image_set_src(wifi_qr_, &qr_dsc_);
    lv_obj_center(wifi_qr_);
}

// ------------------------------------------------------------------ cài đặt
void LtkkDisplay::CreateSettings() {
    settings_ = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(settings_);
    lv_obj_remove_flag(settings_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(settings_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(settings_, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(settings_, lv_color_hex(0x081738), 0);
    lv_obj_set_style_bg_opa(settings_, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_top(settings_, 30, 0);
    lv_obj_set_style_pad_hor(settings_, 8, 0);
    lv_obj_set_style_pad_bottom(settings_, 6, 0);
    lv_obj_set_flex_flow(settings_, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(settings_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(settings_, 6, 0);
    lv_obj_set_style_pad_column(settings_, 6, 0);
    MakeInteractive(settings_);  // chạm vào vùng trống của trang cài đặt: không bật trò chuyện

    // Tiêu đề + nút đóng
    lv_obj_t* head = Box(settings_);
    lv_obj_set_size(head, LV_PCT(100), 30);
    lv_obj_t* title = Row(head, 6);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 2, 0);
    Image(title, &ltkk_icon_gear);
    Image(title, &ltkk_txt_settings);
    set_close_btn_ = MakeButton(head, 40, 30, kTile, false);
    lv_obj_align(set_close_btn_, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_center(Image(set_close_btn_, &ltkk_icon_close));

    auto tile = [&](const lv_image_dsc_t* icon, const lv_image_dsc_t* title_img, bool clickable) {
        lv_obj_t* t = clickable ? MakeButton(settings_, 96, 84, kTile, false) : Box(settings_);
        lv_obj_set_size(t, 96, 84);
        lv_obj_set_style_radius(t, 12, 0);
        lv_obj_set_style_bg_color(t, lv_color_hex(kTile), 0);
        lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(t, 6, 0);
        lv_obj_t* top = Row(t, 4);
        lv_obj_align(top, LV_ALIGN_TOP_LEFT, 0, 0);
        Image(top, icon);
        lv_obj_t* name = Image(t, title_img);
        lv_obj_align(name, LV_ALIGN_TOP_LEFT, 0, 30);
        return t;
    };
    auto stepper = [&](lv_obj_t* t, lv_obj_t** minus, lv_obj_t** plus, lv_obj_t** value) {
        *value = Label(t, LTKK_COLOR_ORANGE);
        lv_obj_align(*value, LV_ALIGN_TOP_RIGHT, 0, 4);
        *minus = MakeButton(t, 38, 26, 0x2A4A8F, false);
        lv_obj_align(*minus, LV_ALIGN_BOTTOM_LEFT, 0, 0);
        lv_obj_center(Image(*minus, &ltkk_icon_minus));
        *plus = MakeButton(t, 38, 26, LTKK_COLOR_ORANGE, false);
        lv_obj_align(*plus, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
        lv_obj_center(Image(*plus, &ltkk_icon_plus));
    };

    lv_obj_t* vol = tile(&ltkk_icon_vol_up, &ltkk_txt_vol_s, false);
    stepper(vol, &set_vol_minus_, &set_vol_plus_, &set_vol_label_);
    lv_obj_t* bri = tile(&ltkk_icon_sun, &ltkk_txt_bri_s, false);
    stepper(bri, &set_bri_minus_, &set_bri_plus_, &set_bri_label_);

    set_theme_tile_ = tile(&ltkk_icon_moon, &ltkk_txt_theme_s, true);
    set_theme_icon_ = lv_obj_get_child(lv_obj_get_child(set_theme_tile_, 0), 0);
    set_theme_img_ = Image(set_theme_tile_, &ltkk_txt_dark);
    lv_obj_align(set_theme_img_, LV_ALIGN_BOTTOM_LEFT, 0, -2);

    set_style_tile_ = tile(&ltkk_icon_layout, &ltkk_txt_standby, true);
    set_style_img_ = Image(set_style_tile_, kStyleNames[0]);
    lv_obj_align(set_style_img_, LV_ALIGN_BOTTOM_LEFT, 0, -2);

    set_media_tile_ = tile(&ltkk_icon_radio, &ltkk_txt_media, true);
    set_media_img_ = Image(set_media_tile_, &ltkk_txt_play_hint);
    lv_obj_align(set_media_img_, LV_ALIGN_BOTTOM_LEFT, 0, -2);

    // Ô thông tin: [wifi] tên mạng / tiêu đề / [pin] phần trăm
    lv_obj_t* info = tile(&ltkk_icon_wifi, &ltkk_txt_info, false);
    lv_obj_t* info_top = lv_obj_get_child(info, 0);
    set_ssid_label_ = Label(info_top, kSoft);
    lv_obj_set_width(set_ssid_label_, 54);
    lv_label_set_long_mode(set_ssid_label_, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_t* batt_row = Row(info, 4);
    lv_obj_align(batt_row, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    Image(batt_row, &ltkk_icon_battery);
    set_batt_label_ = Label(batt_row, kSoft);

    PlaceUnderChrome(settings_);
    SetHidden(settings_, true);
}

void LtkkDisplay::RefreshSettings() {
    auto& board = Board::GetInstance();
    if (auto codec = board.GetAudioCodec()) {
        lv_label_set_text_fmt(set_vol_label_, "%d%%", codec->output_volume());
    }
    if (auto backlight = board.GetBacklight()) {
        lv_label_set_text_fmt(set_bri_label_, "%d%%", backlight->brightness());
    }
    bool dark = current_theme_ != nullptr && current_theme_->name() == "dark";
    lv_image_set_src(set_theme_img_, dark ? &ltkk_txt_dark : &ltkk_txt_light);
    if (set_theme_icon_ != nullptr) {
        lv_image_set_src(set_theme_icon_, dark ? &ltkk_icon_moon : &ltkk_icon_sun);
    }
    lv_image_set_src(set_style_img_, kStyleNames[standby_style_.load()]);
    bool playing = LtkkMusic::GetInstance().IsActive() || LtkkMusic::GetInstance().IsPending();
    lv_image_set_src(set_media_img_, playing ? &ltkk_txt_stop_hint : &ltkk_txt_play_hint);
    lv_obj_set_style_border_width(set_media_tile_, playing ? 2 : 0, 0);
    lv_obj_set_style_border_color(set_media_tile_, lv_color_hex(LTKK_COLOR_ORANGE), 0);

    std::string ssid = WifiManager::GetInstance().GetSsid();
    lv_label_set_text(set_ssid_label_, ssid.empty() ? "--" : ssid.c_str());
    int level = 0;
    bool charging = false, discharging = false;
    if (board.GetBatteryLevel(level, charging, discharging)) {
        lv_label_set_text_fmt(set_batt_label_, charging ? "%d%% +" : "%d%%", level);
    } else {
        lv_label_set_text(set_batt_label_, "--");
    }
}

void LtkkDisplay::OpenSettings(bool open) {
    if (settings_ == nullptr) {
        return;
    }
    if (open) {
        RefreshSettings();
        settings_last_ms_ = lv_tick_get();
        PlaceUnderChrome(settings_);
    }
    SetHidden(settings_, !open);
}

void LtkkDisplay::ToggleSettings() { settings_toggle_request_ = true; }

void LtkkDisplay::ChangeVolume(int delta) {
    auto codec = Board::GetInstance().GetAudioCodec();
    if (codec == nullptr) {
        return;
    }
    int v = codec->output_volume() + delta;
    v = v < 0 ? 0 : (v > 100 ? 100 : v);
    codec->SetOutputVolume(v);
    lv_label_set_text_fmt(set_vol_label_, "%d%%", v);
}

void LtkkDisplay::ChangeBrightness(int delta) {
    auto backlight = Board::GetInstance().GetBacklight();
    if (backlight == nullptr) {
        return;
    }
    int v = backlight->brightness() + delta;
    v = v < 10 ? 10 : (v > 100 ? 100 : v);  // không tắt hẳn màn hình
    backlight->SetBrightness(v, true);
    lv_label_set_text_fmt(set_bri_label_, "%d%%", v);
}

// ------------------------------------------------------------------ cảm ứng
// Không dùng hệ thống sự kiện cảm ứng của LVGL (dễ lỡ các cú chạm nhanh). Bo mạch tự
// đọc FT6336G rồi gọi HandleTap / HandleSwipe; ở đây tìm đúng nút dưới ngón tay.
void LtkkDisplay::CreateTouch() {
#if LTKK_TOUCH_DEBUG
    touch_dot_ = Box(lv_layer_top());
    lv_obj_set_size(touch_dot_, 14, 14);
    lv_obj_set_style_radius(touch_dot_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(touch_dot_, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(touch_dot_, lv_color_hex(LTKK_COLOR_ORANGE), 0);
    lv_obj_set_style_border_width(touch_dot_, 2, 0);
    lv_obj_set_style_border_color(touch_dot_, lv_color_hex(0xFFFFFF), 0);
    SetHidden(touch_dot_, true);
#endif
}

lv_obj_t* LtkkDisplay::FindTarget(int x, int y) {
    lv_point_t pt = {x, y};
    lv_obj_t* obj = lv_indev_search_obj(lv_screen_active(), &pt);
    while (obj != nullptr && !lv_obj_has_flag(obj, LV_OBJ_FLAG_USER_1)) {
        obj = lv_obj_get_parent(obj);
    }
    return obj;
}

void LtkkDisplay::HandlePress(bool down, int x, int y) {
    if (!setup_ui_called_) {
        return;
    }
    DisplayLockGuard lock(this);
    if (pressed_obj_ != nullptr) {
        lv_obj_remove_state(pressed_obj_, LV_STATE_PRESSED);
        pressed_obj_ = nullptr;
    }
    if (down) {
        lv_obj_t* target = FindTarget(x, y);
        if (target != nullptr && target != settings_) {
            lv_obj_add_state(target, LV_STATE_PRESSED);
            pressed_obj_ = target;
        }
        settings_last_ms_ = lv_tick_get();
    }
    if (touch_dot_ != nullptr) {
        lv_obj_set_pos(touch_dot_, x - 7, y - 7);
        SetHidden(touch_dot_, !down);
    }
}

bool LtkkDisplay::HandleTap(int x, int y) {
    if (!setup_ui_called_) {
        return false;
    }
    DisplayLockGuard lock(this);
    lv_obj_t* target = FindTarget(x, y);
    ESP_LOGI(TAG, "Tap (%d,%d) -> %s", x, y, target != nullptr ? "LTKK UI" : "chat");
    if (target == nullptr) {
        return false;
    }
    OnClick(target);
    return true;
}

bool LtkkDisplay::HandleSwipe(Swipe dir) {
    if (!setup_ui_called_) {
        return false;
    }
    DisplayLockGuard lock(this);
    bool settings_open = !IsHidden(settings_);
    if (dir == kSwipeDown && !settings_open) {
        OpenSettings(true);
    } else if (dir == kSwipeUp && settings_open) {
        OpenSettings(false);
    } else if ((dir == kSwipeLeft || dir == kSwipeRight) && !settings_open && !IsHidden(standby_)) {
        SetStandbyStyle(standby_style_.load() + (dir == kSwipeLeft ? 1 : -1));
    } else {
        return settings_open;  // đang mở cài đặt: nuốt mọi thao tác
    }
    return true;
}

void LtkkDisplay::OnClick(lv_obj_t* t) {
    auto& music = LtkkMusic::GetInstance();
    settings_last_ms_ = lv_tick_get();

    if (t == gear_btn_) {
        OpenSettings(true);
    } else if (t == set_close_btn_) {
        OpenSettings(false);
    } else if (t == set_vol_minus_) {
        ChangeVolume(-10);
    } else if (t == set_vol_plus_) {
        ChangeVolume(10);
    } else if (t == set_bri_minus_) {
        ChangeBrightness(-10);
    } else if (t == set_bri_plus_) {
        ChangeBrightness(10);
    } else if (t == set_theme_tile_) {
        bool dark = current_theme_ != nullptr && current_theme_->name() == "dark";
        auto theme = LvglThemeManager::GetInstance().GetTheme(dark ? "light" : "dark");
        if (theme != nullptr) {
            SetTheme(theme);
        }
    } else if (t == set_style_tile_) {
        SetStandbyStyle(standby_style_.load() + 1);
        RefreshSettings();
    } else if (t == set_media_tile_) {
        if (music.IsActive() || music.IsPending()) {
            music.Stop();
        } else {
            music.PlayAny();
            OpenSettings(false);
        }
        RefreshSettings();
    } else if (t == timer_pause_btn_) {
        PauseResumeStudyTimer();
    } else if (t == timer_cancel_btn_) {
        CancelStudyTimer();
    } else if (t == music_play_btn_) {
        music.TogglePause();
    } else if (t == music_next_btn_) {
        music.Next();
    } else if (t == music_prev_btn_) {
        music.Prev();
    } else if (t == music_stop_btn_) {
        music.Stop();
    }
}

// ------------------------------------------------------------------ vòng lặp giao diện
void LtkkDisplay::TickCallback(lv_timer_t* timer) {
    static_cast<LtkkDisplay*>(lv_timer_get_user_data(timer))->OnTick();
}

void LtkkDisplay::OnTick() {
    auto state = Application::GetInstance().GetDeviceState();
    auto& music = LtkkMusic::GetInstance();
    int64_t now_us = esp_timer_get_time();
    uint32_t now_ms = lv_tick_get();
    bool state_changed = (int)state != last_state_;
    last_state_ = (int)state;
    bool idle = (state == kDeviceStateIdle);
    bool listening = (state == kDeviceStateListening);

    // Bạn gọi Lu -> dừng nhạc
    if (state_changed && (listening || state == kDeviceStateConnecting) && music.IsActive()) {
        music.Stop();
    }
    if (state_changed) {
        ShowWifiPanel(state == kDeviceStateWifiConfiguring);
        if (!idle) {
            OpenSettings(false);
        }
    }

    // Màn hình chờ: nhạc > hẹn giờ > kiểu đồng hồ đã chọn
    UpdateTimer(now_us);
    if (idle) {
        if (state_changed) {
            SetHidden(standby_, false);
            shown_style_ = -1;
        }
        bool show_music = music.IsActive();
        bool show_timer = !show_music && IsStudyTimerRunning();
        bool show_clock = !show_music && !show_timer;
        SetHidden(music_panel_, !show_music);
        SetHidden(timer_panel_, !show_timer);
        SetHidden(hint_pill_, !show_clock);
        if (show_clock) {
            ShowStandbyStyle(standby_style_.load());
            UpdateClock(false);
        } else {
            for (int i = 0; i < kStandbyStyles; i++) {
                SetHidden(style_panels_[i], true);
            }
            shown_style_ = -1;
        }
        if (show_music) {
            UpdateMusic();
        }
        if (now_ms - last_slow_ms_ > 1000) {
            last_slow_ms_ = now_ms;
            if (show_clock) {
                UpdateWeather();
            }
        }
    } else if (state_changed) {
        SetHidden(standby_, true);
    }

    // Khuôn mặt và sóng âm
    if (state_changed && face_ != nullptr) {
        if (listening) {
            SetEyes(&ltkk_eyes_listen);
            lv_obj_align(emoji_box_, LV_ALIGN_CENTER, 0, -28);
            SetHidden(wave_box_, false);
            SetHidden(listening_label_, false);
        } else {
            SetHidden(wave_box_, true);
            SetHidden(listening_label_, true);
            lv_obj_align(emoji_box_, LV_ALIGN_CENTER, 0, -16);
            if (state == kDeviceStateConnecting) {
                SetEyes(&ltkk_eyes_think);
            } else if (state == kDeviceStateSpeaking) {
                SetEyes(speaking_eyes_);
            } else if (!idle) {
                SetEyes(&ltkk_eyes_neutral);
            }
        }
    }
    if (face_ != nullptr && !idle && blink_enabled_ && (int32_t)(now_ms - next_blink_ms_) >= 0) {
        Blink();
        next_blink_ms_ = now_ms + 3000 + esp_random() % 3000;
    }

    // Cài đặt: mở/đóng theo yêu cầu, tự đóng sau 15 giây không chạm
    if (settings_toggle_request_.exchange(false)) {
        OpenSettings(IsHidden(settings_));
    }
    if (!IsHidden(settings_)) {
        if (lv_tick_elaps(settings_last_ms_) > 15000) {
            OpenSettings(false);
        } else if (now_ms - last_slow_ms_ < 250) {
            RefreshSettings();
        }
    }
}
