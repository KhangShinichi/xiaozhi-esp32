#include "ltkk_music.h"

#include <dirent.h>
#include <driver/sdmmc_host.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_vfs_fat.h>
#include <lwip/sockets.h>
#include <cJSON.h>
#include <sdmmc_cmd.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <memory>

#include "application.h"
#include "audio_codec.h"
#include "audio_service.h"
#include "board.h"
#include "demuxer/ogg_demuxer.h"
#include "esp_audio_dec_default.h"
#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"
#include "protocols/protocol.h"
#include "settings.h"

#define TAG "LtkkMusic"
#define SD_MOUNT_POINT "/sdcard"

// ------------------------------------------------------------------ danh sách đài
// Thêm đài: chỉ cần luồng MP3 hoặc AAC qua http/https (không hỗ trợ .m3u8 / HLS)
static const LtkkStation kStations[] = {
    {"Lofi - SomaFM Fluid", "http://ice1.somafm.com/fluid-128-mp3"},
    {"Chill - SomaFM Groove Salad", "http://ice1.somafm.com/groovesalad-128-mp3"},
    {"Thu gian - SomaFM Lush", "http://ice1.somafm.com/lush-128-mp3"},
    {"Tap trung - SomaFM Deep Space", "http://ice1.somafm.com/deepspaceone-128-mp3"},
};
static constexpr int kStationCount = sizeof(kStations) / sizeof(kStations[0]);

namespace {

std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    return s;
}

std::string ExtOf(const std::string& path) {
    auto dot = path.find_last_of('.');
    return dot == std::string::npos ? "" : Lower(path.substr(dot));
}

bool IsAudioFile(const std::string& name) {
    static const char* kExts[] = {".mp3", ".wav", ".aac", ".m4a", ".flac", ".ogg"};
    std::string ext = ExtOf(name);
    for (auto e : kExts) {
        if (ext == e) {
            return true;
        }
    }
    return false;
}

std::string TitleOf(const std::string& path) {
    std::string name = path.substr(path.find_last_of('/') + 1);
    auto dot = name.find_last_of('.');
    if (dot != std::string::npos) {
        name.resize(dot);
    }
    std::replace(name.begin(), name.end(), '_', ' ');
    return name;
}

esp_audio_simple_dec_type_t DecoderTypeFor(const std::string& name) {
    std::string n = Lower(name);
    if (n.find(".wav") != std::string::npos) return ESP_AUDIO_SIMPLE_DEC_TYPE_WAV;
    if (n.find(".m4a") != std::string::npos) return ESP_AUDIO_SIMPLE_DEC_TYPE_M4A;
    if (n.find(".flac") != std::string::npos) return ESP_AUDIO_SIMPLE_DEC_TYPE_FLAC;
    if (n.find("aac") != std::string::npos) return ESP_AUDIO_SIMPLE_DEC_TYPE_AAC;
    return ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
}

void RegisterDecoders() {
    static bool registered = false;
    if (!registered) {
        esp_audio_dec_register_default();
        esp_audio_simple_dec_register_default();
        registered = true;
    }
}

// Đưa PCM đã giải mã (mọi tần số / số kênh) ra loa: trộn về mono, đổi tần số
class PcmSink {
public:
    PcmSink(std::atomic<bool>& paused, std::function<bool()> interrupted)
        : paused_(paused), interrupted_(std::move(interrupted)) {
        codec_ = Board::GetInstance().GetAudioCodec();
        out_rate_ = codec_->output_sample_rate();
        out_channels_ = codec_->output_channels();
        out_.reserve(kBlock * 2);
    }

    void Write(const int16_t* pcm, size_t total_samples, int rate, int channels) {
        if (channels < 1 || rate <= 0) {
            return;
        }
        size_t frames = total_samples / channels;
        if (frames == 0) {
            return;
        }
        mono_.resize(frames);
        for (size_t i = 0; i < frames; i++) {
            int32_t sum = 0;
            for (int c = 0; c < channels; c++) {
                sum += pcm[i * channels + c];
            }
            mono_[i] = (int16_t)(sum / channels);
        }
        double step = (double)rate / out_rate_;
        auto frame = [&](long k) -> int32_t { return k < 0 ? prev_ : mono_[k]; };
        while (pos_ < (double)(frames - 1)) {
            long i = (long)(pos_ < 0 ? -1 : pos_);
            double f = pos_ - i;
            int32_t v = (int32_t)(frame(i) * (1.0 - f) + frame(i + 1) * f);
            Emit((int16_t)v);
            pos_ += step;
        }
        pos_ -= frames;
        prev_ = mono_[frames - 1];
    }

