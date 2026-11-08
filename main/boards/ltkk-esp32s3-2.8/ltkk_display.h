#pragma once

// Giao diện Lu - LTKK cho Xiaozhi (màn hình 320x240)
//  Khởi động : logo LTKK + "Học để làm"                     (ltkk_brand.cc)
//  Chờ       : 4 kiểu - Đồng hồ / Kim LTKK / Lu ngủ / Thẻ LTKK (vuốt ngang để đổi)
//  Đang nghe : mắt mở to + sóng âm + "Lu đang nghe nè…"
//  Đang nói  : nhân vật Lu 12 cảm xúc + nhãn tiếng Việt (VUI, BUỒN...) + phụ đề LU / BẠN
//  Hẹn giờ   : vòng đếm ngược, nút Tạm dừng / Huỷ
//  Nhạc      : thẻ microSD (mp3/wav/aac/m4a/flac/ogg) + radio trực tuyến
//  Cài Wi-Fi : mã QR + 3 bước
//  Cài đặt   : nút bánh răng trên màn hình chờ, vuốt xuống, hoặc giữ 1-3 giây
//  Pin yếu   : khung thông báo trắng viền cam

#include <atomic>
#include <string>

#include "display/lcd_display.h"

class LtkkDisplay : public SpiLcdDisplay {
public:
    static constexpr int kStandbyStyles = 4;

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

    // Kiểu màn hình chờ 0..3 - gọi được từ bất kỳ luồng nào
    void SetStandbyStyle(int style);
    int GetStandbyStyle() const { return standby_style_.load(); }

    // Cảm ứng - bo mạch gọi từ luồng cảm ứng, toạ độ màn hình 320x240.
    // Trả về true nếu giao diện LTKK đã xử lý (khi đó bo mạch KHÔNG bật trò chuyện).
    enum Swipe { kSwipeUp, kSwipeDown, kSwipeLeft, kSwipeRight };
    void HandlePress(bool down, int x, int y);   // hiệu ứng nhấn + chấm kiểm tra
    bool HandleTap(int x, int y);
    bool HandleSwipe(Swipe dir);
    // Màn hình hiệu chỉnh cảm ứng: step 1/2 = hiện dấu + thứ 1/2, 0 = xong
    void ShowTouchCalibration(int step);
    void ToggleSettings();
    // Xem các cảm xúc của Lu: lần lượt từng mặt kèm nhãn tiếng Việt (~2 giây mỗi mặt).
    // key = "" -> tất cả; key = "happy", "sad"... -> chỉ hiện mặt đó 5 giây. Gọi từ luồng nào cũng được.
    void StartEmotionDemo(const std::string& key = "");

private:
    // Thứ / ngày / thời tiết dùng chung cho các kiểu màn hình chờ
    struct InfoWidgets {
        lv_obj_t* weekday = nullptr;
        lv_obj_t* date = nullptr;
        lv_obj_t* weather = nullptr;
        lv_obj_t* weather_img = nullptr;
        lv_obj_t* weather_temp = nullptr;
    };

    // Khuôn mặt
    lv_obj_t* face_ = nullptr;
    const lv_image_dsc_t* current_eyes_ = nullptr;
    const lv_image_dsc_t* speaking_eyes_ = nullptr;
    bool blink_enabled_ = true;
    uint32_t next_blink_ms_ = 0;
    uint32_t blink_until_ms_ = 0;
    lv_obj_t* emo_tag_ = nullptr;      // nhãn cảm xúc tiếng Việt
    lv_obj_t* emo_tag_img_ = nullptr;
    uint32_t emo_tag_until_ms_ = 0;

    // Sóng âm khi nghe
    lv_obj_t* wave_box_ = nullptr;
    lv_obj_t* listening_label_ = nullptr;
    static constexpr int kWaveBars = 15;

    // Phụ đề
    lv_obj_t* chat_tag_ = nullptr;

    // Màn hình chờ
    lv_obj_t* standby_ = nullptr;
    lv_obj_t* style_panels_[kStandbyStyles] = {};
    InfoWidgets info_[kStandbyStyles];
    lv_obj_t* clock_digits_[kStandbyStyles][5] = {};
    lv_obj_t* analog_hour_ = nullptr;
    lv_obj_t* analog_minute_ = nullptr;
    lv_obj_t* analog_second_ = nullptr;
    lv_point_precise_t hand_points_[3][2] = {};
    lv_obj_t* sleepy_face_ = nullptr;
    lv_obj_t* hint_pill_ = nullptr;
    lv_obj_t* gear_btn_ = nullptr;
    std::atomic<int> standby_style_{0};
    std::atomic<bool> demo_request_{false};
    std::string demo_key_;
    int demo_index_ = -1;        // -1 = không trình diễn
    int demo_end_ = 0;
    uint32_t demo_next_ms_ = 0;
    void StopEmotionDemo();
    int shown_style_ = -1;
    int last_minute_ = -1;
    int last_second_ = -1;
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

