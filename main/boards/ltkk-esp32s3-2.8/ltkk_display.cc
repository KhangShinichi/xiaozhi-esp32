#include "ltkk_display.h"

#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <wifi_manager.h>

#include <cstring>
#include <ctime>

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

#define TAG "LtkkDisplay"

extern "C" {
extern const lv_image_dsc_t ltkk_background;  // ltkk_background.c
}

namespace {

constexpr uint32_t kCyan = 0x38C8F5;
constexpr uint32_t kSoft = 0xC8DCFF;
constexpr uint32_t kTrack = 0x2A3F75;

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

// Tên cảm xúc của Xiaozhi -> đôi mắt của Lu
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

// Sóng tam giác: v chạy 0..2*range, trả về 0..range..0
inline int32_t Triangle(int32_t v, int32_t range) { return v <= range ? v : 2 * range - v; }

void WaveExec(void* var, int32_t v) {
    lv_obj_set_height(static_cast<lv_obj_t*>(var), 6 + Triangle(v, 30));
}

void BlinkExec(void* var, int32_t v) {
    lv_image_set_scale_y(static_cast<lv_obj_t*>(var), 256 - Triangle(v, 228));
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

// Ảnh co giãn đúng kích thước (để bố cục flex tính đúng)
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

lv_obj_t* Button(lv_obj_t* parent, int w, int h, uint32_t bg, bool outline) {
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
    lv_obj_set_style_bg_opa(btn, LV_OPA_30, LV_STATE_PRESSED);
    return btn;
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

}  // namespace

LtkkDisplay::LtkkDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                         int width, int height, int offset_x, int offset_y, bool mirror_x,
                         bool mirror_y, bool swap_xy)
    : SpiLcdDisplay(panel_io, panel, width, height, offset_x, offset_y, mirror_x, mirror_y,
                    swap_xy) {}

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
    CreateQuickSettings();
    CreateTouch();
    StyleLowBattery();
    tick_timer_ = lv_timer_create(TickCallback, 200, this);
    ESP_LOGI(TAG, "Lu UI ready");
}

// Đặt đối tượng dưới thanh trạng thái / phụ đề / thông báo gốc của Xiaozhi
void LtkkDisplay::PlaceUnderChrome(lv_obj_t* obj) {
    if (top_bar_ != nullptr && lv_obj_get_parent(top_bar_) == lv_obj_get_parent(obj)) {
        lv_obj_move_to_index(obj, lv_obj_get_index(top_bar_));
    } else {
        lv_obj_move_foreground(obj);
    }
}