    void Flush() {
        if (!out_.empty() && !interrupted_()) {
            Output();
        }
        out_.clear();
    }

private:
    static constexpr size_t kBlock = 960;
    AudioCodec* codec_;
    int out_rate_;
    int out_channels_;
    std::atomic<bool>& paused_;
    std::function<bool()> interrupted_;
    std::vector<int16_t> mono_;
    std::vector<int16_t> out_;
    double pos_ = 0;
    int16_t prev_ = 0;

    void Emit(int16_t v) {
        for (int c = 0; c < out_channels_; c++) {
            out_.push_back(v);
        }
        if (out_.size() >= kBlock * out_channels_) {
            Output();
            out_.clear();
        }
    }

    void Output() {
        while (paused_.load() && !interrupted_()) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        if (interrupted_()) {
            return;
        }
        if (!codec_->output_enabled()) {
            codec_->EnableOutput(true);
        }
        codec_->OutputData(out_);
    }
};

// Bộ giải mã dùng chung cho file và luồng radio
class StreamDecoder {
public:
    explicit StreamDecoder(esp_audio_simple_dec_type_t type) {
        RegisterDecoders();
        esp_audio_simple_dec_cfg_t cfg = {};
        cfg.dec_type = type;
        if (esp_audio_simple_dec_open(&cfg, &handle_) != ESP_AUDIO_ERR_OK) {
            handle_ = nullptr;
            ESP_LOGE(TAG, "Cannot open decoder %d", (int)type);
        }
        out_size_ = 4608;
        out_ = (uint8_t*)heap_caps_malloc(out_size_, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    ~StreamDecoder() {
        if (handle_ != nullptr) {
            esp_audio_simple_dec_close(handle_);
        }
        free(out_);
    }
    bool ok() const { return handle_ != nullptr && out_ != nullptr; }

    // Trả về false nếu dữ liệu hỏng quá nhiều
    bool Feed(const uint8_t* data, size_t len, bool eos, PcmSink& sink,
              const std::function<bool()>& interrupted) {
        esp_audio_simple_dec_raw_t raw = {};
        raw.buffer = const_cast<uint8_t*>(data);
        raw.len = len;
        raw.eos = eos;
        while ((raw.len > 0 || eos) && !interrupted()) {
            esp_audio_simple_dec_out_t out = {};
            out.buffer = out_;
            out.len = out_size_;
            auto ret = esp_audio_simple_dec_process(handle_, &raw, &out);
            if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                uint8_t* bigger = (uint8_t*)heap_caps_realloc(out_, out.needed_size,
                                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (bigger == nullptr) {
                    return false;
                }
                out_ = bigger;
                out_size_ = out.needed_size;
                continue;
            }
            if (ret != ESP_AUDIO_ERR_OK) {
                if (++errors_ > 50) {
                    ESP_LOGE(TAG, "Too many decode errors (%d)", (int)ret);
                    return false;
                }
            } else {
                errors_ = 0;
            }
            if (out.decoded_size > 0) {
                if (info_.sample_rate == 0) {
                    esp_audio_simple_dec_get_info(handle_, &info_);
                    ESP_LOGI(TAG, "Stream: %lu Hz, %d ch, %d bit",
                             (unsigned long)info_.sample_rate, info_.channel,
                             info_.bits_per_sample);
                }
                if (info_.bits_per_sample == 16) {
                    sink.Write(reinterpret_cast<int16_t*>(out_), out.decoded_size / 2,
                               info_.sample_rate, info_.channel);
                }
            }
            if (raw.consumed == 0 && out.decoded_size == 0) {
                break;  // cần thêm dữ liệu
            }
            raw.buffer += raw.consumed;
            raw.len -= raw.consumed;
            if (raw.len == 0 && eos && out.decoded_size == 0) {
                break;
            }
        }
        return true;
    }

private:
    esp_audio_simple_dec_handle_t handle_ = nullptr;
    esp_audio_simple_dec_info_t info_ = {};
    uint8_t* out_ = nullptr;
    uint32_t out_size_ = 0;
    int errors_ = 0;
};

}  // namespace

LtkkMusic& LtkkMusic::GetInstance() {
    static LtkkMusic instance;
    return instance;
}

// ------------------------------------------------------------------ thẻ nhớ
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
        ESP_LOGW(TAG, "Mount SD failed: %s", esp_err_to_name(err));
        error_ = "Không đọc được thẻ nhớ microSD (cần định dạng FAT32).";
        return false;
    }
    mounted_ = true;
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
            if (entry->d_type != DT_DIR && name[0] != '.' && IsAudioFile(name)) {
                files_.push_back(std::string(dir_path) + "/" + name);
            }
        }
        closedir(dir);
    }
    std::sort(files_.begin(), files_.end());
    if (index_ >= (int)files_.size()) {
        index_ = 0;
    }
}

