# LTKK ESP32-S3 2.8-inch Display

Firmware Xiaozhi mang bộ nhận diện **LTKK - Lập Trình Không Khó** cho bo
2.8inch ESP32-S3 Display của LCDWIKI (ES3C28P có cảm ứng / ES3N28P không cảm ứng).

Board này dùng chung sơ đồ chân với `freenove-esp32s3-display-2.8-lcd`.

## Phần cứng
- ESP32-S3, 16MB flash, 8MB PSRAM (OPI)
- Màn hình ILI9341V 2.8" 320x240 (SPI), cảm ứng FT6336G (I2C 0x38)
- Codec ES8311 + ampli FM8002E, micro MEMS
- LED RGB IO42, nút BOOT IO0, đo pin IO9

## Giao diện Lu - LTKK
- Khởi động: logo LTKK + "Học để làm"
- Màn hình chờ 4 kiểu (vuốt ngang hoặc vào Cài đặt để đổi): Đồng hồ / Kim LTKK / Lu ngủ / Thẻ LTKK
- Đang nghe: mắt mở to, sóng âm; Đang nói: 10 biểu cảm + phụ đề có nhãn LU / BẠN
- Hẹn giờ học, Nhạc thẻ nhớ, Radio trực tuyến, Cài Wi-Fi bằng mã QR, Pin yếu
- Bộ icon riêng LTKK: nét tròn xanh ngọc + "nút mạch" cam (lấy từ logo)

## Cài đặt (dễ bấm)
- Chạm nút bánh răng ở góc dưới phải màn hình chờ, hoặc vuốt xuống, hoặc giữ 1-3 giây
- Ô Âm lượng / Độ sáng: nút − và + (mỗi lần 10%)
- Ô Giao diện: chạm để đổi Tối / Sáng
- Ô Màn hình chờ: chạm để đổi kiểu
- Ô Nhạc & Radio: chạm để phát (thẻ nhớ, nếu không có thì radio) / chạm lần nữa để tắt
- Tự đóng sau 15 giây không chạm; nút X để đóng ngay

## Ra lệnh bằng giọng nói (MCP)
- "Mở radio lofi", "Mở nhạc thư giãn", "Đổi đài", "Tắt nhạc"
- "Phát bài <tên> trong thẻ nhớ", "Trong thẻ có bài gì?"
- "Hẹn giờ học 25 phút", "Tạm dừng hẹn giờ", "Thêm 5 phút", "Huỷ hẹn giờ"
- "Đặt thành phố thời tiết là Hà Nội"
- "Đổi màn hình chờ sang kiểu đồng hồ kim"
- YouTube / Spotify: không hỗ trợ (cần ứng dụng và bản quyền của họ)

## Nhạc & radio
- Thẻ microSD FAT32, file .mp3 .wav .aac .m4a .flac .ogg ở thư mục gốc hoặc /music
- Đài radio sửa trong `kStations` (ltkk_music.cc): cần luồng MP3/AAC trực tiếp qua http(s)

## Thao tác chạm
- Chạm 1 lần: bắt đầu / dừng trò chuyện; chạm 2 lần: bắt đầu nghe
- Vuốt xuống / giữ 1-3 giây: cài đặt; vuốt ngang trên màn hình chờ: đổi kiểu
- Giữ hơn 3 giây: cài Wi-Fi
- Chấm cam hiện ở chỗ chạm (tắt bằng LTKK_TOUCH_DEBUG 0 trong ltkk_brand.h).
  Nếu chấm lệch so với ngón tay: chỉnh TOUCH_SWAP_XY / TOUCH_MIRROR_X / TOUCH_MIRROR_Y trong config.h

## Build
```bash
python scripts/build.py ltkk-esp32s3-2.8 --language vi-VN
```
Hoặc dùng GitHub Actions: workflow "LTKK - Build firmware".
Nạp file `build/merged-binary.bin` vào địa chỉ `0x0`.
