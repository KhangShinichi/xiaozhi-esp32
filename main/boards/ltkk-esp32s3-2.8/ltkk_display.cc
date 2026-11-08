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

// LVGL: bỏ ảnh đã giải mã khỏi bộ nhớ đệm (ảnh bìa đổi nội dung nhưng giữ địa chỉ)
void lv_image_cache_drop(const void* src);
}

namespace {

constexpr uint32_t kCyan = 0x38C8F5;
constexpr uint32_t kSoft = 0xC8DCFF;
constexpr uint32_t kTrack = 0x2A3F75;
constexpr uint32_t kTile = 0x14306E;
constexpr uint32_t kOrange = 0xFF7A1A;

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
    &ltkk_style_4, &ltkk_style_5, &ltkk_style_6, &ltkk_style_7,
};
const lv_image_dsc_t* const kStyleBig[LtkkDisplay::kStandbyStyles] = {
    &ltkk_stylebig_0, &ltkk_stylebig_1, &ltkk_stylebig_2, &ltkk_stylebig_3,
    &ltkk_stylebig_4, &ltkk_stylebig_5, &ltkk_stylebig_6, &ltkk_stylebig_7,
};
const lv_image_dsc_t* const kWeatherBig[6] = {
    &ltkk_wxbig_0, &ltkk_wxbig_1, &ltkk_wxbig_2, &ltkk_wxbig_3, &ltkk_wxbig_4, &ltkk_wxbig_5,
};
// Lời chào: sáng / trưa / chiều / tối / khuya
const lv_image_dsc_t* const kGreetings[5] = {
    &ltkk_greet_0, &ltkk_greet_1, &ltkk_greet_2, &ltkk_greet_3, &ltkk_greet_4,
};
// Tuỳ chọn mặc định của từng kiểu màn hình chờ
constexpr uint8_t kDefaultOpts[LtkkDisplay::kStandbyStyles] = {
    LtkkDisplay::kOptWeather | LtkkDisplay::kOptDate,                              // Đồng hồ
    LtkkDisplay::kOptWeather | LtkkDisplay::kOptDate | LtkkDisplay::kOptSeconds,   // Kim LTKK
    LtkkDisplay::kOptDate,                                                         // Lu ngủ
    LtkkDisplay::kOptWeather | LtkkDisplay::kOptDate,                              // Thẻ LTKK
    LtkkDisplay::kOptWeather | LtkkDisplay::kOptDate,                              // Lu & bạn
    LtkkDisplay::kOptWeather | LtkkDisplay::kOptDate,                              // Thời tiết
    LtkkDisplay::kOptWeather | LtkkDisplay::kOptDate,                              // Lịch
    LtkkDisplay::kOptDate | LtkkDisplay::kOptPlain,                                // Ban đêm
};

// Cảm xúc của mô hình -> khuôn mặt Lu + nhãn tiếng Việt (nullptr = không hiện nhãn)
struct EmotionMap {
    const char* name;
    const lv_image_dsc_t* eyes;
    const lv_image_dsc_t* label;
};
const EmotionMap kEmotions[] = {
    {"neutral", &ltkk_eyes_neutral, nullptr},
    {"happy", &ltkk_eyes_happy, &ltkk_emo_happy},
    {"laughing", &ltkk_eyes_happy, &ltkk_emo_happy},
    {"funny", &ltkk_eyes_happy, &ltkk_emo_happy},
    {"delicious", &ltkk_eyes_happy, &ltkk_emo_happy},
    {"confident", &ltkk_eyes_wink, &ltkk_emo_wink},
    {"cool", &ltkk_eyes_wink, &ltkk_emo_wink},
    {"relaxed", &ltkk_eyes_happy, &ltkk_emo_happy},
    {"embarrassed", &ltkk_eyes_shy, &ltkk_emo_shy},
    {"sad", &ltkk_eyes_sad, &ltkk_emo_sad},
    {"crying", &ltkk_eyes_sad, &ltkk_emo_sad},
    {"angry", &ltkk_eyes_angry, &ltkk_emo_angry},
    {"loving", &ltkk_eyes_love, &ltkk_emo_love},
    {"kissy", &ltkk_eyes_love, &ltkk_emo_love},
    {"surprised", &ltkk_eyes_surprise, &ltkk_emo_surprise},
    {"shocked", &ltkk_eyes_surprise, &ltkk_emo_surprise},
    {"thinking", &ltkk_eyes_think, &ltkk_emo_think},
    {"confused", &ltkk_eyes_confused, &ltkk_emo_confused},
    {"winking", &ltkk_eyes_wink, &ltkk_emo_wink},
    {"silly", &ltkk_eyes_wink, &ltkk_emo_wink},
    {"sleepy", &ltkk_eyes_sleepy, &ltkk_emo_sleepy},
};

// Trình diễn cảm xúc (vuốt lên ở màn hình chờ, hoặc "Lu ơi, cho xem các cảm xúc")
struct DemoFace {
    const char* key;
    const lv_image_dsc_t* eyes;
    const lv_image_dsc_t* label;
};
const DemoFace kDemoFaces[] = {
    {"neutral", &ltkk_eyes_neutral, &ltkk_emo_neutral},
    {"happy", &ltkk_eyes_happy, &ltkk_emo_happy},
    {"love", &ltkk_eyes_love, &ltkk_emo_love},
    {"wink", &ltkk_eyes_wink, &ltkk_emo_wink},
    {"surprise", &ltkk_eyes_surprise, &ltkk_emo_surprise},
    {"think", &ltkk_eyes_think, &ltkk_emo_think},
    {"confused", &ltkk_eyes_confused, &ltkk_emo_confused},
    {"shy", &ltkk_eyes_shy, &ltkk_emo_shy},
    {"sad", &ltkk_eyes_sad, &ltkk_emo_sad},
    {"angry", &ltkk_eyes_angry, &ltkk_emo_angry},
    {"sleepy", &ltkk_eyes_sleepy, &ltkk_emo_sleepy},
    {"listen", &ltkk_eyes_listen, &ltkk_emo_listen},
};
constexpr int kDemoCount = sizeof(kDemoFaces) / sizeof(kDemoFaces[0]);

inline int32_t Triangle(int32_t v, int32_t range) { return v <= range ? v : 2 * range - v; }