std::vector<std::string> LtkkMusic::List() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> titles;
    if (Mount()) {
        ScanLocked();
        for (const auto& f : files_) {
            titles.push_back(TitleOf(f));
        }
    }
    return titles;
}

std::vector<std::string> LtkkMusic::RadioList() {
    std::vector<std::string> names;
    for (const auto& s : kStations) {
        names.push_back(s.name);
    }
    return names;
}

std::string LtkkMusic::CurrentTitle() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (mode_.load() != kModeSd) {
        return custom_url_.empty() ? kStations[station_ % kStationCount].name : custom_name_;
    }
    if (files_.empty()) {
        return "";
    }
    return TitleOf(files_[index_ % files_.size()]);
}

std::string LtkkMusic::LastError() {
    std::lock_guard<std::mutex> lock(mutex_);
    return error_;
}

// ------------------------------------------------------------------ điều khiển
bool LtkkMusic::Play(const std::string& name) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        error_.clear();
        if (!Mount()) {
            return false;
        }
        ScanLocked();
        if (files_.empty()) {
            error_ = "Thẻ nhớ chưa có bài nào (.mp3 .wav .aac .m4a .flac .ogg).";
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
    return Request(kModeSd);
}

bool LtkkMusic::PlayRadio(int index) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        error_.clear();
        station_ = ((index % kStationCount) + kStationCount) % kStationCount;
        custom_url_.clear();
    }
    return Request(kModeRadio);
}

bool LtkkMusic::PlayRadioByName(const std::string& name) {
    std::string wanted = Lower(name);
    for (int i = 0; i < kStationCount; i++) {
        if (wanted.empty() || Lower(kStations[i].name).find(wanted) != std::string::npos) {
            return PlayRadio(i);
        }
    }
    return PlayRadio(0);
}

bool LtkkMusic::PlayUrl(const std::string& url, const std::string& name) {
    if (url.rfind("http", 0) != 0) {
        std::lock_guard<std::mutex> lock(mutex_);
        error_ = "Địa chỉ phải bắt đầu bằng http:// hoặc https://";
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        error_.clear();
        custom_url_ = url;
        custom_name_ = name.empty() ? "Radio" : name;
    }
    return Request(kModeRadio);
}

bool LtkkMusic::PlayAny() {
    if (!List().empty()) {
        return Play();
    }
    return PlayRadio(station_);
}

