#include "ltkk_music.h"

#include <dirent.h>
#include <driver/sdmmc_host.h>
#include <esp_log.h>
#include <esp_vfs_fat.h>
#include <sdmmc_cmd.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <memory>

#include "application.h"
#include "audio_service.h"
#include "board.h"
#include "demuxer/ogg_demuxer.h"
#include "protocols/protocol.h"

#define TAG "LtkkMusic"
#define SD_MOUNT_POINT "/sdcard"

namespace {

bool EndsWithOgg(const std::string& name) {
    if (name.size() < 4) {
        return false;
    }
    std::string ext = name.substr(name.size() - 4);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext == ".ogg";
}

std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    return s;
}

std::string TitleOf(const std::string& path) {
    std::string name = path.substr(path.find_last_of('/') + 1);
    if (EndsWithOgg(name)) {
        name.resize(name.size() - 4);
    }
    std::replace(name.begin(), name.end(), '_', ' ');
    return name;
}

}  // namespace

LtkkMusic& LtkkMusic::GetInstance() {
    static LtkkMusic instance;
    return instance;
}

bool LtkkMusic::Mount() {
    if (mounted_) {
        return true;
    }
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 4;
    slot.clk = GPIO_NUM_38;
    slot.cmd = GPIO_NUM_40;
    slot.d0 = GPIO_NUM_39;
    slot.d1 = GPIO_NUM_41;
    slot.d2 = GPIO_NUM_48;
    slot.d3 = GPIO_NUM_47;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_vfs_fat_sdmmc_mount_config_t cfg = {};
    cfg.format_if_mount_failed = false;
    cfg.max_files = 4;
    cfg.allocation_unit_size = 16 * 1024;

    sdmmc_card_t* card = nullptr;
    esp_err_t err = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot, &cfg, &card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Mount SD failed: %s", esp_err_to_name(err));
        error_ = "Không đọc được thẻ nhớ. Hãy kiểm tra thẻ microSD (FAT32) đã cắm chưa.";
        return false;
    }
    mounted_ = true;
    ESP_LOGI(TAG, "SD card mounted");
    return true;
}

void LtkkMusic::ScanLocked() {
    files_.clear();
    for (const char* dir_path : {SD_MOUNT_POINT, SD_MOUNT_POINT "/music"}) {
        DIR* dir = opendir(dir_path);
        if (dir == nullptr) {
            continue;
        }
        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            std::string name = entry->d_name;
            if (entry->d_type != DT_DIR && EndsWithOgg(name) && name[0] != '.') {
                files_.push_back(std::string(dir_path) + "/" + name);
            }
        }
        closedir(dir);
    }
    std::sort(files_.begin(), files_.end());
    if (index_ >= (int)files_.size()) {
        index_ = 0;
    }
    ESP_LOGI(TAG, "Found %d song(s)", (int)files_.size());
}

std::vector<std::string> LtkkMusic::List() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> titles;
    if (!Mount()) {
        return titles;
    }
    ScanLocked();
    for (const auto& f : files_) {
        titles.push_back(TitleOf(f));
    }
    return titles;
}

std::string LtkkMusic::CurrentTitle() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (files_.empty()) {
        return "";
    }
    return TitleOf(files_[index_ % files_.size()]);
}

std::string LtkkMusic::LastError() {
    std::lock_guard<std::mutex> lock(mutex_);
    return error_;
}

bool LtkkMusic::Play(const std::string& name) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        error_.clear();
        if (!Mount()) {
            return false;
        }
        ScanLocked();
        if (files_.empty()) {
            error_ = "Thẻ nhớ chưa có bài nào. Hãy chép file .ogg vào thẻ (thư mục gốc hoặc /music).";
            return false;
        }
        if (!name.empty()) {
            std::string wanted = Lower(name);
            for (size_t i = 0; i < files_.size(); i++) {
                if (Lower(TitleOf(files_[i])).find(wanted) != std::string::npos) {
                    index_ = (int)i;
                    break;
                }
            }
        }
    }
    if (active_.load()) {
        // Đang phát: chuyển bài ngay
        stop_ = true;
        while (active_.load()) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
    stop_ = false;
    skip_ = 0;
    paused_ = false;
    pending_ = true;
    EnsureTask();
    xTaskNotifyGive(task_);
    return true;
}

