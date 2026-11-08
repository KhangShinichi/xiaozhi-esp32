#pragma once

// Giao diện Lu - LTKK cho Xiaozhi (màn hình 320x240)
//  1. Khởi động  : logo LTKK + "Học để làm"            (ltkk_brand.cc)
//  2. Chờ        : đồng hồ, thứ/ngày, thời tiết, gợi ý gọi Lu
//  3. Đang nghe  : mắt mở to + sóng âm + "Lu đang nghe nè…"
//  4. Đang nói   : biểu cảm + khung phụ đề có nhãn LU / BẠN
//  6. Hẹn giờ học: vòng đếm ngược, nút Tạm dừng / Huỷ
//  7. Phát nhạc  : nhạc .ogg từ thẻ microSD, nút lùi / dừng / tiếp
//  8. Cài Wi-Fi  : mã QR + 3 bước
//  9. 10 biểu cảm robot, tự chớp mắt
// 10. Cài đặt nhanh: vuốt xuống (hoặc giữ 1-3 giây) – âm lượng, độ sáng, giao diện, Wi-Fi, pin
// 11. Pin yếu    : khung thông báo trắng

#include <atomic>
#include <string>

#include "display/lcd_display.h"

class LtkkDisplay : public SpiLcdDisplay {
public:
    LtkkDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y,
                bool swap_xy);
    ~LtkkDisplay();

    void SetupUI() override;
    bool SetEmotion(const char* emotion) override;
    void SetChatMessage(const char* role, const char* content) override;
    void SetTheme(Theme* theme) override;

    // Hẹn giờ học - gọi được từ bất kỳ luồng nào
    void StartStudyTimer(int minutes);
    void CancelStudyTimer();
    void PauseResumeStudyTimer();
    bool IsStudyTimerRunning() const;
    int StudyTimerRemainingSeconds() const;

    // Cảm ứng: bo mạch đưa toạ độ màn hình vào đây (luồng cảm ứng)
    void FeedTouch(bool pressed, int x, int y);
    // true nếu lần chạm vừa rồi đã được giao diện LTKK xử lý (nút, thanh trượt...)
    bool TakeTouchConsumed();
    void ToggleQuickSettings();

private:
    // Khuôn mặt
    lv_obj_t* face_ = nullptr;
    const lv_image_dsc_t* current_eyes_ = nullptr;
    const lv_image_dsc_t* speaking_eyes_ = nullptr;
    bool blink_enabled_ = true;
    uint32_t next_blink_ms_ = 0;

    // Sóng âm khi nghe
    lv_obj_t* wave_box_ = nullptr;
    lv_obj_t* listening_label_ = nullptr;
    static constexpr int kWaveBars = 15;

    // Phụ đề
    lv_obj_t* chat_tag_ = nullptr;

    // Màn hình chờ
    lv_obj_t* standby_ = nullptr;
    lv_obj_t* clock_box_ = nullptr;
    lv_obj_t* clock_digits_[5] = {};
    lv_obj_t* weekday_img_ = nullptr;
    lv_obj_t* date_label_ = nullptr;
    lv_obj_t* weather_row_ = nullptr;
    lv_obj_t* weather_img_ = nullptr;
    lv_obj_t* weather_temp_ = nullptr;
    lv_obj_t* hint_pill_ = nullptr;
    int last_minute_ = -1;
    int last_weather_key_ = -1000;

    // Hẹn giờ
    lv_obj_t* timer_panel_ = nullptr;
    lv_obj_t* timer_arc_ = nullptr;
    lv_obj_t* timer_digits_[5] = {};
    lv_obj_t* timer_pause_btn_ = nullptr;
    lv_obj_t* timer_pause_img_ = nullptr;
    lv_obj_t* timer_cancel_btn_ = nullptr;
    lv_obj_t* timeup_img_ = nullptr;
    std::atomic<int64_t> timer_end_us_{0};
    std::atomic<int64_t> timer_paused_left_us_{0};
    std::atomic<int64_t> timer_total_us_{0};
    int64_t timeup_until_us_ = 0;
    int last_timer_sec_ = -1;
    bool last_timer_paused_ = false;

    // Nhạc
    lv_obj_t* music_panel_ = nullptr;
    lv_obj_t* music_title_ = nullptr;
    lv_obj_t* music_bar_ = nullptr;
    lv_obj_t* music_prev_btn_ = nullptr;
    lv_obj_t* music_play_btn_ = nullptr;
    lv_obj_t* music_next_btn_ = nullptr;
    lv_obj_t* music_play_img_ = nullptr;
    std::string music_title_text_;
    bool music_paused_shown_ = false;

    // Cài Wi-Fi
    lv_obj_t* wifi_panel_ = nullptr;
    lv_obj_t* wifi_qr_ = nullptr;
    lv_obj_t* wifi_ssid_label_ = nullptr;
    lv_obj_t* wifi_url_label_ = nullptr;

    // Cài đặt nhanh
    lv_obj_t* qs_panel_ = nullptr;
    lv_obj_t* qs_vol_slider_ = nullptr;
    lv_obj_t* qs_vol_label_ = nullptr;
    lv_obj_t* qs_bri_slider_ = nullptr;
    lv_obj_t* qs_bri_label_ = nullptr;
    lv_obj_t* qs_theme_tile_ = nullptr;
    lv_obj_t* qs_theme_img_ = nullptr;
    lv_obj_t* qs_ssid_label_ = nullptr;
    lv_obj_t* qs_batt_label_ = nullptr;
    uint32_t qs_last_ms_ = 0;
    std::atomic<bool> qs_toggle_request_{false};

    // Cảm ứng
    lv_indev_t* indev_ = nullptr;
    std::atomic<bool> touch_pressed_{false};
    std::atomic<int> touch_x_{0};
    std::atomic<int> touch_y_{0};
    std::atomic<bool> touch_consumed_{false};

    lv_timer_t* tick_timer_ = nullptr;
    int last_state_ = -1;
    uint32_t last_slow_ms_ = 0;

    void CreateFace();
    void CreateWave();
    void CreateSubtitle();
    void CreateStandby();
    void CreateClock(lv_obj_t* parent);
    void CreateTimer(lv_obj_t* parent);
    void CreateMusic(lv_obj_t* parent);
    void CreateWifiPanel();
    void CreateQuickSettings();
    void CreateTouch();
    void StyleLowBattery();
    void PlaceUnderChrome(lv_obj_t* obj);
    void MakeInteractive(lv_obj_t* obj);

    void SetEyes(const lv_image_dsc_t* eyes);
    void Blink();
    void SetDigits(lv_obj_t* const* slots, int a, int b);
    void UpdateClock(bool force);
    void UpdateWeather();
    void UpdateTimer(int64_t now_us);
    void UpdateMusic();
    void ShowWifiPanel(bool show);
    void OpenQuickSettings(bool open);
    void RefreshQuickSettings();
    void OnTick();

    static void TickCallback(lv_timer_t* timer);
    static void IndevRead(lv_indev_t* indev, lv_indev_data_t* data);
    static void OnPressedEvent(lv_event_t* e);
    static void OnGestureEvent(lv_event_t* e);
    static void OnButtonEvent(lv_event_t* e);
    static void OnSliderEvent(lv_event_t* e);
};
