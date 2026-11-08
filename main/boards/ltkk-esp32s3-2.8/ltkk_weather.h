#pragma once

// Thời tiết cho màn hình chờ, lấy miễn phí từ Open-Meteo (không cần API key).
// Thành phố được đặt bằng giọng nói: "Lu ơi, đặt thành phố thời tiết là Đà Nẵng".

#include <atomic>
#include <mutex>
#include <string>

class LtkkWeather {
public:
    static LtkkWeather& GetInstance();

    void Start();                                // chạy nền, cập nhật 30 phút/lần
    bool SetCity(const std::string& city);       // tìm toạ độ + cập nhật ngay
    std::string GetCity();

    bool valid() const { return valid_.load(); }
    int temperature() const { return temperature_.load(); }   // °C, đã làm tròn
    int condition() const { return condition_.load(); }       // 0..5, xem ltkk_wx_*

private:
    LtkkWeather() = default;
    static void TaskEntry(void* arg);
    void TaskLoop();
    bool Fetch();

    std::mutex mutex_;
    std::string city_;
    std::string lat_;
    std::string lon_;
    std::atomic<bool> valid_{false};
    std::atomic<int> temperature_{0};
    std::atomic<int> condition_{1};
    bool started_ = false;
};
