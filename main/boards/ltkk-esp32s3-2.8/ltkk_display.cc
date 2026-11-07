#include "ltkk_display.h"

#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>

#include <cstring>
#include <ctime>

#include "application.h"
#include "assets/lang_config.h"
#include "ltkk_brand.h"
#include "ltkk_ui_images.h"

#define TAG "LtkkDisplay"

extern "C" {
extern const lv_image_dsc_t ltkk_background;  // ltkk_background.c
}

namespace {

const lv_image_dsc_t* const kDigits[10] = {
    &ltkk_digit_0, &ltkk_digit_1, &ltkk_digit_2, &ltkk_digit_3, &ltkk_digit_4,
    &ltkk_digit_5, &ltkk_digit_6, &ltkk_digit_7, &ltkk_digit_8, &ltkk_digit_9,
};

const lv_image_dsc_t* const kWeekdays[7] = {
    &ltkk_wday_0, &ltkk_wday_1, &ltkk_wday_2, &ltkk_wday_3,
    &ltkk_wday_4, &ltkk_wday_5, &ltkk_wday_6,  // Thứ Hai ... Chủ Nhật
};

// Tên cảm xúc của Xiaozhi -> đôi mắt của Lu
struct EmotionMap {
    const char* name;
    const lv_image_dsc_t* eyes;
};
const EmotionMap kEmotions[] = {
    {"neutral", &ltkk_eyes_neutral},   {"happy", &ltkk_eyes_happy},
    {"laughing", &ltkk_eyes_happy},    {"funny", &ltkk_eyes_happy},
    {"delicious", &ltkk_eyes_happy},   {"confident", &ltkk_eyes_happy},
    {"cool", &ltkk_eyes_happy},        {"relaxed", &ltkk_eyes_happy},
    {"embarrassed", &ltkk_eyes_happy}, {"sad", &ltkk_eyes_sad},
    {"crying", &ltkk_eyes_sad},        {"angry", &ltkk_eyes_angry},
    {"loving", &ltkk_eyes_love},       {"kissy", &ltkk_eyes_love},
    {"surprised", &ltkk_eyes_surprise}, {"shocked", &ltkk_eyes_surprise},
    {"thinking", &ltkk_eyes_think},    {"confused", &ltkk_eyes_think},
    {"winking", &ltkk_eyes_wink},      {"silly", &ltkk_eyes_wink},
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

lv_obj_t* CreatePlainBox(lv_obj_t* parent) {
    lv_obj_t* obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    return obj;
}

// Ảnh co giãn đúng kích thước (để bố cục flex tính đúng)
lv_obj_t* CreateScaledImage(lv_obj_t* parent, const lv_image_dsc_t* src, float scale) {
    lv_obj_t* img = lv_image_create(parent);
    lv_image_set_src(img, src);
    lv_obj_set_size(img, (int32_t)(src->header.w * scale), (int32_t)(src->header.h * scale));
    lv_image_set_inner_align(img, LV_IMAGE_ALIGN_STRETCH);
    return img;
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
    CreateStandby();
    tick_timer_ = lv_timer_create(TickCallback, 200, this);
    ESP_LOGI(TAG, "Lu UI ready");
}

// ------------------------------------------------------------------ khuôn mặt
void LtkkDisplay::CreateFace() {
    // Ẩn emoji gốc, đặt đôi mắt của Lu vào cùng hộp (giữ nguyên thứ tự lớp)
    if (emoji_label_ != nullptr) {
        lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
    }
    if (emoji_image_ != nullptr) {
        lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
    }
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
    // Chỉ chớp với mắt dạng "mở"
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
    if (emotion == nullptr || face_ == nullptr) {
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
    wave_box_ = CreatePlainBox(screen);
    lv_obj_set_size(wave_box_, LV_PCT(100), 40);
    lv_obj_align(wave_box_, LV_ALIGN_CENTER, 0, 34);
    lv_obj_set_flex_flow(wave_box_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(wave_box_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(wave_box_, 5, 0);

    for (int i = 0; i < kWaveBars; i++) {
        lv_obj_t* bar = CreatePlainBox(wave_box_);
        lv_obj_set_size(bar, 6, 10);
        lv_obj_set_style_radius(bar, 3, 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(
            bar, lv_color_hex(i % 3 == 0 ? LTKK_COLOR_ORANGE : 0x38C8F5), 0);

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

    listening_label_ = lv_image_create(screen);
    lv_image_set_src(listening_label_, &ltkk_txt_listening);
    lv_obj_align(listening_label_, LV_ALIGN_CENTER, 0, 72);

    // Đặt dưới thanh trạng thái và phụ đề của Xiaozhi
    lv_obj_move_to_index(wave_box_, lv_obj_get_index(top_bar_));
    lv_obj_move_to_index(listening_label_, lv_obj_get_index(top_bar_));
    lv_obj_add_flag(wave_box_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(listening_label_, LV_OBJ_FLAG_HIDDEN);
}

// ------------------------------------------------------------------ màn hình chờ
void LtkkDisplay::CreateStandby() {
    lv_obj_t* screen = lv_screen_active();
    standby_ = CreatePlainBox(screen);
    lv_obj_set_size(standby_, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(standby_, lv_color_hex(LTKK_COLOR_NAVY), 0);
    lv_obj_set_style_bg_opa(standby_, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_image_src(standby_, &ltkk_background, 0);

    // --- Đồng hồ
    clock_box_ = CreatePlainBox(standby_);
    lv_obj_set_size(clock_box_, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_align(clock_box_, LV_ALIGN_CENTER, 0, -10);
    lv_obj_set_flex_flow(clock_box_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(clock_box_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(clock_box_, 6, 0);

    lv_obj_t* digits_row = CreatePlainBox(clock_box_);
    lv_obj_set_size(digits_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(digits_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(digits_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    for (int i = 0; i < 5; i++) {
        clock_digits_[i] = lv_image_create(digits_row);
        lv_image_set_src(clock_digits_[i], i == 2 ? &ltkk_digit_colon : &ltkk_digit_0);
    }

    lv_obj_t* date_row = CreatePlainBox(clock_box_);
    lv_obj_set_size(date_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(date_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(date_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(date_row, 8, 0);
    weekday_img_ = lv_image_create(date_row);
    lv_image_set_src(weekday_img_, kWeekdays[0]);
    date_label_ = lv_label_create(date_row);
    lv_obj_set_style_text_color(date_label_, lv_color_hex(0xC8DCFF), 0);
    lv_label_set_text(date_label_, "");

    // --- Hẹn giờ
    timer_box_ = CreatePlainBox(standby_);
    lv_obj_set_size(timer_box_, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_align(timer_box_, LV_ALIGN_CENTER, 0, -6);
    lv_obj_set_flex_flow(timer_box_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(timer_box_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(timer_box_, 6, 0);

    lv_obj_t* timer_title = lv_image_create(timer_box_);
    lv_image_set_src(timer_title, &ltkk_txt_timer);

    timer_arc_ = lv_arc_create(timer_box_);
    lv_obj_set_size(timer_arc_, 140, 140);
    lv_arc_set_rotation(timer_arc_, 270);
    lv_arc_set_bg_angles(timer_arc_, 0, 360);
    lv_arc_set_range(timer_arc_, 0, 1000);
    lv_arc_set_value(timer_arc_, 1000);
    lv_obj_remove_style(timer_arc_, nullptr, LV_PART_KNOB);
    lv_obj_remove_flag(timer_arc_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(timer_arc_, 10, LV_PART_MAIN);
    lv_obj_set_style_arc_width(timer_arc_, 10, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(timer_arc_, lv_color_hex(0x2A3F75), LV_PART_MAIN);
    lv_obj_set_style_arc_color(timer_arc_, lv_color_hex(LTKK_COLOR_ORANGE), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(timer_arc_, true, LV_PART_INDICATOR);

    lv_obj_t* arc_center = CreatePlainBox(timer_arc_);
    lv_obj_set_size(arc_center, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_center(arc_center);
    lv_obj_set_flex_flow(arc_center, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(arc_center, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(arc_center, 2, 0);
    lv_obj_t* timer_row = CreatePlainBox(arc_center);
    lv_obj_set_size(timer_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(timer_row, LV_FLEX_FLOW_ROW);
    for (int i = 0; i < 5; i++) {
        timer_digits_[i] =
            CreateScaledImage(timer_row, i == 2 ? &ltkk_digit_colon : &ltkk_digit_0, 0.45f);
    }
    lv_obj_t* remaining = lv_image_create(arc_center);
    lv_image_set_src(remaining, &ltkk_txt_remaining);
    lv_obj_add_flag(timer_box_, LV_OBJ_FLAG_HIDDEN);

    // --- Hết giờ
    timeup_img_ = lv_image_create(standby_);
    lv_image_set_src(timeup_img_, &ltkk_txt_timeup);
    lv_obj_align(timeup_img_, LV_ALIGN_TOP_MID, 0, 44);
    lv_obj_add_flag(timeup_img_, LV_OBJ_FLAG_HIDDEN);

    // --- Gợi ý gọi Lu
    hint_pill_ = CreatePlainBox(standby_);
    lv_obj_set_size(hint_pill_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(hint_pill_, lv_color_hex(LTKK_COLOR_ORANGE), 0);
    lv_obj_set_style_bg_opa(hint_pill_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(hint_pill_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_hor(hint_pill_, 14, 0);
    lv_obj_set_style_pad_ver(hint_pill_, 6, 0);
    lv_obj_align(hint_pill_, LV_ALIGN_BOTTOM_MID, 0, -12);
    lv_obj_t* hint = lv_image_create(hint_pill_);
    lv_image_set_src(hint, &ltkk_txt_hint);

    // Thanh màu thương hiệu ở mép dưới
    lv_obj_t* bar = CreatePlainBox(standby_);
    lv_obj_set_size(bar, LV_PCT(100), 3);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(LTKK_COLOR_BLUE), 0);
    lv_obj_set_style_bg_grad_color(bar, lv_color_hex(LTKK_COLOR_ORANGE), 0);
    lv_obj_set_style_bg_grad_dir(bar, LV_GRAD_DIR_HOR, 0);

    // Nằm trên khuôn mặt, dưới thanh trạng thái / phụ đề / thông báo của Xiaozhi
    lv_obj_move_to_index(standby_, lv_obj_get_index(top_bar_));
    lv_obj_add_flag(standby_, LV_OBJ_FLAG_HIDDEN);
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
    bool valid = (t.tm_year + 1900) >= 2025;  // đã đồng bộ giờ từ máy chủ
    if (!valid) {
        lv_obj_add_flag(clock_box_, LV_OBJ_FLAG_HIDDEN);
        last_minute_ = -1;
        return;
    }
    lv_obj_remove_flag(clock_box_, LV_OBJ_FLAG_HIDDEN);
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

// ------------------------------------------------------------------ hẹn giờ
void LtkkDisplay::StartStudyTimer(int minutes) {
    int64_t total = (int64_t)minutes * 60 * 1000000LL;
    timer_total_us_ = total;
    timer_end_us_ = esp_timer_get_time() + total;
    ESP_LOGI(TAG, "Study timer: %d min", minutes);
}

void LtkkDisplay::CancelStudyTimer() { timer_end_us_ = 0; }

int LtkkDisplay::StudyTimerRemainingSeconds() const {
    int64_t end = timer_end_us_.load();
    if (end <= 0) {
        return 0;
    }
    int64_t left = end - esp_timer_get_time();
    return left > 0 ? (int)((left + 999999) / 1000000) : 0;
}

void LtkkDisplay::UpdateTimer(int64_t now_us) {
    int64_t end = timer_end_us_.load();
    bool running = end > 0;

    if (running && now_us >= end) {
        // Hết giờ: báo âm thanh và hiện dòng chữ 15 giây
        timer_end_us_ = 0;
        running = false;
        timeup_until_us_ = now_us + 15 * 1000000LL;
        SetEyes(&ltkk_eyes_happy);
        Application::GetInstance().Schedule([]() {
            Application::GetInstance().PlaySound(Lang::Sounds::OGG_POPUP);
        });
    }

    bool timeup = now_us < timeup_until_us_;
    if (timeup) {
        lv_obj_remove_flag(timeup_img_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(timeup_img_, LV_OBJ_FLAG_HIDDEN);
    }

    if (running) {
        lv_obj_add_flag(clock_box_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(timer_box_, LV_OBJ_FLAG_HIDDEN);
        int left = (int)((end - now_us + 999999) / 1000000);
        if (left != last_timer_sec_) {
            last_timer_sec_ = left;
            int mm = left / 60;
            if (mm > 99) {
                mm = 99;
            }
            SetDigits(timer_digits_, mm, left % 60);
            int64_t total = timer_total_us_.load();
            int32_t permille = total > 0 ? (int32_t)((end - now_us) * 1000 / total) : 0;
            lv_arc_set_value(timer_arc_, permille);
        }
    } else {
        lv_obj_add_flag(timer_box_, LV_OBJ_FLAG_HIDDEN);
        last_timer_sec_ = -1;
        UpdateClock(false);
    }
}

// ------------------------------------------------------------------ vòng lặp giao diện
void LtkkDisplay::TickCallback(lv_timer_t* timer) {
    static_cast<LtkkDisplay*>(lv_timer_get_user_data(timer))->OnTick();
}

void LtkkDisplay::OnTick() {
    auto state = Application::GetInstance().GetDeviceState();
    int64_t now_us = esp_timer_get_time();
    bool state_changed = (int)state != last_state_;
    last_state_ = (int)state;

    bool idle = (state == kDeviceStateIdle);
    bool listening = (state == kDeviceStateListening);

    // Màn hình chờ (đồng hồ / hẹn giờ)
    if (idle) {
        if (state_changed) {
            lv_obj_remove_flag(standby_, LV_OBJ_FLAG_HIDDEN);
            UpdateClock(true);
        }
        UpdateTimer(now_us);
    } else {
        if (state_changed) {
            lv_obj_add_flag(standby_, LV_OBJ_FLAG_HIDDEN);
        }
        UpdateTimer(now_us);  // vẫn đếm ngược khi đang trò chuyện
    }

    // Khuôn mặt và sóng âm
    if (state_changed) {
        if (listening) {
            SetEyes(&ltkk_eyes_listen);
            lv_obj_align(emoji_box_, LV_ALIGN_CENTER, 0, -28);
            lv_obj_remove_flag(wave_box_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(listening_label_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(wave_box_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(listening_label_, LV_OBJ_FLAG_HIDDEN);
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
    uint32_t now_ms = lv_tick_get();
    if (!idle && blink_enabled_ && (int32_t)(now_ms - next_blink_ms_) >= 0) {
        Blink();
        next_blink_ms_ = now_ms + 3000 + esp_random() % 3000;
    }
}
