// LTKK - Lập Trình Không Khó
// Xiaozhi cho bo 2.8inch ESP32-S3 Display (LCDWIKI ES3C28P / ES3N28P)
// Dựa trên board freenove-esp32s3-display-2.8-lcd (cùng sơ đồ chân).

#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>

#include <wifi_station.h>
#include "wifi_board.h"
#include "codecs/es8311_audio_codec.h"
#include "display/lcd_display.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "mcp_server.h"
#include "adc_battery_monitor.h"

#include <esp_log.h>
#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_timer.h>

#include "led/single_led.h"
#include "system_reset.h"
#include "esp_lcd_ili9341.h"
#include "ltkk_brand.h"
#include "settings.h"
#include "ltkk_display.h"
#include "ltkk_music.h"
#include "ltkk_weather.h"
#include <cJSON.h>
#include <utility>
#include <atomic>
#include <cstdlib>

#define TAG "LtkkEsp32S3"

class TouchDriver {
public:
    TouchDriver() : dev_(nullptr) {}

    bool Init(i2c_master_bus_handle_t bus, uint8_t addr) {
        i2c_device_config_t cfg = {
            .device_address = addr,
            .scl_speed_hz = 400000,
            .scl_wait_us = 0,
        };
        return i2c_master_bus_add_device(bus, &cfg, &dev_) == ESP_OK;
    }

    bool Read(bool &touched, uint16_t &x, uint16_t &y) {
        touched = false;
        x = y = 0;
        if (!dev_) return false;

        uint8_t reg = 0x02;
        uint8_t buf[5];
        if (i2c_master_transmit_receive(dev_, &reg, 1, buf, 5, 50) != ESP_OK) return false;

        uint8_t points = buf[0] & 0x0F;
        if (points == 0) return true;

        touched = true;
        x = ((buf[1] & 0x0F) << 8) | buf[2];
        y = ((buf[3] & 0x0F) << 8) | buf[4];
        return true;
    }

private:
    i2c_master_dev_handle_t dev_;
};

class LtkkEsp32S3Display : public WifiBoard {
private:
    Button boot_button_;
    LtkkDisplay* display_;
    i2c_master_bus_handle_t codec_i2c_bus_;
    TouchDriver touch_;
    AdcBatteryMonitor* adc_battery_monitor_;

    void InitializeBatteryMonitor() {
        adc_battery_monitor_ = new AdcBatteryMonitor(ADC_UNIT_1, ADC_CHANNEL_8, 200000, 200000, GPIO_NUM_NC);
    }

    // Hướng cảm ứng: bit0 = đổi trục, bit1 = lật X, bit2 = lật Y.
    // Mặc định lấy từ config.h, sau khi hiệu chỉnh thì lưu trong bộ nhớ (NVS "ltkk/touch").
    std::atomic<int> touch_cfg_{(TOUCH_SWAP_XY ? 1 : 0) | (TOUCH_MIRROR_X ? 2 : 0) |
                                (TOUCH_MIRROR_Y ? 4 : 0)};
    std::atomic<int> calib_step_{0};  // 0 = bình thường, 1/2 = đang chờ chạm dấu + thứ 1/2
    int calib_x1_ = 0, calib_y1_ = 0;

    void MapTouch(uint16_t rx, uint16_t ry, int &sx, int &sy) {
        int cfg = touch_cfg_.load();
        int x = rx, y = ry;
        if (cfg & 1) std::swap(x, y);
        if (cfg & 2) x = DISPLAY_WIDTH - 1 - x;
        if (cfg & 4) y = DISPLAY_HEIGHT - 1 - y;
        sx = x < 0 ? 0 : (x >= DISPLAY_WIDTH ? DISPLAY_WIDTH - 1 : x);
        sy = y < 0 ? 0 : (y >= DISPLAY_HEIGHT ? DISPLAY_HEIGHT - 1 : y);
    }

public:
    // Hiệu chỉnh cảm ứng: chạm 2 dấu + ở hai góc, tự tính hướng đúng
    void StartTouchCalibration() {
        calib_step_ = 1;
        display_->ShowTouchCalibration(1);
    }

private:
    void OnCalibrationTap(int rx, int ry) {
        if (calib_step_ == 1) {
            calib_x1_ = rx;
            calib_y1_ = ry;
            calib_step_ = 2;
            display_->ShowTouchCalibration(2);
            return;
        }
        int dxr = rx - calib_x1_, dyr = ry - calib_y1_;
        if (abs(dxr) < 60 && abs(dyr) < 60) {  // chạm quá gần nhau: làm lại
            calib_step_ = 1;
            display_->ShowTouchCalibration(1);
            return;
        }
        // Dấu 1 ở góc trên-trái, dấu 2 ở góc dưới-phải màn hình ngang
        bool swap = abs(dyr) > abs(dxr);
        int along_x = swap ? dyr : dxr;
        int along_y = swap ? dxr : dyr;
        int cfg = (swap ? 1 : 0) | (along_x < 0 ? 2 : 0) | (along_y < 0 ? 4 : 0);
        touch_cfg_ = cfg;
        Settings settings("ltkk", true);
        settings.SetInt("touch", cfg);
        calib_step_ = 0;
        ESP_LOGI(TAG, "Touch calibrated: cfg=%d (raw d=%d,%d)", cfg, dxr, dyr);
        display_->ShowTouchCalibration(0);
    }