void WaveExec(void* var, int32_t v) {
    lv_obj_set_height(static_cast<lv_obj_t*>(var), 6 + Triangle(v, 30));
}
void EqExec(void* var, int32_t v) {
    lv_obj_set_height(static_cast<lv_obj_t*>(var), 4 + Triangle(v, 16));
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
    for (int i = 0; i < kStandbyStyles; i++) {
        int o = settings.GetInt("sbopt" + std::to_string(i), kDefaultOpts[i]);
        standby_opts_[i] = (uint8_t)(o & 0x0F);
    }
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

    // Nhãn cảm xúc tiếng Việt (VUI, BUỒN, GIẬN...) - hiện 2,5 giây khi đổi cảm xúc
    lv_obj_t* screen = lv_screen_active();
    emo_tag_ = Box(screen);
    lv_obj_set_size(emo_tag_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(emo_tag_, 10, 0);
    lv_obj_set_style_pad_ver(emo_tag_, 3, 0);
    lv_obj_set_style_radius(emo_tag_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(emo_tag_, lv_color_hex(kOrange), 0);
    lv_obj_set_style_bg_opa(emo_tag_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(emo_tag_, 2, 0);
    lv_obj_set_style_border_color(emo_tag_, lv_color_hex(0xFFFFFF), 0);
    emo_tag_img_ = lv_image_create(emo_tag_);
    lv_image_set_src(emo_tag_img_, &ltkk_emo_happy);
    SetHidden(emo_tag_, true);
}

void LtkkDisplay::StartEmotionDemo(const std::string& key) {
    DisplayLockGuard lock(this);
    demo_key_ = key;
    demo_request_ = true;   // xử lý trong OnTick (luồng LVGL)
}

void LtkkDisplay::StopEmotionDemo() {
    if (demo_index_ < 0) {
        return;
    }
    demo_index_ = -1;
    ShowEmotionTag(nullptr);
    last_state_ = -1;       // vẽ lại màn hình theo trạng thái hiện tại
    current_eyes_ = nullptr;
}

void LtkkDisplay::ShowEmotionTag(const lv_image_dsc_t* label) {
    if (emo_tag_ == nullptr) {
        return;
    }
    if (label == nullptr) {
        SetHidden(emo_tag_, true);
        return;
    }
    lv_image_set_src(emo_tag_img_, label);
    lv_obj_align_to(emo_tag_, emoji_box_, LV_ALIGN_OUT_BOTTOM_MID, 0, -14);
    lv_obj_move_to_index(emo_tag_, -1);
    SetHidden(emo_tag_, false);
    emo_tag_until_ms_ = lv_tick_get() + 2500;
}

void LtkkDisplay::SetEyes(const lv_image_dsc_t* eyes) {
    if (face_ == nullptr || eyes == current_eyes_) {
        return;
    }
    current_eyes_ = eyes;
    blink_until_ms_ = 0;
    lv_image_set_src(face_, eyes);
    lv_obj_set_size(face_, eyes->header.w, eyes->header.h);
    lv_image_set_pivot(face_, eyes->header.w / 2, eyes->header.h / 2);
    // Khi nghe: thu nhỏ Lu để chừa chỗ cho sóng âm
    lv_image_set_scale(face_, eyes == &ltkk_eyes_listen ? 210 : 256);
    blink_enabled_ = (eyes == &ltkk_eyes_neutral || eyes == &ltkk_eyes_listen);
}

// Chớp mắt: đổi sang ảnh nhắm mắt trong một nhịp (~200 ms) rồi trả lại
void LtkkDisplay::Blink() {
    const lv_image_dsc_t* closed =
        current_eyes_ == &ltkk_eyes_listen ? &ltkk_eyes_listen_blink : &ltkk_eyes_blink;
    lv_image_set_src(face_, closed);
    blink_until_ms_ = lv_tick_get() + 150;
}

bool LtkkDisplay::SetEmotion(const char* emotion) {
    if (face_ == nullptr) {
        return SpiLcdDisplay::SetEmotion(emotion);
    }
    if (emotion == nullptr) {
        return false;
    }
    const lv_image_dsc_t* eyes = &ltkk_eyes_neutral;
    const lv_image_dsc_t* label = nullptr;
    for (const auto& e : kEmotions) {
        if (strcmp(e.name, emotion) == 0) {
            eyes = e.eyes;
            label = e.label;
            break;
        }
    }
    DisplayLockGuard lock(this);
    bool changed = eyes != speaking_eyes_;
    speaking_eyes_ = eyes;
    auto state = Application::GetInstance().GetDeviceState();
    if (state != kDeviceStateListening && state != kDeviceStateConnecting) {
        SetEyes(eyes);
        if (changed) {
            ShowEmotionTag(label);
        }
    }
    return true;
}

// ------------------------------------------------------------------ sóng âm
void LtkkDisplay::CreateWave() {
    lv_obj_t* screen = lv_screen_active();
    wave_box_ = Box(screen);
    lv_obj_set_size(wave_box_, LV_PCT(100), 40);
    lv_obj_align(wave_box_, LV_ALIGN_CENTER, 0, 38);
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
// 8 kiểu, mỗi kiểu có tuỳ chọn riêng (thời tiết / ngày tháng / giây / nền trơn) lưu trong NVS.
// Chỉnh trực tiếp: Cài đặt -> "Màn hình chờ" mở bảng tuỳ chỉnh ở mép dưới, màn hình
// chờ phía trên đổi ngay để xem trước.
void LtkkDisplay::CreateStandby() {
    standby_ = Box(lv_screen_active());
    lv_obj_set_size(standby_, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(standby_, lv_color_hex(LTKK_COLOR_NAVY), 0);
    lv_obj_set_style_bg_grad_color(standby_, lv_color_hex(0x050E26), 0);
    lv_obj_set_style_bg_grad_dir(standby_, LV_GRAD_DIR_VER, 0);
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
    CreateStyleCompanion(style_panels_[4]);
    CreateStyleWeather(style_panels_[5]);
    CreateStyleCalendar(style_panels_[6]);
    CreateStyleNight(style_panels_[7]);
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
    brand_bar_ = Box(standby_);
    lv_obj_set_size(brand_bar_, LV_PCT(100), 3);
    lv_obj_align(brand_bar_, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(brand_bar_, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(brand_bar_, lv_color_hex(LTKK_COLOR_BLUE), 0);
    lv_obj_set_style_bg_grad_color(brand_bar_, lv_color_hex(LTKK_COLOR_ORANGE), 0);
    lv_obj_set_style_bg_grad_dir(brand_bar_, LV_GRAD_DIR_HOR, 0);

    CreateStandbyEditor();
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

// Viên "kính mờ": nền trắng trong suốt, viền mảnh, bo tròn
namespace {
lv_obj_t* Pill(lv_obj_t* parent, int gap) {
    lv_obj_t* p = Row(parent, gap);
    lv_obj_set_style_bg_color(p, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(p, 30, 0);
    lv_obj_set_style_border_width(p, 1, 0);
    lv_obj_set_style_border_color(p, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_opa(p, 46, 0);
    lv_obj_set_style_radius(p, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_hor(p, 10, 0);
    lv_obj_set_style_pad_ver(p, 3, 0);
    return p;
}
}  // namespace

namespace {
lv_obj_t* SecondsBar(lv_obj_t* parent, int width) {
    lv_obj_t* bar = lv_bar_create(parent);
    lv_obj_set_size(bar, width, 4);
    lv_bar_set_range(bar, 0, 59);
    lv_obj_set_style_radius(bar, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 2, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(bar, lv_color_hex(kTrack), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_hex(LTKK_COLOR_BLUE), LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_color(bar, lv_color_hex(LTKK_COLOR_ORANGE), LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_dir(bar, LV_GRAD_DIR_HOR, LV_PART_INDICATOR);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    return bar;
}
}  // namespace

namespace {
void StartBreathe(lv_obj_t* obj, int duration) {
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, BreatheExec);
    lv_anim_set_values(&a, 0, 16);
    lv_anim_set_duration(&a, duration);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);
}
}  // namespace

// centered = các viên thông tin xếp ngang (kiểu căn giữa); false = xếp dọc (cột bên phải)
lv_obj_t* LtkkDisplay::CreateInfo(lv_obj_t* parent, InfoWidgets& info, bool centered) {
    lv_obj_t* holder = centered ? Row(parent, 8) : Column(parent, 6);
    lv_obj_set_style_margin_top(holder, centered ? 8 : 4, 0);
    if (centered) {
        info.date_box = Pill(holder, 8);
    } else {
        info.date_box = Column(holder, 1);
    }
    info.weekday = Image(info.date_box, kWeekdays[0]);
    info.date = Label(info.date_box, kSoft);

    info.compact = centered;
    info.weather = Pill(holder, 5);
    if (centered) {
        // gọn: biểu tượng thời tiết nhỏ + nhiệt độ (vừa một hàng với viên ngày)
        info.weather_img = ScaledImage(info.weather, kWeatherBig[1], 0.32f);
    } else {
        Image(info.weather, &ltkk_icon_cloud);
        info.weather_img = Image(info.weather, kWeather[1]);
    }
    info.weather_temp = Label(info.weather, 0xFFFFFF);
    Image(info.weather, &ltkk_txt_degc);
    SetHidden(info.weather, true);
    return holder;
}

// Kiểu 0: Đồng hồ số lớn + thanh giây + viên ngày / thời tiết
void LtkkDisplay::CreateStyleClock(lv_obj_t* panel) {
    lv_obj_t* col = Column(panel, 6);
    lv_obj_set_width(col, LV_PCT(100));
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_align(col, LV_ALIGN_CENTER, 0, -14);
    CreateDigits(col, clock_digits_[0], 1.0f);
    sec_bars_[0] = SecondsBar(col, 236);
    CreateInfo(col, info_[0], true);
}

// Kiểu 1: Đồng hồ kim với vòng màu LTKK (xanh -> cam)
void LtkkDisplay::CreateStyleAnalog(lv_obj_t* panel) {
    constexpr int kDial = 146;
    lv_obj_t* dial = Box(panel);
    lv_obj_set_size(dial, kDial, kDial);
    lv_obj_align(dial, LV_ALIGN_LEFT_MID, 10, 2);
    lv_obj_set_style_radius(dial, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dial, lv_color_hex(0x0B1F4D), 0);
    lv_obj_set_style_bg_grad_color(dial, lv_color_hex(0x050E26), 0);
    lv_obj_set_style_bg_grad_dir(dial, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(dial, 230, 0);

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
    lv_obj_align(right, LV_ALIGN_LEFT_MID, 170, 0);
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

// Kiểu 2: Lu ngủ - Lu thở nhẹ, giờ nhỏ bên dưới
void LtkkDisplay::CreateStyleSleepy(lv_obj_t* panel) {
    sleepy_face_ = ScaledImage(panel, &ltkk_eyes_sleepy, 0.78f);
    lv_obj_align(sleepy_face_, LV_ALIGN_CENTER, 0, -50);
    StartBreathe(sleepy_face_, 3200);

    lv_obj_t* col = Column(panel, 4);
    lv_obj_set_width(col, LV_PCT(100));
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_align(col, LV_ALIGN_CENTER, 0, 34);
    CreateDigits(col, clock_digits_[2], 0.5f);
    CreateInfo(col, info_[2], true);
}

// Kiểu 3: Thẻ LTKK - logo "Học để làm" + giờ
void LtkkDisplay::CreateStyleCard(lv_obj_t* panel) {
    lv_obj_t* logo = ScaledImage(panel, &ltkk_splash_image, 0.62f);
    lv_obj_align(logo, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_t* right = Column(panel, 6);
    lv_obj_align(right, LV_ALIGN_LEFT_MID, 156, 0);
    CreateDigits(right, clock_digits_[3], 0.55f);
    CreateInfo(right, info_[3], false);
}

// Kiểu 4: Lu & bạn - Lu thở nhẹ bên trái, lời chào theo buổi + giờ bên phải
void LtkkDisplay::CreateStyleCompanion(lv_obj_t* panel) {
    lv_obj_t* face = ScaledImage(panel, &ltkk_eyes_neutral, 0.8f);
    lv_obj_align(face, LV_ALIGN_LEFT_MID, 2, -6);
    StartBreathe(face, 2800);

    lv_obj_t* right = Column(panel, 3);
    lv_obj_align(right, LV_ALIGN_LEFT_MID, 144, -6);
    greet_img_ = Image(right, kGreetings[0]);
    lv_obj_t* digits = CreateDigits(right, clock_digits_[4], 0.52f);
    lv_obj_set_style_margin_top(digits, 4, 0);
    CreateInfo(right, info_[4], false);
}

// Kiểu 5: Thời tiết - thẻ kính lớn: biểu tượng, nhiệt độ, thành phố; giờ + ngày bên dưới
void LtkkDisplay::CreateStyleWeather(lv_obj_t* panel) {
    lv_obj_t* card = Box(panel);
    lv_obj_set_size(card, 296, 124);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 30);
    lv_obj_set_style_radius(card, 22, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(card, 26, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_opa(card, 56, 0);

    wx_content_ = Box(card);
    lv_obj_set_size(wx_content_, LV_PCT(100), LV_PCT(100));
    wx_big_img_ = Image(wx_content_, &ltkk_wxbig_1);
    lv_obj_align(wx_big_img_, LV_ALIGN_LEFT_MID, 14, 0);
    lv_obj_t* col = Column(wx_content_, 2);
    lv_obj_align(col, LV_ALIGN_LEFT_MID, 104, 0);
    lv_obj_t* temp_row = Row(col, 0);
    lv_obj_set_flex_align(temp_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    wx_temp_digits_[0] = ScaledImage(temp_row, &ltkk_digit_2, 0.66f);
    wx_temp_digits_[1] = ScaledImage(temp_row, &ltkk_digit_7, 0.66f);
    lv_obj_t* degc = Image(temp_row, &ltkk_txt_degc_big);
    lv_obj_set_style_margin_left(degc, 4, 0);
    wx_cond_img_ = Image(col, kWeather[1]);
    wx_city_label_ = Label(col, kSoft);
    lv_obj_set_width(wx_city_label_, 180);
    lv_label_set_long_mode(wx_city_label_, LV_LABEL_LONG_MODE_DOTS);

    wx_wait_img_ = Image(card, &ltkk_txt_wx_wait);
    lv_obj_center(wx_wait_img_);
    SetHidden(wx_wait_img_, true);

    lv_obj_t* row = Row(panel, 10);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, 164);
    CreateDigits(row, clock_digits_[5], 0.36f);
    lv_obj_t* holder = CreateInfo(row, info_[5], true);
    lv_obj_set_style_margin_top(holder, 0, 0);
    SetHidden(info_[5].weather, true);
    info_[5].weather = nullptr;  // thời tiết đã ở thẻ lớn
}

// Kiểu 6: Lịch - tờ lịch (tháng, ngày, thứ) + giờ bên phải
void LtkkDisplay::CreateStyleCalendar(lv_obj_t* panel) {
    lv_obj_t* card = Box(panel);
    lv_obj_set_size(card, 124, 142);
    lv_obj_align(card, LV_ALIGN_LEFT_MID, 16, -2);
    lv_obj_set_style_radius(card, 18, 0);
    lv_obj_set_style_clip_corner(card, true, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(kTile), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(kCyan), 0);
    lv_obj_set_style_border_opa(card, 90, 0);

    lv_obj_t* band = Box(card);
    lv_obj_set_size(band, LV_PCT(100), 32);
    lv_obj_align(band, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_opa(band, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(band, lv_color_hex(LTKK_COLOR_ORANGE), 0);
    lv_obj_set_style_bg_grad_color(band, lv_color_hex(0xFF9A3D), 0);
    lv_obj_set_style_bg_grad_dir(band, LV_GRAD_DIR_HOR, 0);
    lv_obj_t* head = Row(band, 6);
    lv_obj_center(head);
    Image(head, &ltkk_txt_month);
    cal_month_label_ = Label(head, 0xFFFFFF);

    lv_obj_t* day = Row(card, 0);
    lv_obj_align(day, LV_ALIGN_CENTER, 0, 8);
    cal_day_[0] = ScaledImage(day, &ltkk_digit_0, 0.72f);
    cal_day_[1] = ScaledImage(day, &ltkk_digit_8, 0.72f);
    cal_wday_img_ = Image(card, kWeekdays[0]);
    lv_obj_align(cal_wday_img_, LV_ALIGN_BOTTOM_MID, 0, -8);

    lv_obj_t* right = Column(panel, 6);
    lv_obj_align(right, LV_ALIGN_LEFT_MID, 156, -2);
    CreateDigits(right, clock_digits_[6], 0.5f);
    CreateInfo(right, info_[6], false);
    SetHidden(info_[6].weekday, true);  // thứ đã có trên tờ lịch
}

// Kiểu 7: Ban đêm - nền đen, chữ số xanh dịu, rất ít chi tiết (đỡ chói khi ngủ)
void LtkkDisplay::CreateStyleNight(lv_obj_t* panel) {
    lv_obj_t* black = Box(panel);
    lv_obj_set_size(black, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(black, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(black, LV_OPA_COVER, 0);

    lv_obj_t* col = Column(panel, 6);
    lv_obj_set_width(col, LV_PCT(100));
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_align(col, LV_ALIGN_CENTER, 0, -4);
    lv_obj_t* digits = CreateDigits(col, clock_digits_[7], 0.82f);
    for (lv_obj_t* d : clock_digits_[7]) {
        lv_obj_set_style_image_recolor(d, lv_color_hex(kCyan), 0);
        lv_obj_set_style_image_recolor_opa(d, 110, 0);
    }
    lv_obj_set_style_opa(digits, 150, 0);
    sec_bars_[7] = SecondsBar(col, 150);
    lv_obj_set_style_opa(sec_bars_[7], 120, 0);
    lv_obj_t* info = CreateInfo(col, info_[7], true);
    lv_obj_set_style_opa(info, 120, 0);
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

uint8_t LtkkDisplay::GetStandbyOptions(int style) const {
    return (style >= 0 && style < kStandbyStyles) ? standby_opts_[style].load() : 0;
}

void LtkkDisplay::SetStandbyOptions(int style, uint8_t opts) {
    if (style < 0 || style >= kStandbyStyles) {
        return;
    }
    standby_opts_[style] = opts;
    opts_dirty_ = true;
    Settings settings("ltkk", true);
    settings.SetInt("sbopt" + std::to_string(style), opts);
}

void LtkkDisplay::OpenStandbyEditor() { editor_request_ = true; }

// Áp dụng tuỳ chọn hiển thị cho kiểu đang hiện
void LtkkDisplay::ApplyStandbyOptions(int style) {
    uint8_t o = standby_opts_[style].load();
    InfoWidgets& info = info_[style];
    SetHidden(info.date_box, !(o & kOptDate));
    if (!(o & kOptWeather)) {
        SetHidden(info.weather, true);
    }
    SetHidden(sec_bars_[style], !(o & kOptSeconds));
    if (style == 1) {
        SetHidden(analog_second_, !(o & kOptSeconds));
    }
    if (clock_digits_[style][2] != nullptr) {
        lv_obj_set_style_opa(clock_digits_[style][2], LV_OPA_COVER, 0);
    }
    bool plain = (o & kOptPlain) != 0;
    lv_obj_set_style_bg_image_src(standby_, plain ? nullptr : &ltkk_background, 0);
    bool night = style == 7;
    lv_obj_set_style_opa(gear_btn_, night ? 110 : LV_OPA_COVER, 0);
    lv_obj_set_style_opa(brand_bar_, night ? 60 : LV_OPA_COVER, 0);
    last_weather_key_ = -1000;
}

void LtkkDisplay::ShowStandbyStyle(int style) {
    bool dirty = opts_dirty_.exchange(false);
    for (int i = 0; i < kStandbyStyles; i++) {
        SetHidden(style_panels_[i], i != style);
    }
    if (style != shown_style_ || dirty) {
        shown_style_ = style;
        last_minute_ = -1;
        last_second_ = -1;
        ApplyStandbyOptions(style);
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

    uint8_t o = standby_opts_[style].load();
    bool seconds = (o & kOptSeconds) != 0;
    if ((style == 1 || seconds) && (force || t.tm_sec != last_second_)) {
        last_second_ = t.tm_sec;
        if (style == 1) {
            UpdateAnalog(t);
        }
        if (sec_bars_[style] != nullptr) {
            lv_bar_set_value(sec_bars_[style], t.tm_sec, LV_ANIM_OFF);
        }
        if (seconds && style != 1 && clock_digits_[style][2] != nullptr) {
            // dấu ":" nhấp nháy theo giây
            lv_obj_set_style_opa(clock_digits_[style][2], (t.tm_sec % 2) ? 70 : LV_OPA_COVER, 0);
        }
    }
    int minute_key = t.tm_hour * 60 + t.tm_min;
    if (!force && minute_key == last_minute_) {
        return;
    }
    last_minute_ = minute_key;
    int wday = t.tm_wday == 0 ? 6 : t.tm_wday - 1;
    SetDigits(clock_digits_[style], t.tm_hour, t.tm_min);
    InfoWidgets& info = info_[style];
    lv_image_set_src(info.weekday, kWeekdays[wday]);
    if (info.compact) {
        lv_label_set_text_fmt(info.date, "%02d/%02d", t.tm_mday, t.tm_mon + 1);
    } else {
        lv_label_set_text_fmt(info.date, "%02d/%02d/%04d", t.tm_mday, t.tm_mon + 1, t.tm_year + 1900);
    }

    if (style == 4 && greet_img_ != nullptr) {
        int h = t.tm_hour;
        int g = (h >= 5 && h < 11) ? 0 : (h < 13 && h >= 11) ? 1 : (h >= 13 && h < 18) ? 2
                : (h >= 18 && h < 22) ? 3 : 4;
        lv_image_set_src(greet_img_, kGreetings[g]);
    }
    if (style == 6 && cal_month_label_ != nullptr) {
        lv_label_set_text_fmt(cal_month_label_, "%d", t.tm_mon + 1);
        lv_image_set_src(cal_day_[0], kDigits[t.tm_mday / 10]);
        lv_image_set_src(cal_day_[1], kDigits[t.tm_mday % 10]);
        lv_image_set_src(cal_wday_img_, kWeekdays[wday]);
    }
}

void LtkkDisplay::UpdateWeather() {
    auto& weather = LtkkWeather::GetInstance();
    int style = shown_style_ < 0 ? 0 : shown_style_;
    bool valid = weather.valid();
    int cond = weather.condition();
    cond = (cond >= 0 && cond < 6) ? cond : 1;
    int temp = weather.temperature();
    int key = valid ? cond * 1000 + temp : -999;
    bool changed = key != last_weather_key_;
    last_weather_key_ = key;

    if (style == 5 && wx_content_ != nullptr) {
        SetHidden(wx_content_, !valid);
        SetHidden(wx_wait_img_, valid);
        if (valid && changed) {
            int a = temp < 0 ? -temp : temp;
            lv_image_set_src(wx_big_img_, kWeatherBig[cond]);
            lv_image_set_src(wx_temp_digits_[0], kDigits[(a / 10) % 10]);
            lv_image_set_src(wx_temp_digits_[1], kDigits[a % 10]);
            SetHidden(wx_temp_digits_[0], a < 10);
            lv_image_set_src(wx_cond_img_, kWeather[cond]);
            std::string city = weather.GetCity();
            lv_label_set_text(wx_city_label_, city.c_str());
        }
    }

    InfoWidgets& info = info_[style];
    if (info.weather == nullptr) {
        return;
    }
    if (!valid || !(standby_opts_[style].load() & kOptWeather)) {
        SetHidden(info.weather, true);
        return;
    }
    if (changed) {
        lv_image_set_src(info.weather_img, info.compact ? kWeatherBig[cond] : kWeather[cond]);
        lv_label_set_text_fmt(info.weather_temp, "%d", temp);
    }
    SetHidden(info.weather, false);
}

// ----- bảng tuỳ chỉnh màn hình chờ (mép dưới, xem trước trực tiếp)
void LtkkDisplay::CreateStandbyEditor() {
    editor_ = Box(standby_);
    lv_obj_set_size(editor_, LV_PCT(100), 100);
    lv_obj_align(editor_, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(editor_, lv_color_hex(0x081738), 0);
    lv_obj_set_style_bg_opa(editor_, 245, 0);
    lv_obj_set_style_radius(editor_, 18, 0);
    lv_obj_set_style_border_side(editor_, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(editor_, 2, 0);
    lv_obj_set_style_border_color(editor_, lv_color_hex(kCyan), 0);
    lv_obj_set_style_border_opa(editor_, 140, 0);
    MakeInteractive(editor_);  // chạm vùng trống của bảng: không gọi Lu

    ed_prev_ = MakeButton(editor_, 40, 34, kTile, false);
    lv_obj_align(ed_prev_, LV_ALIGN_TOP_LEFT, 10, 10);
    lv_obj_center(Image(ed_prev_, &ltkk_icon_prev));
    lv_obj_t* name_box = Box(editor_);
    lv_obj_set_size(name_box, 138, 34);
    lv_obj_align(name_box, LV_ALIGN_TOP_LEFT, 54, 10);
    ed_name_ = Image(name_box, kStyleBig[0]);
    lv_obj_center(ed_name_);
    ed_next_ = MakeButton(editor_, 40, 34, kTile, false);
    lv_obj_align(ed_next_, LV_ALIGN_TOP_LEFT, 196, 10);
    lv_obj_center(Image(ed_next_, &ltkk_icon_next));
    ed_done_ = MakeButton(editor_, 66, 34, LTKK_COLOR_ORANGE, false);
    lv_obj_align(ed_done_, LV_ALIGN_TOP_RIGHT, -10, 10);
    lv_obj_center(Image(ed_done_, &ltkk_txt_done));

    static const lv_image_dsc_t* const kChipText[4] = {&ltkk_chip_0, &ltkk_chip_1, &ltkk_chip_2,
                                                       &ltkk_chip_3};
    for (int i = 0; i < 4; i++) {
        ed_chips_[i] = MakeButton(editor_, 72, 36, kTile, false);
        lv_obj_set_style_radius(ed_chips_[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_align(ed_chips_[i], LV_ALIGN_TOP_LEFT, 10 + i * 76, 54);
        lv_obj_center(Image(ed_chips_[i], kChipText[i]));
    }
    SetHidden(editor_, true);
}

void LtkkDisplay::ShowStandbyEditor(bool show) {
    if (editor_ == nullptr) {
        return;
    }
    if (show) {
        settings_last_ms_ = lv_tick_get();
        editor_key_ = -1;
        RefreshStandbyEditor();
    }
    SetHidden(editor_, !show);
}

void LtkkDisplay::RefreshStandbyEditor() {
    int style = standby_style_.load();
    uint8_t o = standby_opts_[style].load();
    int key = style * 256 + o;
    if (key == editor_key_) {
        return;
    }
    editor_key_ = key;
    lv_image_set_src(ed_name_, kStyleBig[style]);
    static const uint8_t kBits[4] = {kOptWeather, kOptDate, kOptSeconds, kOptPlain};
    for (int i = 0; i < 4; i++) {
        bool on = (o & kBits[i]) != 0;
        lv_obj_set_style_bg_color(ed_chips_[i], lv_color_hex(on ? LTKK_COLOR_ORANGE : kTile), 0);
        lv_obj_set_style_border_width(ed_chips_[i], on ? 0 : 1, 0);
        lv_obj_set_style_border_color(ed_chips_[i], lv_color_hex(kCyan), 0);
        lv_obj_set_style_border_opa(ed_chips_[i], 120, 0);
    }
}

void LtkkDisplay::ToggleStandbyOption(int chip) {
    static const uint8_t kBits[4] = {kOptWeather, kOptDate, kOptSeconds, kOptPlain};
    int style = standby_style_.load();
    SetStandbyOptions(style, standby_opts_[style].load() ^ kBits[chip]);
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
// Ảnh bìa (YouTube: ảnh thu nhỏ của video do Lu Manager gửi) + tên bài chạy chữ + ca sĩ
// + sóng nhạc nhảy theo nhịp + thanh tiến trình + nút điều khiển.
void LtkkDisplay::CreateMusic(lv_obj_t* parent) {
    music_panel_ = Box(parent);
    lv_obj_set_size(music_panel_, LV_PCT(100), LV_PCT(100));

    lv_obj_t* art = Box(music_panel_);
    lv_obj_set_size(art, 112, 112);
    lv_obj_align(art, LV_ALIGN_TOP_LEFT, 14, 34);
    lv_obj_set_style_radius(art, 18, 0);
    lv_obj_set_style_clip_corner(art, true, 0);
    lv_obj_set_style_bg_opa(art, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(art, lv_color_hex(LTKK_COLOR_BLUE), 0);
    lv_obj_set_style_bg_grad_color(art, lv_color_hex(LTKK_COLOR_ORANGE), 0);
    lv_obj_set_style_bg_grad_dir(art, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_border_width(art, 2, 0);
    lv_obj_set_style_border_color(art, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_opa(art, 70, 0);
    music_art_icon_ = Image(art, &ltkk_icon_note);
    lv_obj_center(music_art_icon_);
    music_cover_ = lv_image_create(art);
    lv_obj_remove_flag(music_cover_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(music_cover_);
    SetHidden(music_cover_, true);
    cover_buf_ = static_cast<uint8_t*>(
        heap_caps_malloc(LtkkMusic::kCoverBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (cover_buf_ == nullptr) {
        cover_buf_ = static_cast<uint8_t*>(heap_caps_malloc(LtkkMusic::kCoverBytes, MALLOC_CAP_8BIT));
    }
    cover_dsc_.header.magic = LV_IMAGE_HEADER_MAGIC;
    cover_dsc_.header.cf = LV_COLOR_FORMAT_RGB565;
    cover_dsc_.header.w = LtkkMusic::kCoverSize;
    cover_dsc_.header.h = LtkkMusic::kCoverSize;
    cover_dsc_.header.stride = LtkkMusic::kCoverSize * 2;
    cover_dsc_.data_size = LtkkMusic::kCoverBytes;
    cover_dsc_.data = cover_buf_;

    lv_obj_t* info = Column(music_panel_, 4);
    lv_obj_align(info, LV_ALIGN_TOP_LEFT, 138, 36);
    music_kind_img_ = Image(info, &ltkk_txt_music);
    music_title_ = Label(info, 0xFFFFFF);
    lv_obj_set_width(music_title_, 170);
    lv_label_set_long_mode(music_title_, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    music_artist_ = Label(info, kSoft);
    lv_obj_set_width(music_artist_, 170);
    lv_label_set_long_mode(music_artist_, LV_LABEL_LONG_MODE_DOTS);
    SetHidden(music_artist_, true);
    music_sub_img_ = Image(info, &ltkk_txt_sdcard);

    // sóng nhạc: 6 vạch nhảy lệch nhịp
    music_eq_ = Row(info, 3);
    lv_obj_set_height(music_eq_, 20);
    lv_obj_set_flex_align(music_eq_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_margin_top(music_eq_, 4, 0);
    for (int i = 0; i < kEqBars; i++) {
        lv_obj_t* bar = Box(music_eq_);
        lv_obj_set_size(bar, 4, 4);
        lv_obj_set_style_radius(bar, 2, 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(bar, lv_color_hex(i % 2 ? kCyan : LTKK_COLOR_ORANGE), 0);
        music_eq_bars_[i] = bar;
    }

    music_bar_ = lv_bar_create(music_panel_);
    lv_obj_set_size(music_bar_, 292, 5);
    lv_obj_align(music_bar_, LV_ALIGN_TOP_LEFT, 14, 156);
    lv_bar_set_range(music_bar_, 0, 1000);
    lv_obj_remove_flag(music_bar_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(music_bar_, lv_color_hex(kTrack), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(music_bar_, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(music_bar_, lv_color_hex(LTKK_COLOR_BLUE), LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_color(music_bar_, lv_color_hex(LTKK_COLOR_ORANGE), LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_dir(music_bar_, LV_GRAD_DIR_HOR, LV_PART_INDICATOR);

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

void LtkkDisplay::SetEqualizer(bool running) {
    if (running == eq_running_) {
        return;
    }
    eq_running_ = running;
    for (int i = 0; i < kEqBars; i++) {
        lv_obj_t* bar = music_eq_bars_[i];
        lv_anim_delete(bar, EqExec);
        if (!running) {
            lv_obj_set_height(bar, 4);
            continue;
        }
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, bar);
        lv_anim_set_exec_cb(&a, EqExec);
        lv_anim_set_values(&a, 0, 32);
        lv_anim_set_duration(&a, 420 + i * 97 % 260);
        lv_anim_set_delay(&a, i * 70);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_start(&a);
    }
}

void LtkkDisplay::UpdateMusic() {
    auto& music = LtkkMusic::GetInstance();
    // 0 = thẻ nhớ, 1 = radio, 2 = bài hát trực tuyến, 3 = loa máy tính, 4 = YouTube
    int mode = music.IsRadio() ? 1 : (music.IsSong() ? (music.IsYoutube() ? 4 : 2) : (music.IsSpeaker() ? 3 : 0));
    int key = mode + (music.IsBuffering() ? 10 : 0);
    if (key != music_mode_shown_) {
        music_mode_shown_ = key;
        lv_image_set_src(music_kind_img_, mode == 0   ? &ltkk_txt_music
                                          : mode == 1 ? &ltkk_txt_radio
                                          : mode == 2 ? &ltkk_txt_online
                                          : mode == 4 ? &ltkk_txt_youtube
                                                      : &ltkk_txt_speaker);
        lv_image_set_src(music_sub_img_, music.IsBuffering() ? &ltkk_txt_connecting
                                         : mode == 0         ? &ltkk_txt_sdcard
                                         : mode == 1         ? &ltkk_txt_live
                                         : mode == 2         ? &ltkk_txt_server
                                         : mode == 4         ? &ltkk_txt_youtube_sub
                                                             : &ltkk_txt_speaker_sub);
        lv_image_set_src(music_art_icon_, mode == 1 ? &ltkk_icon_radio : &ltkk_icon_note);
        lv_image_set_scale(music_art_icon_, mode == 1 ? 420 : 256);
    }
    std::string title = music.CurrentTitle();
    if (title != music_title_text_) {
        music_title_text_ = title;
        lv_label_set_text(music_title_, title.c_str());
    }
    std::string artist = music.CurrentArtist();
    if (artist != music_artist_text_) {
        music_artist_text_ = artist;
        lv_label_set_text(music_artist_, artist.c_str());
        SetHidden(music_artist_, artist.empty());
    }
    SetHidden(music_sub_img_, !artist.empty() && !music.IsBuffering());

    // Ảnh bìa: chép sang bộ nhớ riêng của màn hình rồi hiển thị
    int version = music.CoverVersion();
    if (version != cover_version_shown_) {
        cover_version_shown_ = version;
        bool has = cover_buf_ != nullptr && music.CopyCover(cover_buf_, LtkkMusic::kCoverBytes);
        lv_image_cache_drop(&cover_dsc_);
        if (has) {
            lv_image_set_src(music_cover_, &cover_dsc_);
            lv_obj_invalidate(music_cover_);
        }
        SetHidden(music_cover_, !has);
        SetHidden(music_art_icon_, has);
    }

    lv_bar_set_value(music_bar_, mode == 0 ? music.ProgressPermille() : 1000, LV_ANIM_OFF);
    bool paused = music.IsPaused();
    if (paused != music_paused_shown_) {
        music_paused_shown_ = paused;
        lv_image_set_src(music_play_img_, paused ? &ltkk_icon_play : &ltkk_icon_pause);
    }
    SetEqualizer(!paused && !music.IsBuffering());
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

void LtkkDisplay::ShowTouchCalibration(int step) {
    DisplayLockGuard lock(this);
    if (calib_panel_ == nullptr) {
        calib_panel_ = Box(lv_layer_top());
        lv_obj_set_size(calib_panel_, LV_PCT(100), LV_PCT(100));
        lv_obj_set_style_bg_color(calib_panel_, lv_color_hex(0x081738), 0);
        lv_obj_set_style_bg_opa(calib_panel_, LV_OPA_COVER, 0);
        lv_obj_t* col = Column(calib_panel_, 6);
        lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_width(col, LV_PCT(100));
        lv_obj_align(col, LV_ALIGN_CENTER, 0, 0);
        calib_text_ = Image(col, &ltkk_txt_cal_title);
        Image(col, &ltkk_txt_cal_tap);
        calib_step_label_ = Label(col, LTKK_COLOR_ORANGE);

        // Dấu + : vòng tròn + 2 vạch
        calib_target_ = Box(calib_panel_);
        lv_obj_set_size(calib_target_, 40, 40);
        lv_obj_t* ring = Box(calib_target_);
        lv_obj_set_size(ring, 26, 26);
        lv_obj_center(ring);
        lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(ring, 3, 0);
        lv_obj_set_style_border_color(ring, lv_color_hex(LTKK_COLOR_ORANGE), 0);
        for (int i = 0; i < 2; i++) {
            lv_obj_t* bar = Box(calib_target_);
            lv_obj_set_size(bar, i == 0 ? 40 : 3, i == 0 ? 3 : 40);
            lv_obj_center(bar);
            lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(bar, lv_color_hex(0xFFFFFF), 0);
        }
    }
    if (step == 0) {
        // Xong: báo 1,5 giây rồi ẩn
        lv_image_set_src(calib_text_, &ltkk_txt_cal_done);
        lv_label_set_text(calib_step_label_, "");
        SetHidden(calib_target_, true);
        lv_obj_fade_out(calib_panel_, 300, 1500);
        return;
    }
    lv_anim_delete(calib_panel_, nullptr);
    lv_obj_set_style_opa(calib_panel_, LV_OPA_COVER, 0);
    lv_image_set_src(calib_text_, &ltkk_txt_cal_title);
    lv_label_set_text_fmt(calib_step_label_, "%d / 2", step);
    SetHidden(calib_target_, false);
    // Dấu 1: góc trên-trái, dấu 2: góc dưới-phải
    if (step == 1) {
        lv_obj_set_pos(calib_target_, 30 - 20, 30 - 20);
    } else {
        lv_obj_set_pos(calib_target_, LV_HOR_RES - 30 - 20, LV_VER_RES - 30 - 20);
    }
    SetHidden(calib_panel_, false);
    lv_obj_move_foreground(calib_panel_);
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
    if (demo_index_ >= 0) {
        StopEmotionDemo();
        return true;
    }
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
    } else if (dir == kSwipeUp && !IsHidden(standby_)) {
        demo_key_.clear();
        demo_request_ = true;
    } else if ((dir == kSwipeLeft || dir == kSwipeRight) && !settings_open && !IsHidden(standby_)) {
        SetStandbyStyle(standby_style_.load() + (dir == kSwipeLeft ? 1 : -1));
        if (!IsHidden(editor_)) {
            settings_last_ms_ = lv_tick_get();
            RefreshStandbyEditor();
        }
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
        OpenSettings(false);
        ShowStandbyEditor(true);
    } else if (t == ed_prev_ || t == ed_next_) {
        SetStandbyStyle(standby_style_.load() + (t == ed_next_ ? 1 : -1));
        RefreshStandbyEditor();
    } else if (t == ed_done_) {
        ShowStandbyEditor(false);
    } else if (t == ed_chips_[0] || t == ed_chips_[1] || t == ed_chips_[2] || t == ed_chips_[3]) {
        for (int i = 0; i < 4; i++) {
            if (t == ed_chips_[i]) {
                ToggleStandbyOption(i);
            }
        }
        RefreshStandbyEditor();
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
// Trả về true khi đang trình diễn cảm xúc (bỏ qua cập nhật thường lệ)
bool LtkkDisplay::OnDemoTick() {
    uint32_t now_ms = lv_tick_get();
    if (demo_request_.exchange(false)) {
        demo_index_ = 0;
        demo_end_ = kDemoCount;
        for (int i = 0; i < kDemoCount && !demo_key_.empty(); i++) {
            if (demo_key_ == kDemoFaces[i].key) {
                demo_index_ = i;
                demo_end_ = i + 1;
            }
        }
        demo_next_ms_ = now_ms;
        OpenSettings(false);
        SetHidden(standby_, true);
        SetHidden(wave_box_, true);
        SetHidden(listening_label_, true);
        if (emoji_box_ != nullptr) {
            lv_obj_align(emoji_box_, LV_ALIGN_CENTER, 0, -16);
        }
    }
    if (demo_index_ < 0 || face_ == nullptr) {
        return false;
    }
    // Bạn gọi Lu -> dừng trình diễn
    if (Application::GetInstance().GetDeviceState() == kDeviceStateListening) {
        StopEmotionDemo();
        return false;
    }
    if ((int32_t)(now_ms - demo_next_ms_) < 0) {
        return true;
    }
    if (demo_index_ >= demo_end_) {
        StopEmotionDemo();
        return false;
    }
    const auto& f = kDemoFaces[demo_index_];
    SetEyes(f.eyes);
    lv_image_set_scale(face_, 256);
    blink_enabled_ = false;
    ShowEmotionTag(f.label);
    bool single = demo_end_ - demo_index_ == 1 && demo_end_ != kDemoCount;
    uint32_t hold = single ? 5000 : 2200;
    emo_tag_until_ms_ = now_ms + hold;
    demo_next_ms_ = now_ms + hold;
    demo_index_++;
    return true;
}

void LtkkDisplay::TickCallback(lv_timer_t* timer) {
    static_cast<LtkkDisplay*>(lv_timer_get_user_data(timer))->OnTick();
}

void LtkkDisplay::OnTick() {
    if (OnDemoTick()) {
        return;
    }
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
        SetHidden(hint_pill_, !show_clock || standby_style_.load() == 7 || !IsHidden(editor_));
        if (!show_music) {
            SetEqualizer(false);
        }
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
        ShowStandbyEditor(false);
    }

    // Thanh trên cùng: màn hình chờ đã có đồng hồ lớn -> ẩn giờ nhỏ ở thanh trên
    bool standby_on = !IsHidden(standby_);
    if (status_label_ != nullptr && standby_on != status_hidden_) {
        status_hidden_ = standby_on;
        lv_obj_set_style_opa(status_label_, standby_on ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
    }
    bool night = standby_on && standby_style_.load() == 7;
    if (top_bar_ != nullptr && night != top_dimmed_) {
        top_dimmed_ = night;
        lv_obj_set_style_opa(top_bar_, night ? 90 : LV_OPA_COVER, 0);
    }

    // Bảng tuỳ chỉnh màn hình chờ
    if (editor_request_.exchange(false) && idle) {
        OpenSettings(false);
        ShowStandbyEditor(true);
    }
    if (!IsHidden(editor_)) {
        if (lv_tick_elaps(settings_last_ms_) > 30000) {
            ShowStandbyEditor(false);
        } else {
            RefreshStandbyEditor();
        }
    }

    // Khuôn mặt và sóng âm
    if (state_changed && face_ != nullptr) {
        if (listening) {
            SetEyes(&ltkk_eyes_listen);
            ShowEmotionTag(nullptr);
            lv_obj_align(emoji_box_, LV_ALIGN_CENTER, 0, -32);
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
    if (face_ != nullptr && blink_until_ms_ != 0 && (int32_t)(now_ms - blink_until_ms_) >= 0) {
        blink_until_ms_ = 0;
        lv_image_set_src(face_, current_eyes_);   // mở mắt lại
    }
    if (emo_tag_ != nullptr && emo_tag_until_ms_ != 0 && (int32_t)(now_ms - emo_tag_until_ms_) >= 0) {
        emo_tag_until_ms_ = 0;
        SetHidden(emo_tag_, true);
    }
    if (face_ != nullptr && !idle && blink_enabled_ && blink_until_ms_ == 0 &&
        (int32_t)(now_ms - next_blink_ms_) >= 0) {
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
