#pragma once

// Giao diện Lu - LTKK cho Xiaozhi
// - Đôi mắt robot thay cho emoji (10 biểu cảm, biết chớp mắt)
// - Màn hình chờ: đồng hồ lớn + thứ/ngày + gợi ý gọi Lu
// - Khi đang nghe: sóng âm cam/xanh + "Lu đang nghe nè…"
// - Hẹn giờ học (Pomodoro): vòng đếm ngược, điều khiển bằng giọng nói qua MCP

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

    // Hẹn giờ học - gọi được từ bất kỳ luồng nào
    void StartStudyTimer(int minutes);
    void CancelStudyTimer();
    bool IsStudyTimerRunning() const { return timer_end_us_.load() > 0; }
    int StudyTimerRemainingSeconds() const;

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

    // Màn hình chờ
    lv_obj_t* standby_ = nullptr;
    lv_obj_t* clock_box_ = nullptr;
    lv_obj_t* clock_digits_[5] = {};
    lv_obj_t* weekday_img_ = nullptr;
    lv_obj_t* date_label_ = nullptr;
    lv_obj_t* hint_pill_ = nullptr;
    int last_minute_ = -1;

    // Hẹn giờ
    lv_obj_t* timer_box_ = nullptr;
    lv_obj_t* timer_arc_ = nullptr;
    lv_obj_t* timer_digits_[5] = {};
    lv_obj_t* timeup_img_ = nullptr;
    std::atomic<int64_t> timer_end_us_{0};
    std::atomic<int64_t> timer_total_us_{0};
    int64_t timeup_until_us_ = 0;
    int last_timer_sec_ = -1;

    lv_timer_t* tick_timer_ = nullptr;
    int last_state_ = -1;

    void CreateFace();
    void CreateWave();
    void CreateStandby();
    void SetEyes(const lv_image_dsc_t* eyes);
    void Blink();
    void SetDigits(lv_obj_t* const* slots, int a, int b);
    void UpdateClock(bool force);
    void UpdateTimer(int64_t now_us);
    void OnTick();
    static void TickCallback(lv_timer_t* timer);
};