    static void TouchTask(void *arg) {
        auto *self = static_cast<LtkkEsp32S3Display*>(arg);
        auto &app = Application::GetInstance();
        auto *ui = self->display_;

        uint32_t last_tap = 0;
        uint32_t down_start = 0;
        bool down = false;
        int start_x = 0, start_y = 0, last_x = 0, last_y = 0;
        int raw_x = 0, raw_y = 0;
        int logged = 0;

        while (true) {
            bool t;
            uint16_t x, y;
            self->touch_.Read(t, x, y);
            uint32_t now = esp_timer_get_time() / 1000;

            if (t) {
                int sx, sy;
                self->MapTouch(x, y, sx, sy);
                raw_x = x;
                raw_y = y;
                last_x = sx;
                last_y = sy;
                if (!down) {
                    down = true;
                    down_start = now;
                    start_x = sx;
                    start_y = sy;
                    if (logged < 30) {
                        logged++;
                        ESP_LOGI(TAG, "Touch raw=(%d,%d) screen=(%d,%d)", x, y, sx, sy);
                    }
                }
                if (ui != nullptr && self->calib_step_.load() == 0) {
                    ui->HandlePress(true, sx, sy);
                }
            } else if (down) {
                down = false;
                uint32_t press = now - down_start;
                int dx = last_x - start_x;
                int dy = last_y - start_y;
                bool handled = false;
                if (self->calib_step_.load() != 0) {
                    self->OnCalibrationTap(raw_x, raw_y);  // đang hiệu chỉnh: dùng toạ độ thô
                    vTaskDelay(pdMS_TO_TICKS(20));
                    continue;
                }
                if (press > 8000) {
                    self->StartTouchCalibration();  // giữ hơn 8 giây: hiệu chỉnh cảm ứng
                    continue;
                }
                if (ui != nullptr) {
                    ui->HandlePress(false, last_x, last_y);
                    // Vuốt: ưu tiên trước chạm
                    if (abs(dy) > 50 && abs(dy) > abs(dx)) {
                        handled = ui->HandleSwipe(dy > 0 ? LtkkDisplay::kSwipeDown
                                                         : LtkkDisplay::kSwipeUp);
                    } else if (abs(dx) > 60 && abs(dx) > abs(dy)) {
                        handled = ui->HandleSwipe(dx > 0 ? LtkkDisplay::kSwipeRight
                                                         : LtkkDisplay::kSwipeLeft);
                    } else if (press < 1000) {
                        handled = ui->HandleTap(last_x, last_y);  // nút trên giao diện LTKK?
                    }
                }

                if (handled || abs(dx) > 50 || abs(dy) > 50) {
                    // giao diện LTKK đã xử lý, hoặc là cú vuốt: không bật trò chuyện
                } else if (press > 3000) {
                    self->EnterWifiConfigMode();      // giữ hơn 3 giây: cài Wi-Fi
                } else if (press > 1000) {
                    ui->ToggleSettings();             // giữ 1-3 giây: mở cài đặt
                } else if (now - last_tap < 300) {
                    app.StartListening();             // chạm 2 lần
                    last_tap = 0;
                } else {
                    app.ToggleChatState();            // chạm 1 lần vào vùng trống
                    last_tap = now;
                }
            }

            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }

    void InitializeTouch() {
        if (!touch_.Init(codec_i2c_bus_, TOUCH_I2C_ADDR)) return;
        xTaskCreatePinnedToCore(TouchTask, "touch_task", 4096, this, 5, nullptr, 0);
    }

    void InitializeI2c() {
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = AUDIO_CODEC_I2C_NUM,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &codec_i2c_bus_));
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = DISPLAY_MOSI_PIN;
        buscfg.miso_io_num = DISPLAY_MIS0_PIN;
        buscfg.sclk_io_num = DISPLAY_SCK_PIN;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(LCD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto &app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
            }
            app.ToggleChatState();
        });
    }

    void InitializeLcdDisplay() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;
        // Khởi tạo giao tiếp SPI với màn hình
        ESP_LOGD(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_CS_PIN;
        io_config.dc_gpio_num = DISPLAY_DC_PIN;
        io_config.spi_mode = DISPLAY_SPI_MODE;
        io_config.pclk_hz = DISPLAY_SPI_SCLK_HZ;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(LCD_SPI_HOST, &io_config, &panel_io));

        // Khởi tạo chip điều khiển ILI9341
        ESP_LOGD(TAG, "Install LCD driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = DISPLAY_RST_PIN;
        panel_config.rgb_ele_order = DISPLAY_RGB_ORDER;
        panel_config.bits_per_pixel = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(panel_io, &panel_config, &panel));
        ESP_LOGI(TAG, "Install LCD driver ILI9341");
        esp_lcd_panel_reset(panel);

        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, DISPLAY_INVERT_COLOR);
        esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
        display_ = new LtkkDisplay(panel_io, panel,
            DISPLAY_WIDTH, DISPLAY_HEIGHT,
            DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
            DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);

        // Bộ nhận diện LTKK: tô màu giao diện và hiện màn hình chào
        ltkk::ApplyTheme();
        ltkk::ShowSplash(display_);
    }

    // Công cụ để Lu tự gọi khi bạn nói (MCP)
    void InitializeTools() {
        auto& mcp = McpServer::GetInstance();
        mcp.AddTool("self.study_timer.start",
            "Start a study / Pomodoro countdown timer shown on the screen. "
            "Use when the user asks to set a timer, remind them after some minutes, "
            "or start a study session (default 25 minutes).",
            PropertyList({Property("minutes", kPropertyTypeInteger, 25, 1, 180)}),
            [this](const PropertyList& properties) -> ReturnValue {
                display_->StartStudyTimer(properties["minutes"].value<int>());
                return true;
            });
        mcp.AddTool("self.study_timer.add_minutes",
            "Add minutes to the running study timer (or start one if none is running).",
            PropertyList({Property("minutes", kPropertyTypeInteger, 5, 1, 60)}),
            [this](const PropertyList& properties) -> ReturnValue {
                int add = properties["minutes"].value<int>();
                int left = display_->StudyTimerRemainingSeconds();
                display_->StartStudyTimer((left + 59) / 60 + add);
                return true;
            });
        mcp.AddTool("self.study_timer.cancel",
            "Cancel the running study timer.",
            PropertyList(),
            [this](const PropertyList& properties) -> ReturnValue {
                display_->CancelStudyTimer();
                return true;
            });
        mcp.AddTool("self.study_timer.pause_resume",
            "Pause the running study timer, or resume it if it is paused.",
            PropertyList(),
            [this](const PropertyList& properties) -> ReturnValue {
                display_->PauseResumeStudyTimer();
                return true;
            });

        // --- Nhạc từ thẻ nhớ microSD
        mcp.AddTool("self.music.play",
            "Play music files (mp3, wav, aac, m4a, flac, ogg) from the microSD card. Use when the "
            "user asks to play a song from the card. `name` is optional: part of the song title. "
            "If the card has no songs, use self.radio.play instead. "
            "Music starts after you finish speaking; keep your reply short.",
            PropertyList({Property("name", kPropertyTypeString, std::string(""))}),
            [](const PropertyList& properties) -> ToolResult {
                auto& music = LtkkMusic::GetInstance();
                if (!music.Play(properties["name"].value<std::string>())) {
                    return std::unexpected(music.LastError());
                }
                return std::string("Playing: ") + music.CurrentTitle();
            });
        mcp.AddTool("self.music.stop", "Stop the music or radio.", PropertyList(),
            [](const PropertyList& properties) -> ReturnValue {
                LtkkMusic::GetInstance().Stop();
                return true;
            });
        mcp.AddTool("self.music.next", "Skip to the next song or next radio station.", PropertyList(),
            [](const PropertyList& properties) -> ReturnValue {
                LtkkMusic::GetInstance().Next();
                return true;
            });
        mcp.AddTool("self.music.list", "List the songs on the microSD card.", PropertyList(),
            [](const PropertyList& properties) -> ToolResult {
                auto& music = LtkkMusic::GetInstance();
                auto songs = music.List();
                if (songs.empty()) {
                    auto error = music.LastError();
                    return std::unexpected(error.empty() ? std::string("No songs found") : error);
                }
                cJSON* arr = cJSON_CreateArray();
                for (const auto& song : songs) {
                    cJSON_AddItemToArray(arr, cJSON_CreateString(song.c_str()));
                }
                return arr;
            });

        // --- Radio trực tuyến
        mcp.AddTool("self.radio.play",
            "Play online radio / streaming music (lofi, chill, relax, focus stations). Use when "
            "the user asks for background music for studying without naming a song or station. "
            "`station` is optional (`lofi`, `chill`, `focus`). For other stations use self.radio.search. "
            "YouTube and Spotify are not supported. Radio starts after you finish speaking.",
            PropertyList({Property("station", kPropertyTypeString, std::string(""))}),
            [](const PropertyList& properties) -> ToolResult {
                auto& music = LtkkMusic::GetInstance();
                music.PlayRadioByName(properties["station"].value<std::string>());
                return std::string("Playing radio: ") + music.CurrentTitle();
            });
        mcp.AddTool("self.music.play_song",
            "Play a specific song by name. Use whenever the user asks for a particular song or "
            "artist, e.g. 'mở bài Nơi này có anh'. The LTKK app on the user's computer looks in "
            "the user's music library first, otherwise opens the song in the official YouTube "
            "player on the computer and streams its sound to this device. "
            "`song_name` is required, `artist_name` optional. If this fails, suggest radio. "
            "The song starts after you finish speaking; keep your reply short.",
            PropertyList({Property("song_name", kPropertyTypeString),
                          Property("artist_name", kPropertyTypeString, std::string(""))}),
            [](const PropertyList& properties) -> ToolResult {
                auto& music = LtkkMusic::GetInstance();
                if (!music.PlaySong(properties["song_name"].value<std::string>(),
                                    properties["artist_name"].value<std::string>())) {
                    return std::unexpected(music.LastError());
                }
                return std::string("Playing: ") + music.CurrentTitle();
            });
        mcp.AddTool("self.music.pc_speaker",
            "Use this device as a wireless speaker for the user's computer (plays whatever sound "
            "the computer is playing: YouTube in the browser, Spotify app, videos...). Use when the "
            "user says 'làm loa cho máy tính' or wants to hear the computer's sound here.",
            PropertyList(),
            [](const PropertyList& properties) -> ToolResult {
                auto& music = LtkkMusic::GetInstance();
                if (!music.PlayPcSpeaker()) {
                    return std::unexpected(music.LastError());
                }
                return true;
            });
        mcp.AddTool("self.radio.search",
            "Search the worldwide online radio directory and play the best match. `query` is a "
            "station name or genre/tag, e.g. 'VOV', 'nhac tre', 'bolero', 'lofi', 'jazz', 'news'. "
            "`countrycode` optional ISO code, e.g. 'VN' for Vietnamese stations.",
            PropertyList({Property("query", kPropertyTypeString),
                          Property("countrycode", kPropertyTypeString, std::string(""))}),
            [](const PropertyList& properties) -> ToolResult {
                auto& music = LtkkMusic::GetInstance();
                if (!music.SearchRadio(properties["query"].value<std::string>(),
                                       properties["countrycode"].value<std::string>())) {
                    return std::unexpected(music.LastError());
                }
                return std::string("Playing radio: ") + music.CurrentTitle();
            });
        mcp.AddTool("self.music.set_server",
            "Set the address of the LTKK music server (e.g. '192.168.1.10:8765'). Normally not "
            "needed: the device finds the server on the home network automatically.",
            PropertyList({Property("url", kPropertyTypeString)}),
            [](const PropertyList& properties) -> ReturnValue {
                LtkkMusic::GetInstance().SetMusicServer(properties["url"].value<std::string>());
                return true;
            });
        mcp.AddTool("self.radio.play_url",
            "Play an internet radio stream from a direct http(s) MP3 or AAC stream URL "
            "(not .m3u8, not YouTube).",
            PropertyList({Property("url", kPropertyTypeString),
                          Property("name", kPropertyTypeString, std::string("Radio"))}),
            [](const PropertyList& properties) -> ToolResult {
                auto& music = LtkkMusic::GetInstance();
                if (!music.PlayUrl(properties["url"].value<std::string>(),
                                   properties["name"].value<std::string>())) {
                    return std::unexpected(music.LastError());
                }
                return true;
            });
        mcp.AddTool("self.radio.list", "List the built-in online radio stations.", PropertyList(),
            [](const PropertyList& properties) -> ReturnValue {
                cJSON* arr = cJSON_CreateArray();
                for (const auto& name : LtkkMusic::GetInstance().RadioList()) {
                    cJSON_AddItemToArray(arr, cJSON_CreateString(name.c_str()));
                }
                return arr;
            });

        mcp.AddTool("self.screen.calibrate_touch",
            "Start touch screen calibration (two + targets to tap). Use when the user says touches "
            "are inverted, mirrored, misaligned, or asks to calibrate the touch screen.",
            PropertyList(),
            [this](const PropertyList& properties) -> ReturnValue {
                StartTouchCalibration();
                return true;
            });

        // --- Kiểu màn hình chờ
        mcp.AddTool("self.screen.set_standby_style",
            "Change the standby (idle) screen style: 0 = big digital clock, 1 = LTKK analog clock, "
            "2 = sleeping Lu face, 3 = LTKK brand card.",
            PropertyList({Property("style", kPropertyTypeInteger, 0, 0, 3)}),
            [this](const PropertyList& properties) -> ReturnValue {
                display_->SetStandbyStyle(properties["style"].value<int>());
                return true;
            });

        // --- Thời tiết trên màn hình chờ
        mcp.AddTool("self.weather.set_city",
            "Set the city whose current weather is shown on the standby screen "
            "(for example `Ha Noi`, `Da Nang`, `Ho Chi Minh`).",
            PropertyList({Property("city", kPropertyTypeString)}),
            [](const PropertyList& properties) -> ToolResult {
                auto city = properties["city"].value<std::string>();
                if (!LtkkWeather::GetInstance().SetCity(city)) {
                    return std::unexpected("Could not find weather for " + city);
                }
                auto& w = LtkkWeather::GetInstance();
                return city + ": " + std::to_string(w.temperature()) + " C";
            });

        mcp.AddTool("self.study_timer.get_remaining",
            "Get how many seconds are left on the study timer (0 if no timer is running).",
            PropertyList(),
            [this](const PropertyList& properties) -> ReturnValue {
                return display_->StudyTimerRemainingSeconds();
            });
    }