void LtkkMusic::Stop() {
    pending_ = false;
    if (active_.load()) {
        stop_ = true;
    }
}

void LtkkMusic::TogglePause() {
    if (active_.load()) {
        paused_ = !paused_.load();
    } else {
        Play();
    }
}

void LtkkMusic::Next() {
    if (active_.load()) {
        paused_ = false;
        skip_ = 1;
    }
}

void LtkkMusic::Prev() {
    if (active_.load()) {
        paused_ = false;
        skip_ = -1;
    }
}

void LtkkMusic::EnsureTask() {
    if (task_ == nullptr) {
        xTaskCreatePinnedToCore(TaskEntry, "ltkk_music", 6144, this, 4, &task_, 1);
    }
}

void LtkkMusic::TaskEntry(void* arg) { static_cast<LtkkMusic*>(arg)->TaskLoop(); }

void LtkkMusic::TaskLoop() {
    auto& app = Application::GetInstance();
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        // Chờ Lu nói xong. Nếu Lu chuyển sang nghe tiếp thì đóng cuộc trò chuyện.
        bool saw_speaking = false;
        bool closed = false;
        while (pending_.load() && !stop_.load()) {
            auto state = app.GetDeviceState();
            if (state == kDeviceStateIdle) {
                break;
            }
            if (state == kDeviceStateSpeaking) {
                saw_speaking = true;
            } else if (state == kDeviceStateListening && saw_speaking && !closed) {
                closed = true;
                app.Schedule([]() { Application::GetInstance().ToggleChatState(); });
            }
            vTaskDelay(pdMS_TO_TICKS(150));
        }
        if (!pending_.load() || stop_.load()) {
            pending_ = false;
            stop_ = false;
            continue;
        }
        pending_ = false;
        active_ = true;
        paused_ = false;

        int failures = 0;
        while (!stop_.load()) {
            std::string path;
            int count;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                count = (int)files_.size();
                if (count == 0) {
                    break;
                }
                path = files_[index_ % count];
            }
            bool ok = PlayFile(path);
            if (stop_.load()) {
                break;
            }
            failures = ok ? 0 : failures + 1;
            if (failures >= count) {
                break;
            }
            int step = skip_.exchange(0) < 0 ? -1 : 1;
            std::lock_guard<std::mutex> lock(mutex_);
            index_ = (index_ + step + count) % count;
        }

        app.GetAudioService().ResetDecoder();
        stop_ = false;
        skip_ = 0;
        paused_ = false;
        progress_ = 0;
        active_ = false;
        ESP_LOGI(TAG, "Music stopped");
    }
}

bool LtkkMusic::PlayFile(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (f == nullptr) {
        ESP_LOGE(TAG, "Cannot open %s", path.c_str());
        return false;
    }
    fseek(f, 0, SEEK_END);
    long total = ftell(f);
    fseek(f, 0, SEEK_SET);
    ESP_LOGI(TAG, "Play %s (%ld bytes)", path.c_str(), total);

    auto codec = Board::GetInstance().GetAudioCodec();
    if (!codec->output_enabled()) {
        codec->EnableOutput(true);
    }
    auto& audio = Application::GetInstance().GetAudioService();

    OggDemuxer demuxer;
    demuxer.OnPacket([this, &audio](const uint8_t* data, int sample_rate, int frame_duration_ms,
                                    size_t size) {
        while (paused_.load() && !stop_.load() && skip_.load() == 0) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        if (stop_.load() || skip_.load() != 0) {
            return;
        }
        auto packet = std::make_unique<AudioStreamPacket>();
        packet->sample_rate = sample_rate;
        packet->frame_duration = frame_duration_ms;
        packet->payload.assign(data, data + size);
        audio.PushPacketToDecodeQueue(std::move(packet), true);
    });
    demuxer.Reset();

    constexpr size_t kChunk = 4096;
    std::unique_ptr<uint8_t[]> buf(new uint8_t[kChunk]);
    long done = 0;
    size_t n;
    while (!stop_.load() && skip_.load() == 0 && (n = fread(buf.get(), 1, kChunk, f)) > 0) {
        demuxer.Process(buf.get(), n);
        done += n;
        if (total > 0) {
            progress_ = (int)(done * 1000 / total);
        }
    }
    fclose(f);
    if (stop_.load() || skip_.load() != 0) {
        audio.ResetDecoder();
    }
    progress_ = 0;
    return true;
}