    // Nhạc / radio
    lv_obj_t* music_panel_ = nullptr;
    lv_obj_t* music_kind_img_ = nullptr;
    lv_obj_t* music_sub_img_ = nullptr;
    lv_obj_t* music_art_icon_ = nullptr;
    lv_obj_t* music_title_ = nullptr;
    lv_obj_t* music_bar_ = nullptr;
    lv_obj_t* music_prev_btn_ = nullptr;
    lv_obj_t* music_play_btn_ = nullptr;
    lv_obj_t* music_next_btn_ = nullptr;
    lv_obj_t* music_stop_btn_ = nullptr;
    lv_obj_t* music_play_img_ = nullptr;
    std::string music_title_text_;
    int music_mode_shown_ = -1;
    bool music_paused_shown_ = false;

    // Cài Wi-Fi
    lv_obj_t* wifi_panel_ = nullptr;
    lv_obj_t* wifi_qr_ = nullptr;
    lv_obj_t* wifi_ssid_label_ = nullptr;
    lv_obj_t* wifi_url_label_ = nullptr;
    uint8_t* qr_pixels_ = nullptr;
    lv_image_dsc_t qr_dsc_ = {};
    std::string qr_text_;

    // Cài đặt
    lv_obj_t* settings_ = nullptr;
    lv_obj_t* set_close_btn_ = nullptr;
    lv_obj_t* set_vol_minus_ = nullptr;
    lv_obj_t* set_vol_plus_ = nullptr;
    lv_obj_t* set_vol_label_ = nullptr;
    lv_obj_t* set_bri_minus_ = nullptr;
    lv_obj_t* set_bri_plus_ = nullptr;
    lv_obj_t* set_bri_label_ = nullptr;
    lv_obj_t* set_theme_tile_ = nullptr;
    lv_obj_t* set_theme_icon_ = nullptr;
    lv_obj_t* set_theme_img_ = nullptr;
    lv_obj_t* set_style_tile_ = nullptr;
    lv_obj_t* set_style_img_ = nullptr;
    lv_obj_t* set_media_tile_ = nullptr;
    lv_obj_t* set_media_img_ = nullptr;
    lv_obj_t* set_ssid_label_ = nullptr;
    lv_obj_t* set_batt_label_ = nullptr;
    uint32_t settings_last_ms_ = 0;
    std::atomic<bool> settings_toggle_request_{false};

    // Cảm ứng
    lv_obj_t* touch_dot_ = nullptr;
    lv_obj_t* pressed_obj_ = nullptr;
    lv_obj_t* calib_panel_ = nullptr;
    lv_obj_t* calib_target_ = nullptr;
    lv_obj_t* calib_step_label_ = nullptr;
    lv_obj_t* calib_text_ = nullptr;

    lv_timer_t* tick_timer_ = nullptr;
    int last_state_ = -1;
    uint32_t last_slow_ms_ = 0;

    // Dựng giao diện
    void CreateFace();
    void CreateWave();
    void CreateSubtitle();
    void CreateStandby();
    void CreateInfo(lv_obj_t* parent, InfoWidgets& info, bool centered);
    lv_obj_t* CreateDigits(lv_obj_t* parent, lv_obj_t** slots, float scale);
    void CreateStyleClock(lv_obj_t* panel);
    void CreateStyleAnalog(lv_obj_t* panel);
    void CreateStyleSleepy(lv_obj_t* panel);
    void CreateStyleCard(lv_obj_t* panel);
    void CreateTimer(lv_obj_t* parent);
    void CreateMusic(lv_obj_t* parent);
    void CreateWifiPanel();
    void CreateSettings();
    void CreateTouch();
    void StyleLowBattery();
    void PlaceUnderChrome(lv_obj_t* obj);
    void MakeInteractive(lv_obj_t* obj);
    lv_obj_t* FindTarget(int x, int y);
    void OnClick(lv_obj_t* target);
    lv_obj_t* MakeButton(lv_obj_t* parent, int w, int h, uint32_t bg, bool outline);

    // Cập nhật
    void SetEyes(const lv_image_dsc_t* eyes);
    void Blink();
    void ShowEmotionTag(const lv_image_dsc_t* label);
    bool OnDemoTick();
    void SetDigits(lv_obj_t* const* slots, int a, int b);
    void ShowStandbyStyle(int style);
    void UpdateClock(bool force);
    void UpdateAnalog(const struct tm& t);
    void UpdateWeather();
    void UpdateTimer(int64_t now_us);
    void UpdateMusic();
    void ShowWifiPanel(bool show);
    void DrawQrCode(const std::string& text);
    void OpenSettings(bool open);
    void RefreshSettings();
    void ChangeVolume(int delta);
    void ChangeBrightness(int delta);
    void OnTick();

    static void TickCallback(lv_timer_t* timer);
};
