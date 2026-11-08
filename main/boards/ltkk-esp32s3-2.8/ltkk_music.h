#pragma once

// Trình phát nhạc của Lu
//  - Thẻ nhớ microSD (SDIO 4-bit): .mp3 .wav .aac .m4a .flac .ogg (thư mục gốc hoặc /music)
//  - Radio trực tuyến: luồng MP3 / AAC qua HTTP (danh sách đài ở ltkk_music.cc)
// Nhạc chỉ bắt đầu khi Lu đã nói xong; tự dừng khi bạn gọi Lu.

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

struct LtkkStation {
    const char* name;
    const char* url;
};

class LtkkMusic {
public:
    static LtkkMusic& GetInstance();

    // Thẻ nhớ: phát bài theo tên gần đúng (rỗng = bài hiện tại)
    bool Play(const std::string& name = "");
    // Radio: theo số thứ tự / tên đài, hoặc một địa chỉ http(s) bất kỳ
    bool PlayRadio(int index);
    bool PlayRadioByName(const std::string& name);
    bool PlayUrl(const std::string& url, const std::string& name);
    // Phím tắt trên màn hình: có nhạc trên thẻ thì phát thẻ, không thì phát radio
    bool PlayAny();

    void Stop();
    void TogglePause();
    void Next();
    void Prev();

    bool IsActive() const { return active_.load(); }
    bool IsPaused() const { return paused_.load(); }
    bool IsPending() const { return pending_.load(); }
    bool IsRadio() const { return radio_.load(); }
    bool IsBuffering() const { return buffering_.load(); }
    int ProgressPermille() const { return progress_.load(); }
    std::string CurrentTitle();
    std::vector<std::string> List();
    std::vector<std::string> RadioList();
    std::string LastError();

private:
    LtkkMusic() = default;
    bool Mount();
    void ScanLocked();
    bool Request(bool radio);
    void EnsureTask();
    static void TaskEntry(void* arg);
    void TaskLoop();
    bool WaitUntilIdle();
    bool PlayOggFile(const std::string& path);
    bool PlayDecodedFile(const std::string& path);
    bool PlayStream(const std::string& url);
    bool Interrupted() const { return stop_.load() || skip_.load() != 0; }

    std::mutex mutex_;
    bool mounted_ = false;
    std::vector<std::string> files_;
    int index_ = 0;
    int station_ = 0;
    std::string custom_url_;
    std::string custom_name_;
    std::string error_;

    TaskHandle_t task_ = nullptr;
    std::atomic<bool> pending_{false};
    std::atomic<bool> active_{false};
    std::atomic<bool> paused_{false};
    std::atomic<bool> stop_{false};
    std::atomic<bool> radio_{false};
    std::atomic<bool> buffering_{false};
    std::atomic<int> skip_{0};
    std::atomic<int> progress_{0};
};