void LtkkDisplay::MakeInteractive(lv_obj_t* obj) {
    lv_obj_add_event_cb(obj, OnPressedEvent, LV_EVENT_PRESSED, this);
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
    // Khung phụ đề bo tròn, viền xanh ngọc, nhãn LU / BẠN phía trên
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
    // Nhãn LU / BẠN cần khung phụ đề nhiều dòng (tự giãn chiều cao)
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
    if (strcmp(role, "assistant") == 0) {
        lv_image_set_src(chat_tag_, &ltkk_tag_lu);
        SetHidden(chat_tag_, false);
        lv_obj_set_style_pad_top(bottom_bar_, 16, 0);
    } else if (strcmp(role, "user") == 0) {
        lv_image_set_src(chat_tag_, &ltkk_tag_user);
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
    if (bottom_bar_ != nullptr && chat_tag_ != nullptr) {
        lv_obj_set_style_bg_color(bottom_bar_, lv_color_hex(LTKK_COLOR_NAVY), 0);
        lv_obj_set_style_bg_opa(bottom_bar_, 210, 0);
        lv_obj_set_style_text_color(chat_message_label_, lv_color_hex(0xFFFFFF), 0);
    }
    if (qs_theme_img_ != nullptr) {
        RefreshQuickSettings();
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

    CreateClock(standby_);
    CreateTimer(standby_);
    CreateMusic(standby_);

    timeup_img_ = Image(standby_, &ltkk_txt_timeup);
    lv_obj_align(timeup_img_, LV_ALIGN_TOP_MID, 0, 36);
    SetHidden(timeup_img_, true);

    // Gợi ý gọi Lu
    hint_pill_ = Box(standby_);
    lv_obj_set_size(hint_pill_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(hint_pill_, lv_color_hex(LTKK_COLOR_ORANGE), 0);
    lv_obj_set_style_bg_opa(hint_pill_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(hint_pill_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_hor(hint_pill_, 14, 0);
    lv_obj_set_style_pad_ver(hint_pill_, 6, 0);
    lv_obj_align(hint_pill_, LV_ALIGN_BOTTOM_MID, 0, -12);
    Image(hint_pill_, &ltkk_txt_hint);

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

void LtkkDisplay::CreateClock(lv_obj_t* parent) {
    clock_box_ = Column(parent, 4);
    lv_obj_set_width(clock_box_, LV_PCT(100));
    lv_obj_set_flex_align(clock_box_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_align(clock_box_, LV_ALIGN_CENTER, 0, -14);

    lv_obj_t* digits = Row(clock_box_, 0);
    for (int i = 0; i < 5; i++) {
        clock_digits_[i] = Image(digits, i == 2 ? &ltkk_digit_colon : &ltkk_digit_0);
    }

    lv_obj_t* date_row = Row(clock_box_, 8);
    weekday_img_ = Image(date_row, kWeekdays[0]);
    date_label_ = Label(date_row, kSoft);

    weather_row_ = Row(clock_box_, 6);
    lv_obj_set_style_bg_color(weather_row_, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(weather_row_, 26, 0);
    lv_obj_set_style_radius(weather_row_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_hor(weather_row_, 10, 0);
    lv_obj_set_style_pad_ver(weather_row_, 3, 0);
    lv_obj_set_style_margin_top(weather_row_, 4, 0);
    Image(weather_row_, &ltkk_icon_cloud);
    weather_img_ = Image(weather_row_, kWeather[1]);
    weather_temp_ = Label(weather_row_, 0xFFFFFF);
    Image(weather_row_, &ltkk_txt_degc);
    SetHidden(weather_row_, true);
}

void LtkkDisplay::CreateTimer(lv_obj_t* parent) {
    timer_panel_ = Box(parent);
    lv_obj_set_size(timer_panel_, LV_PCT(100), LV_PCT(100));

    timer_arc_ = lv_arc_create(timer_panel_);
    lv_obj_set_size(timer_arc_, 128, 128);
    lv_obj_align(timer_arc_, LV_ALIGN_LEFT_MID, 16, 2);
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
    lv_obj_t* row = Row(center, 0);
    for (int i = 0; i < 5; i++) {
        timer_digits_[i] = ScaledImage(row, i == 2 ? &ltkk_digit_colon : &ltkk_digit_0, 0.42f);
    }
    Image(center, &ltkk_txt_remaining);

    lv_obj_t* right = Column(timer_panel_, 8);
    lv_obj_align(right, LV_ALIGN_LEFT_MID, 160, -6);
    Image(right, &ltkk_txt_timer);
    Image(right, &ltkk_txt_timer_sub);
    lv_obj_t* buttons = Row(right, 8);
    lv_obj_set_style_margin_top(buttons, 6, 0);

    timer_pause_btn_ = Button(buttons, 84, 32, 0xFFFFFF, false);
    timer_pause_img_ = Image(timer_pause_btn_, &ltkk_btn_pause);
    lv_obj_center(timer_pause_img_);
    timer_cancel_btn_ = Button(buttons, 56, 32, 0, true);
    lv_obj_center(Image(timer_cancel_btn_, &ltkk_btn_cancel));
    for (lv_obj_t* b : {timer_pause_btn_, timer_cancel_btn_}) {
        MakeInteractive(b);
        lv_obj_add_event_cb(b, OnButtonEvent, LV_EVENT_CLICKED, this);
    }

    lv_obj_t* hint = Image(timer_panel_, &ltkk_txt_timer_hint);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -10);
    SetHidden(timer_panel_, true);
}

void LtkkDisplay::CreateMusic(lv_obj_t* parent) {
    music_panel_ = Box(parent);
    lv_obj_set_size(music_panel_, LV_PCT(100), LV_PCT(100));

    lv_obj_t* art = Box(music_panel_);
    lv_obj_set_size(art, 96, 96);
    lv_obj_align(art, LV_ALIGN_TOP_LEFT, 18, 42);
    lv_obj_set_style_radius(art, 16, 0);
    lv_obj_set_style_bg_opa(art, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(art, lv_color_hex(LTKK_COLOR_BLUE), 0);
    lv_obj_set_style_bg_grad_color(art, lv_color_hex(LTKK_COLOR_ORANGE), 0);
    lv_obj_set_style_bg_grad_dir(art, LV_GRAD_DIR_VER, 0);
    lv_obj_center(Image(art, &ltkk_icon_note));

    lv_obj_t* info = Column(music_panel_, 6);
    lv_obj_align(info, LV_ALIGN_TOP_LEFT, 128, 50);
    Image(info, &ltkk_txt_music);
    music_title_ = Label(info, 0xFFFFFF);
    lv_obj_set_width(music_title_, 176);
    lv_label_set_long_mode(music_title_, LV_LABEL_LONG_MODE_DOTS);
    Image(info, &ltkk_txt_sdcard);
    music_bar_ = lv_bar_create(info);
    lv_obj_set_size(music_bar_, 176, 6);
    lv_bar_set_range(music_bar_, 0, 1000);
    lv_obj_set_style_bg_color(music_bar_, lv_color_hex(kTrack), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(music_bar_, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(music_bar_, lv_color_hex(LTKK_COLOR_ORANGE), LV_PART_INDICATOR);
    lv_obj_set_style_margin_top(music_bar_, 8, 0);

    lv_obj_t* controls = Row(music_panel_, 22);
    lv_obj_align(controls, LV_ALIGN_BOTTOM_MID, 0, -16);
    music_prev_btn_ = Button(controls, 42, 42, 0x2A4A8F, false);
    music_play_btn_ = Button(controls, 54, 54, LTKK_COLOR_ORANGE, false);
    music_next_btn_ = Button(controls, 42, 42, 0x2A4A8F, false);
    for (lv_obj_t* b : {music_prev_btn_, music_play_btn_, music_next_btn_}) {
        lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
        MakeInteractive(b);
        lv_obj_add_event_cb(b, OnButtonEvent, LV_EVENT_CLICKED, this);
    }
    lv_obj_center(Image(music_prev_btn_, &ltkk_icon_prev));
    music_play_img_ = Image(music_play_btn_, &ltkk_icon_pause);
    lv_obj_center(music_play_img_);
    lv_obj_center(Image(music_next_btn_, &ltkk_icon_next));
    SetHidden(music_panel_, true);
}

void LtkkDisplay::SetDigits(lv_obj_t* const* slots, int a, int b) {
    lv_image_set_src(slots[0], kDigits[(a / 10) % 10]);
    lv_image_set_src(slots[1], kDigits[a % 10]);
    lv_image_set_src(slots[3], kDigits[(b / 10) % 10]);
    lv_image_set_src(slots[4], kDigits[b % 10]);
}

void LtkkDisplay::UpdateClock(bool force) {
    time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);
    if ((t.tm_year + 1900) < 2025) {  // chưa đồng bộ giờ từ máy chủ
        SetHidden(clock_box_, true);
        last_minute_ = -1;
        return;
    }
    SetHidden(clock_box_, false);
    int minute_key = t.tm_hour * 60 + t.tm_min;
    if (!force && minute_key == last_minute_) {
        return;
    }
    last_minute_ = minute_key;
    SetDigits(clock_digits_, t.tm_hour, t.tm_min);
    lv_image_set_src(weekday_img_, kWeekdays[t.tm_wday == 0 ? 6 : t.tm_wday - 1]);
    lv_label_set_text_fmt(date_label_, "%02d/%02d/%04d", t.tm_mday, t.tm_mon + 1,
                          t.tm_year + 1900);
}

void LtkkDisplay::UpdateWeather() {
    auto& weather = LtkkWeather::GetInstance();
    if (!weather.valid()) {
        SetHidden(weather_row_, true);
        return;
    }
    int cond = weather.condition();
    int temp = weather.temperature();
    int key = cond * 1000 + temp;
    if (key != last_weather_key_) {
        last_weather_key_ = key;
        lv_image_set_src(weather_img_, kWeather[cond >= 0 && cond < 6 ? cond : 1]);
        lv_label_set_text_fmt(weather_temp_, "%d", temp);
    }
    SetHidden(weather_row_, false);
}

// ------------------------------------------------------------------ hẹn giờ
void LtkkDisplay::StartStudyTimer(int minutes) {
    int64_t total = (int64_t)minutes * 60 * 1000000LL;
    timer_total_us_ = total;
    timer_paused_left_us_ = 0;
    timer_end_us_ = esp_timer_get_time() + total;
    ESP_LOGI(TAG, "Study timer: %d min", minutes);
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
        // Hết giờ: báo chuông và hiện dòng chữ 15 giây
        timer_end_us_ = 0;
        timeup_until_us_ = now_us + 15 * 1000000LL;
        SetEyes(&ltkk_eyes_happy);
        Application::GetInstance().Schedule([]() {
            Application::GetInstance().PlaySound(Lang::Sounds::OGG_POPUP);
        });
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

// ------------------------------------------------------------------ nhạc
void LtkkDisplay::UpdateMusic() {
    auto& music = LtkkMusic::GetInstance();
    std::string title = music.CurrentTitle();
    if (title != music_title_text_) {
        music_title_text_ = title;
        lv_label_set_text(music_title_, title.c_str());
    }
    lv_bar_set_value(music_bar_, music.ProgressPermille(), LV_ANIM_OFF);
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
#if LV_USE_QRCODE
    wifi_qr_ = lv_qrcode_create(qr_box);
    lv_qrcode_set_size(wifi_qr_, 100);
    lv_qrcode_set_dark_color(wifi_qr_, lv_color_hex(LTKK_COLOR_NAVY));
    lv_qrcode_set_light_color(wifi_qr_, lv_color_hex(0xFFFFFF));
    lv_obj_center(wifi_qr_);
#endif
    Image(left, &ltkk_txt_wifi_scan);

    lv_obj_t* right = Column(wifi_panel_, 9);
    lv_obj_align(right, LV_ALIGN_LEFT_MID, 142, 0);
    Image(right, &ltkk_txt_wifi_title);

    auto step = [&](const char* num, const lv_image_dsc_t* title, lv_obj_t** value_label,
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
        Image(col, title);
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
    std::string url = wifi.GetApWebUrl();
    if (ssid.empty()) {
        return;  // BluFi: dùng hướng dẫn mặc định của Xiaozhi
    }
    lv_label_set_text(wifi_ssid_label_, ssid.c_str());
    lv_label_set_text(wifi_url_label_, url.c_str());
#if LV_USE_QRCODE
    std::string qr = "WIFI:T:nopass;S:" + ssid + ";;";
    lv_qrcode_update(wifi_qr_, qr.c_str(), qr.size());
#endif
    lv_obj_move_foreground(wifi_panel_);
    SetHidden(wifi_panel_, false);
}

// ------------------------------------------------------------------ cài đặt nhanh
void LtkkDisplay::CreateQuickSettings() {
    qs_panel_ = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(qs_panel_);
    lv_obj_remove_flag(qs_panel_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(qs_panel_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(qs_panel_, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(qs_panel_, lv_color_hex(0x081738), 0);
    lv_obj_set_style_bg_opa(qs_panel_, 245, 0);
    lv_obj_set_style_pad_top(qs_panel_, 34, 0);
    lv_obj_set_style_pad_hor(qs_panel_, 14, 0);
    lv_obj_set_flex_flow(qs_panel_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(qs_panel_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(qs_panel_, 8, 0);
    MakeInteractive(qs_panel_);
    lv_obj_add_event_cb(qs_panel_, OnButtonEvent, LV_EVENT_CLICKED, this);

    auto slider_card = [&](const lv_image_dsc_t* title, lv_obj_t** slider, lv_obj_t** value) {
        lv_obj_t* card = Column(qs_panel_, 6);
        lv_obj_set_width(card, LV_PCT(100));
        lv_obj_set_style_bg_color(card, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_bg_opa(card, 26, 0);
        lv_obj_set_style_radius(card, 12, 0);
        lv_obj_set_style_pad_all(card, 8, 0);
        lv_obj_set_style_pad_hor(card, 12, 0);
        lv_obj_t* head = Box(card);
        lv_obj_set_size(head, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_align(Image(head, title), LV_ALIGN_LEFT_MID, 0, 0);
        *value = Label(head, LTKK_COLOR_ORANGE);
        lv_obj_align(*value, LV_ALIGN_RIGHT_MID, 0, 0);
        *slider = lv_slider_create(card);
        lv_obj_set_size(*slider, LV_PCT(96), 8);
        lv_obj_set_style_margin_hor(*slider, 6, 0);
        lv_slider_set_range(*slider, 0, 100);
        lv_obj_set_style_bg_color(*slider, lv_color_hex(kTrack), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(*slider, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(*slider, lv_color_hex(kCyan), LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(*slider, lv_color_hex(0xFFFFFF), LV_PART_KNOB);
        lv_obj_set_style_pad_all(*slider, 5, LV_PART_KNOB);
        lv_obj_set_ext_click_area(*slider, 12);
        MakeInteractive(*slider);
        lv_obj_add_event_cb(*slider, OnSliderEvent, LV_EVENT_VALUE_CHANGED, this);
    };
    slider_card(&ltkk_txt_volume, &qs_vol_slider_, &qs_vol_label_);
    slider_card(&ltkk_txt_brightness, &qs_bri_slider_, &qs_bri_label_);

    lv_obj_t* tiles = Row(qs_panel_, 8);
    auto tile = [&](const lv_image_dsc_t* title, const char* text) {
        lv_obj_t* t = Column(tiles, 2);
        lv_obj_set_size(t, 92, 50);
        lv_obj_set_style_bg_color(t, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_bg_opa(t, 26, 0);
        lv_obj_set_style_radius(t, 12, 0);
        lv_obj_set_style_pad_all(t, 7, 0);
        lv_obj_set_style_pad_left(t, 10, 0);
        if (title != nullptr) {
            Image(t, title);
        } else {
            lv_label_set_text(Label(t, 0xFFFFFF), text);
        }
        return t;
    };
    qs_theme_tile_ = tile(&ltkk_txt_theme, nullptr);
    lv_obj_add_flag(qs_theme_tile_, LV_OBJ_FLAG_CLICKABLE);
    MakeInteractive(qs_theme_tile_);
    lv_obj_add_event_cb(qs_theme_tile_, OnButtonEvent, LV_EVENT_CLICKED, this);
    qs_theme_img_ = Image(qs_theme_tile_, &ltkk_txt_dark);

    lv_obj_t* wifi_tile = tile(nullptr, "Wi-Fi");
    qs_ssid_label_ = Label(wifi_tile, kSoft);
    lv_obj_set_width(qs_ssid_label_, 74);
    lv_label_set_long_mode(qs_ssid_label_, LV_LABEL_LONG_MODE_DOTS);

    lv_obj_t* batt_tile = tile(&ltkk_txt_battery, nullptr);
    qs_batt_label_ = Label(batt_tile, kSoft);

    Image(qs_panel_, &ltkk_txt_qs_hint);

    PlaceUnderChrome(qs_panel_);
    SetHidden(qs_panel_, true);
}

void LtkkDisplay::RefreshQuickSettings() {
    auto& board = Board::GetInstance();
    auto codec = board.GetAudioCodec();
    if (codec != nullptr) {
        lv_slider_set_value(qs_vol_slider_, codec->output_volume(), LV_ANIM_OFF);
        lv_label_set_text_fmt(qs_vol_label_, "%d%%", codec->output_volume());
    }
    auto backlight = board.GetBacklight();
    if (backlight != nullptr) {
        lv_slider_set_value(qs_bri_slider_, backlight->brightness(), LV_ANIM_OFF);
        lv_label_set_text_fmt(qs_bri_label_, "%d%%", backlight->brightness());
    }
    bool dark = current_theme_ != nullptr && current_theme_->name() == "dark";
    lv_image_set_src(qs_theme_img_, dark ? &ltkk_txt_dark : &ltkk_txt_light);
    lv_obj_set_style_bg_color(qs_theme_tile_,
                              lv_color_hex(dark ? LTKK_COLOR_ORANGE : 0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(qs_theme_tile_, dark ? LV_OPA_COVER : 26, 0);

    std::string ssid = WifiManager::GetInstance().GetSsid();
    lv_label_set_text(qs_ssid_label_, ssid.empty() ? "--" : ssid.c_str());

    int level = 0;
    bool charging = false, discharging = false;
    if (board.GetBatteryLevel(level, charging, discharging)) {
        lv_label_set_text_fmt(qs_batt_label_, charging ? "%d%% +" : "%d%%", level);
    } else {
        lv_label_set_text(qs_batt_label_, "--");
    }
}

void LtkkDisplay::OpenQuickSettings(bool open) {
    if (qs_panel_ == nullptr) {
        return;
    }
    if (open) {
        RefreshQuickSettings();
        qs_last_ms_ = lv_tick_get();
    }
    SetHidden(qs_panel_, !open);
}

void LtkkDisplay::ToggleQuickSettings() { qs_toggle_request_ = true; }

// ------------------------------------------------------------------ cảm ứng
void LtkkDisplay::CreateTouch() {
    indev_ = lv_indev_create();
    lv_indev_set_type(indev_, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev_, IndevRead);
    lv_indev_set_user_data(indev_, this);
    lv_indev_set_display(indev_, display_);
    lv_obj_add_event_cb(lv_screen_active(), OnGestureEvent, LV_EVENT_GESTURE, this);
}

void LtkkDisplay::FeedTouch(bool pressed, int x, int y) {
    if (pressed && !touch_pressed_.load()) {
        touch_consumed_ = false;  // lần chạm mới
    }
    touch_x_ = x;
    touch_y_ = y;
    touch_pressed_ = pressed;
}

bool LtkkDisplay::TakeTouchConsumed() { return touch_consumed_.exchange(false); }

void LtkkDisplay::IndevRead(lv_indev_t* indev, lv_indev_data_t* data) {
    auto self = static_cast<LtkkDisplay*>(lv_indev_get_user_data(indev));
    data->point.x = self->touch_x_.load();
    data->point.y = self->touch_y_.load();
    data->state = self->touch_pressed_.load() ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

void LtkkDisplay::OnPressedEvent(lv_event_t* e) {
    auto self = static_cast<LtkkDisplay*>(lv_event_get_user_data(e));
    self->touch_consumed_ = true;
    self->qs_last_ms_ = lv_tick_get();
}

void LtkkDisplay::OnGestureEvent(lv_event_t* e) {
    auto self = static_cast<LtkkDisplay*>(lv_event_get_user_data(e));
    lv_indev_t* indev = lv_indev_active();
    if (indev == nullptr) {
        return;
    }
    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    if (dir == LV_DIR_BOTTOM) {
        self->OpenQuickSettings(true);
        self->touch_consumed_ = true;
    } else if (dir == LV_DIR_TOP && !lv_obj_has_flag(self->qs_panel_, LV_OBJ_FLAG_HIDDEN)) {
        self->OpenQuickSettings(false);
        self->touch_consumed_ = true;
    }
}

void LtkkDisplay::OnButtonEvent(lv_event_t* e) {
    auto self = static_cast<LtkkDisplay*>(lv_event_get_user_data(e));
    lv_obj_t* target = lv_event_get_current_target_obj(e);
    auto& music = LtkkMusic::GetInstance();

    if (target == self->timer_pause_btn_) {
        self->PauseResumeStudyTimer();
    } else if (target == self->timer_cancel_btn_) {
        self->CancelStudyTimer();
    } else if (target == self->music_play_btn_) {
        music.TogglePause();
    } else if (target == self->music_next_btn_) {
        music.Next();
    } else if (target == self->music_prev_btn_) {
        music.Prev();
    } else if (target == self->qs_theme_tile_) {
        bool dark = self->current_theme_ != nullptr && self->current_theme_->name() == "dark";
        auto theme = LvglThemeManager::GetInstance().GetTheme(dark ? "light" : "dark");
        if (theme != nullptr) {
            self->SetTheme(theme);
        }
    } else if (target == self->qs_panel_ && lv_event_get_target_obj(e) == self->qs_panel_) {
        self->OpenQuickSettings(false);  // chạm vùng trống để đóng
    }
}

void LtkkDisplay::OnSliderEvent(lv_event_t* e) {
    auto self = static_cast<LtkkDisplay*>(lv_event_get_user_data(e));
    lv_obj_t* slider = lv_event_get_current_target_obj(e);
    int value = lv_slider_get_value(slider);
    auto& board = Board::GetInstance();
    if (slider == self->qs_vol_slider_) {
        if (auto codec = board.GetAudioCodec()) {
            codec->SetOutputVolume(value);
        }
        lv_label_set_text_fmt(self->qs_vol_label_, "%d%%", value);
    } else if (slider == self->qs_bri_slider_) {
        if (value < 5) {
            value = 5;  // không tắt hẳn màn hình
        }
        if (auto backlight = board.GetBacklight()) {
            backlight->SetBrightness(value, true);
        }
        lv_label_set_text_fmt(self->qs_bri_label_, "%d%%", value);
    }
    self->qs_last_ms_ = lv_tick_get();
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

    // Người dùng bắt đầu nói chuyện -> dừng nhạc (trừ khi nhạc đang chờ Lu nói xong)
    if (state_changed && (listening || state == kDeviceStateConnecting) && music.IsActive()) {
        music.Stop();
    }

    // Màn hình cài Wi-Fi
    if (state_changed) {
        ShowWifiPanel(state == kDeviceStateWifiConfiguring);
    }

    // Màn hình chờ: nhạc > hẹn giờ > đồng hồ
    UpdateTimer(now_us);
    if (idle) {
        if (state_changed) {
            SetHidden(standby_, false);
            UpdateClock(true);
        }
        bool show_music = music.IsActive();
        bool show_timer = !show_music && IsStudyTimerRunning();
        bool show_clock = !show_music && !show_timer;
        SetHidden(music_panel_, !show_music);
        SetHidden(timer_panel_, !show_timer);
        SetHidden(hint_pill_, !show_clock);
        if (show_clock) {
            UpdateClock(false);
        } else {
            SetHidden(clock_box_, true);
            last_minute_ = -1;
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

    // Chớp mắt ngẫu nhiên 3-6 giây một lần
    if (face_ != nullptr && !idle && blink_enabled_ &&
        (int32_t)(now_ms - next_blink_ms_) >= 0) {
        Blink();
        next_blink_ms_ = now_ms + 3000 + esp_random() % 3000;
    }

    // Cài đặt nhanh: mở/đóng theo yêu cầu, tự đóng sau 10 giây không chạm
    if (qs_toggle_request_.exchange(false)) {
        OpenQuickSettings(lv_obj_has_flag(qs_panel_, LV_OBJ_FLAG_HIDDEN));
    }
    if (!lv_obj_has_flag(qs_panel_, LV_OBJ_FLAG_HIDDEN)) {
        if (lv_tick_elaps(qs_last_ms_) > 10000) {
            OpenQuickSettings(false);
        }
    }
}