bool LtkkMusic::Request(Mode mode) {
    if (active_.load() || pending_.load()) {
        stop_ = true;
        pending_ = false;
        for (int i = 0; i < 100 && active_.load(); i++) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
    stop_ = false;
    skip_ = 0;
    paused_ = false;
    mode_ = mode;
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
        PlayAny();
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

// ------------------------------------------------------------------ nhạc trực tuyến
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

bool HttpGetText(const std::string& url, std::string& body) {
    auto http = Board::GetInstance().GetNetwork()->CreateHttp(0);
    if (!http) {
        return false;
    }
    http->SetTimeout(10000);
    http->SetHeader("User-Agent", "LTKK-Lu/1.0");
    bool ok = false;
    if (auto opened = http->Open("GET", url); opened) {
        auto status = http->GetStatusCode();
        if (status && *status == 200) {
            body = http->ReadAll();
            ok = true;
        }
    }
    http->Close();
    return ok;
}

}  // namespace

bool LtkkMusic::SearchRadio(const std::string& query, const std::string& countrycode) {
    // Radio Browser: danh bạ đài mở, miễn phí, không cần khoá
    static const char* kHosts[] = {"de1.api.radio-browser.info", "nl1.api.radio-browser.info",
                                   "at1.api.radio-browser.info"};
    std::string common = "&limit=12&hidebroken=true&order=clickcount&reverse=true";
    if (!countrycode.empty()) {
        common += "&countrycode=" + UrlEncode(countrycode);
    }
    for (const char* field : {"name", "tag"}) {
        for (const char* host : kHosts) {
            std::string body;
            std::string url = std::string("https://") + host + "/json/stations/search?" + field +
                              "=" + UrlEncode(query) + common;
            if (!HttpGetText(url, body)) {
                continue;  // thử máy chủ khác
            }
            cJSON* root = cJSON_Parse(body.c_str());
            if (root == nullptr) {
                continue;
            }
            std::string found_url, found_name;
            cJSON* item;
            cJSON_ArrayForEach(item, root) {
                cJSON* u = cJSON_GetObjectItem(item, "url_resolved");
                cJSON* n = cJSON_GetObjectItem(item, "name");
                cJSON* codec = cJSON_GetObjectItem(item, "codec");
                cJSON* hls = cJSON_GetObjectItem(item, "hls");
                if (!cJSON_IsString(u) || !cJSON_IsString(codec)) {
                    continue;
                }
                std::string c = Lower(codec->valuestring);
                if ((c == "mp3" || c.find("aac") != std::string::npos) &&
                    !(cJSON_IsNumber(hls) && hls->valueint == 1)) {
                    found_url = u->valuestring;
                    if (c.find("aac") != std::string::npos &&
                        Lower(found_url).find("aac") == std::string::npos) {
                        found_url += (found_url.find('?') == std::string::npos ? "?aac" : "&aac");
                    }
                    found_name = cJSON_IsString(n) ? n->valuestring : query;
                    break;
                }
            }
            cJSON_Delete(root);
            if (!found_url.empty()) {
                ESP_LOGI(TAG, "Radio found: %s", found_name.c_str());
                return PlayUrl(found_url, found_name);
            }
            break;  // máy chủ trả lời nhưng không có kết quả: thử trường khác
        }
    }
    std::lock_guard<std::mutex> lock(mutex_);
    error_ = "Không tìm thấy đài phù hợp với: " + query;
    return false;
}

void LtkkMusic::SetMusicServer(const std::string& url) {
    std::string u = url;
    if (!u.empty() && u.rfind("http", 0) != 0) {
        u = "http://" + u;
    }
    while (!u.empty() && u.back() == '/') {
        u.pop_back();
    }
    std::lock_guard<std::mutex> lock(mutex_);
    server_ = u;
    Settings settings("ltkk", true);
    settings.SetString("music_srv", u);
}

std::string LtkkMusic::GetMusicServer() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (server_.empty()) {
        Settings settings("ltkk", false);
        server_ = settings.GetString("music_srv", "");
    }
    return server_;
}

// Tìm phần mềm LTKK (máy chủ nhạc) trong mạng nhà bằng gói UDP quảng bá
std::string LtkkMusic::DiscoverServer() {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        return "";
    }
    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
    struct timeval tv = {1, 500000};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    struct sockaddr_in to = {};
    to.sin_family = AF_INET;
    to.sin_port = htons(8766);
    to.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    const char* ask = "LTKK_MUSIC_DISCOVER";
    std::string found;
    for (int attempt = 0; attempt < 2 && found.empty(); attempt++) {
        sendto(sock, ask, strlen(ask), 0, (struct sockaddr*)&to, sizeof(to));
        char buf[64] = {};
        struct sockaddr_in from = {};
        socklen_t from_len = sizeof(from);
        int n = recvfrom(sock, buf, sizeof(buf) - 1, 0, (struct sockaddr*)&from, &from_len);
        if (n > 11 && strncmp(buf, "LTKK_MUSIC ", 11) == 0) {
            int port = atoi(buf + 11);
            char ip[16];
            inet_ntoa_r(from.sin_addr, ip, sizeof(ip));
            found = std::string("http://") + ip + ":" + std::to_string(port > 0 ? port : 8765);
        }
    }
    close(sock);
    if (!found.empty()) {
        ESP_LOGI(TAG, "Music server found: %s", found.c_str());
        SetMusicServer(found);
    }
    return found;
}

