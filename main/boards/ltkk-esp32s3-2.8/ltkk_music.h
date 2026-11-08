#pragma once

// Phát nhạc từ thẻ nhớ microSD (SDIO 4-bit trên bo 2.8inch ESP32-S3)
//
// Định dạng: file .ogg (Opus, mono, 16 kHz, khung 60 ms) đặt ở thư mục gốc
// hoặc thư mục /music của thẻ nhớ. Đổi từ mp3 bằng ffmpeg:
//   ffmpeg -i bai_hat.mp3 -c:a libopus -b:a 32k -ac 1 -ar 16000 -frame_duration 60 bai_hat.ogg
// Tên file nên viết không dấu để hiển thị đúng trên màn hình.

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

class LtkkMusic {
public:
    static LtkkMusic& GetInstance();

    // Yêu cầu phát (bài theo tên gần đúng, rỗng = bài hiện tại / bài đầu).
    // Nhạc chỉ bắt đầu khi Lu đã nói xong và về trạng thái chờ.
    bool Play(const std::string& name = "");
    void Stop();
    void TogglePause();
    void Next();
    void Prev();

    bool IsActive() const { return active_.load(); }    // đang phát hoặc tạm dừng
    bool IsPaused() const { return paused_.load(); }
    bool IsPending() const { return pending_.load(); }  // chờ Lu nói xong để phát
    int ProgressPermille() const { return progress_.load(); }
    std::string CurrentTitle();
    std::vector<std::string> List();
    std::string LastError();

private:
    LtkkMusic() = default;
    bool Mount();
    void ScanLocked();
    void EnsureTask();
    static void TaskEntry(void* arg);
    void TaskLoop();
    bool PlayFile(const std::string& path);

    std::mutex mutex_;
    bool mounted_ = false;
    std::vector<std::string> files_;  // đường dẫn đầy đủ
    int index_ = 0;
    std::string error_;

    TaskHandle_t task_ = nullptr;
    std::atomic<bool> pending_{false};
    std::atomic<bool> active_{false};
    std::atomic<bool> paused_{false};
    std::atomic<bool> stop_{false};
    std::atomic<int> skip_{0};  // +1 bài sau, -1 bài trước
    std::atomic<int> progress_{0};
};