public:
    LtkkEsp32S3Display(): boot_button_(BOOT_BUTTON_GPIO)
    {
        {
            Settings settings("ltkk", false);
            int saved = settings.GetInt("touch", -1);
            if (saved >= 0 && saved <= 7) {
                touch_cfg_ = saved;
            }
        }
        InitializeI2c();
        InitializeBatteryMonitor();
        InitializeSpi();
        ltkk::SetDefaultThemeDark();
        InitializeLcdDisplay();
        InitializeTouch();
        InitializeButtons();
        InitializeTools();
        LtkkWeather::GetInstance().Start();
        GetBacklight()->SetBrightness(100);
    }

    virtual Led *GetLed() override {
        static SingleLed led(BUILTIN_LED_GPIO);
        return &led;
    }

    virtual AudioCodec* GetAudioCodec() override {
        static Es8311AudioCodec audio_codec(codec_i2c_bus_, AUDIO_CODEC_I2C_NUM,
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE, AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK,
            AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN, AUDIO_CODEC_PA_PIN,
            AUDIO_CODEC_ES8311_ADDR, true, true);
        return &audio_codec;
    }

    virtual Display *GetDisplay() override { return display_; }

    virtual Backlight *GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }

    virtual bool GetBatteryLevel(int &level, bool& charging, bool& discharging) override {
        charging = adc_battery_monitor_->IsCharging();
        discharging = adc_battery_monitor_->IsDischarging();
        level = adc_battery_monitor_->GetBatteryLevel();
        return true;
    }
};

DECLARE_BOARD(LtkkEsp32S3Display);