bool LtkkMusic::PlaySong(const std::string& song, const std::string& artist) {
    std::string base = GetMusicServer();
    std::string body;
    std::string query = "/stream_pcm?song=" + UrlEncode(song) + "&artist=" + UrlEncode(artist);
    bool ok = !base.empty() && HttpGetText(base + query, body);
    if (!ok) {
        // Máy chủ cũ không trả lời (đổi IP?) -> tìm lại trong mạng nhà
        base = DiscoverServer();
        ok = !base.empty() && HttpGetText(base + query, body);
    }
    if (!ok) {
        std::lock_guard<std::mutex> lock(mutex_);
        error_ = "Chưa kết nối được máy chủ nhạc. Hãy bật Máy chủ nhạc trong phần mềm LTKK trên máy "
                 "tính (cùng mạng Wi-Fi), hoặc dùng radio.";
        return false;
    }
    cJSON* root = cJSON_Parse(body.c_str());
    std::string audio, title;
    if (root != nullptr) {
        cJSON* a = cJSON_GetObjectItem(root, "audio_url");
        cJSON* t = cJSON_GetObjectItem(root, "title");
        cJSON* ar = cJSON_GetObjectItem(root, "artist");
        if (cJSON_IsString(a)) {
            audio = a->valuestring;
        }
        title = cJSON_IsString(t) ? t->valuestring : song;
        if (cJSON_IsString(ar) && strlen(ar->valuestring) > 0) {
            title += std::string(" - ") + ar->valuestring;
        }
        cJSON_Delete(root);
    }
    if (audio.empty()) {
        std::lock_guard<std::mutex> lock(mutex_);
        error_ = "Máy chủ nhạc không có bài: " + song;
        return false;
    }
    if (audio[0] == '/') {
        audio = base + audio;  // đường dẫn tương đối
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        error_.clear();
        custom_url_ = audio;
        custom_name_ = title;
    }
    return Request(kModeSong);
}

// ------------------------------------------------------------------ luồng phát
void LtkkMusic::EnsureTask() {
    if (task_ == nullptr) {
        xTaskCreatePinnedToCore(TaskEntry, "ltkk_music", 8192, this, 4, &task_, 1);
    }
}

void LtkkMusic::TaskEntry(void* arg) { static_cast<LtkkMusic*>(arg)->TaskLoop(); }

// Chờ Lu nói xong. Nếu Lu chuyển sang nghe tiếp thì đóng cuộc trò chuyện.
bool LtkkMusic::WaitUntilIdle() {
    auto& app = Application::GetInstance();
    bool saw_speaking = false;
    bool closed = false;
    while (pending_.load() && !stop_.load()) {
        auto state = app.GetDeviceState();
        if (state == kDeviceStateIdle) {
            return true;
        }
        if (state == kDeviceStateSpeaking) {
            saw_speaking = true;
        } else if (state == kDeviceStateListening && saw_speaking && !closed) {
            closed = true;
            app.Schedule([]() { Application::GetInstance().ToggleChatState(); });
        }
        vTaskDelay(pdMS_TO_TICKS(150));
    }
    return false;
}

void LtkkMusic::TaskLoop() {
    auto& app = Application::GetInstance();
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (!WaitUntilIdle()) {
            pending_ = false;
            stop_ = false;
            continue;
        }
        pending_ = false;
        active_ = true;
        paused_ = false;
        app.GetAudioService().ResetDecoder();

        int failures = 0;
        while (!stop_.load()) {
            bool ok;
            int count;
            if (mode_.load() == kModeSong) {
                std::string url;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    url = custom_url_;
                }
                PlayStream(url);  // phát một lần rồi dừng
                break;
            }
            if (mode_.load() == kModeRadio) {
                std::string url;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    url = custom_url_.empty() ? kStations[station_ % kStationCount].url
                                              : custom_url_;
                }
                count = custom_url_.empty() ? kStationCount : 1;
                ok = PlayStream(url);
                if (stop_.load()) {
                    break;
                }
                int step = skip_.exchange(0);
                if (step != 0) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    custom_url_.clear();
                    station_ = (station_ + step + kStationCount) % kStationCount;
                    failures = 0;
                    continue;
                }
                // Mất kết nối: thử lại tối đa 3 lần
                if (++failures >= 3) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    error_ = "Không kết nối được đài radio.";
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(2000));
                (void)ok;
                (void)count;
                continue;
            }

            std::string path;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                count = (int)files_.size();
                if (count == 0) {
                    break;
                }
                path = files_[index_ % count];
            }
            ok = ExtOf(path) == ".ogg" ? PlayOggFile(path) : PlayDecodedFile(path);
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
        buffering_ = false;
        progress_ = 0;
        active_ = false;
        ESP_LOGI(TAG, "Music stopped");
    }
}

