#include "ltkk_weather.h"

#include <cJSON.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_timer.h>

#include <cctype>
#include <cmath>
#include <cstdio>

#include "application.h"
#include "board.h"
#include "settings.h"

#define TAG "LtkkWeather"

namespace {

std::string UrlEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += (char)c;
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

bool HttpGet(const std::string& url, std::string& body) {
    auto http = Board::GetInstance().GetNetwork()->CreateHttp(0);
    if (auto opened = http->Open("GET", url); !opened) {
        ESP_LOGW(TAG, "Open failed: %s", url.c_str());
        return false;
    }
    auto status = http->GetStatusCode();
    if (!status || *status != 200) {
        http->Close();
        return false;
    }
    body = http->ReadAll();
    http->Close();
    return true;
}

// Mã thời tiết WMO -> ảnh chữ ltkk_wx_0..5
int ConditionOf(int code) {
    if (code == 0) return 0;                                   // Trời quang
    if (code <= 3) return 1;                                   // Có mây
    if (code == 45 || code == 48) return 2;                    // Sương mù
    if (code >= 95) return 4;                                  // Giông bão
    if ((code >= 71 && code <= 77) || code == 85 || code == 86) return 5;  // Có tuyết
    return 3;                                                  // Có mưa
}

}  // namespace

LtkkWeather& LtkkWeather::GetInstance() {
    static LtkkWeather instance;
    return instance;
}

void LtkkWeather::Start() {
    if (started_) {
        return;
    }
    started_ = true;
    Settings settings("ltkk", false);
    city_ = settings.GetString("city", "");
    lat_ = settings.GetString("lat", "");
    lon_ = settings.GetString("lon", "");
    xTaskCreate(TaskEntry, "ltkk_weather", 6144, this, 2, nullptr);
}

std::string LtkkWeather::GetCity() {
    std::lock_guard<std::mutex> lock(mutex_);
    return city_;
}

bool LtkkWeather::SetCity(const std::string& city) {
    std::string body;
    std::string url = "https://geocoding-api.open-meteo.com/v1/search?count=1&language=vi&name=" +
                      UrlEncode(city);
    if (!HttpGet(url, body)) {
        return false;
    }
    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        return false;
    }
    bool ok = false;
    cJSON* results = cJSON_GetObjectItem(root, "results");
    cJSON* first = cJSON_IsArray(results) ? cJSON_GetArrayItem(results, 0) : nullptr;
    if (first != nullptr) {
        cJSON* lat = cJSON_GetObjectItem(first, "latitude");
        cJSON* lon = cJSON_GetObjectItem(first, "longitude");
        if (cJSON_IsNumber(lat) && cJSON_IsNumber(lon)) {
            char buf[24];
            std::lock_guard<std::mutex> lock(mutex_);
            city_ = city;
            snprintf(buf, sizeof(buf), "%.4f", lat->valuedouble);
            lat_ = buf;
            snprintf(buf, sizeof(buf), "%.4f", lon->valuedouble);
            lon_ = buf;
            Settings settings("ltkk", true);
            settings.SetString("city", city_);
            settings.SetString("lat", lat_);
            settings.SetString("lon", lon_);
            ok = true;
        }
    }
    cJSON_Delete(root);
    if (ok) {
        Fetch();
    }
    return ok;
}

bool LtkkWeather::Fetch() {
    std::string lat, lon;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        lat = lat_;
        lon = lon_;
    }
    if (lat.empty() || lon.empty()) {
        return false;
    }
    std::string body;
    std::string url = "https://api.open-meteo.com/v1/forecast?latitude=" + lat +
                      "&longitude=" + lon + "&current=temperature_2m,weather_code";
    if (!HttpGet(url, body)) {
        return false;
    }
    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        return false;
    }
    bool ok = false;
    cJSON* current = cJSON_GetObjectItem(root, "current");
    if (cJSON_IsObject(current)) {
        cJSON* temp = cJSON_GetObjectItem(current, "temperature_2m");
        cJSON* code = cJSON_GetObjectItem(current, "weather_code");
        if (cJSON_IsNumber(temp) && cJSON_IsNumber(code)) {
            temperature_ = (int)lround(temp->valuedouble);
            condition_ = ConditionOf(code->valueint);
            valid_ = true;
            ok = true;
            ESP_LOGI(TAG, "Weather: %d C, code %d", temperature_.load(), code->valueint);
        }
    }
    cJSON_Delete(root);
    return ok;
}

void LtkkWeather::TaskEntry(void* arg) { static_cast<LtkkWeather*>(arg)->TaskLoop(); }

void LtkkWeather::TaskLoop() {
    int64_t next_ms = 0;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        // Chỉ gọi mạng khi Lu đang rảnh (đã kết nối mạng, không trò chuyện)
        if (Application::GetInstance().GetDeviceState() != kDeviceStateIdle) {
            continue;
        }
        int64_t now_ms = esp_timer_get_time() / 1000;
        if (now_ms < next_ms) {
            continue;
        }
        bool ok = Fetch();
        next_ms = now_ms + (ok ? 30 * 60 * 1000 : 5 * 60 * 1000);
    }
}