// .ogg (Opus): đi qua bộ giải mã có sẵn của Xiaozhi
bool LtkkMusic::PlayOggFile(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (f == nullptr) {
        return false;
    }
    fseek(f, 0, SEEK_END);
    long total = ftell(f);
    fseek(f, 0, SEEK_SET);

    auto codec = Board::GetInstance().GetAudioCodec();
    if (!codec->output_enabled()) {
        codec->EnableOutput(true);
    }
    auto& audio = Application::GetInstance().GetAudioService();
    OggDemuxer demuxer;
    demuxer.OnPacket([this, &audio](const uint8_t* data, int sample_rate, int frame_duration_ms,
                                    size_t size) {
        while (paused_.load() && !Interrupted()) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        if (Interrupted()) {
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
    while (!Interrupted() && (n = fread(buf.get(), 1, kChunk, f)) > 0) {
        demuxer.Process(buf.get(), n);
        done += n;
        progress_ = total > 0 ? (int)(done * 1000 / total) : 0;
    }
    fclose(f);
    if (Interrupted()) {
        audio.ResetDecoder();
    }
    return true;
}

// .mp3 .wav .aac .m4a .flac: giải mã tại chỗ rồi phát thẳng ra loa
bool LtkkMusic::PlayDecodedFile(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (f == nullptr) {
        return false;
    }
    fseek(f, 0, SEEK_END);
    long total = ftell(f);
    fseek(f, 0, SEEK_SET);
    ESP_LOGI(TAG, "Play %s (%ld bytes)", path.c_str(), total);

    auto interrupted = [this]() { return Interrupted(); };
    StreamDecoder decoder(DecoderTypeFor(path));
    if (!decoder.ok()) {
        fclose(f);
        return false;
    }
    PcmSink sink(paused_, interrupted);

    constexpr size_t kChunk = 4096;
    std::unique_ptr<uint8_t[]> buf(new uint8_t[kChunk]);
    long done = 0;
    size_t n;
    bool ok = true;
    while (!Interrupted() && (n = fread(buf.get(), 1, kChunk, f)) > 0) {
        done += n;
        bool eos = done >= total;
        if (!decoder.Feed(buf.get(), n, eos, sink, interrupted)) {
            ok = false;
            break;
        }
        progress_ = total > 0 ? (int)(done * 1000 / total) : 0;
    }
    sink.Flush();
    fclose(f);
    progress_ = 0;
    return ok;
}

// Radio: đọc luồng HTTP, giải mã MP3/AAC và phát liên tục
bool LtkkMusic::PlayStream(const std::string& url) {
    buffering_ = true;
    progress_ = 1000;
    ESP_LOGI(TAG, "Radio: %s", url.c_str());
    auto http = Board::GetInstance().GetNetwork()->CreateHttp(0);
    if (!http) {
        buffering_ = false;
        return false;
    }
    http->SetTimeout(10000);
    http->SetHeader("Accept-Encoding", "identity");
    http->SetHeader("User-Agent", "LTKK-Lu/1.0");
    bool ok = false;
    if (auto opened = http->Open("GET", url); opened) {
        auto status = http->GetStatusCode();
        if (status && *status >= 200 && *status < 300) {
            auto interrupted = [this]() { return Interrupted(); };
            StreamDecoder decoder(DecoderTypeFor(url));
            PcmSink sink(paused_, interrupted);
            if (decoder.ok()) {
                ok = true;
                std::unique_ptr<char[]> buf(new char[4096]);
                while (!Interrupted()) {
                    auto size = http->Read(buf.get(), 4096);
                    if (!size || *size == 0) {
                        ok = false;  // mất kết nối hoặc đài ngừng phát
                        break;
                    }
                    buffering_ = false;
                    if (!decoder.Feed(reinterpret_cast<uint8_t*>(buf.get()), *size, false, sink,
                                      interrupted)) {
                        ok = false;
                        break;
                    }
                }
                sink.Flush();
            }
        } else {
            ESP_LOGW(TAG, "Radio HTTP status error");
        }
    } else {
        ESP_LOGW(TAG, "Radio open failed");
    }
    http->Close();
    buffering_ = false;
    return ok;
}
