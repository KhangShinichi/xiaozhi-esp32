# LTKK ESP32-S3 2.8-inch Display

Firmware Xiaozhi mang bộ nhận diện **LTKK - Lập Trình Không Khó** cho bo
2.8inch ESP32-S3 Display của LCDWIKI (ES3C28P có cảm ứng / ES3N28P không cảm ứng).

Board này dùng chung sơ đồ chân với `freenove-esp32s3-display-2.8-lcd`.

## Phần cứng
- ESP32-S3, 16MB flash, 8MB PSRAM (OPI)
- Màn hình ILI9341V 2.8" 320x240 (SPI), cảm ứng FT6336G (I2C 0x38)
- Codec ES8311 + ampli FM8002E, micro MEMS
- LED RGB IO42, nút BOOT IO0, đo pin IO9

## Giao diện Lu - LTKK (theo bộ thiết kế 11 màn hình)
1. Khởi động: logo LTKK + "Học để làm" (`ltkk_brand.cc`)
2. Chờ: đồng hồ lớn, thứ/ngày, thời tiết, gợi ý "Nói Hi ESP hoặc chạm để gọi Lu"
3. Đang nghe: mắt mở to, sóng âm cam/xanh, "Lu đang nghe nè…"
4. Đang nói: biểu cảm + khung phụ đề có nhãn LU / BẠN
5. (Kiểu tin nhắn WeChat: tuỳ chọn build `display_style=wechat`, khi đó không có khuôn mặt)
6. Hẹn giờ học: vòng đếm ngược, nút Tạm dừng / Huỷ, chuông khi hết giờ
7. Phát nhạc: file .ogg từ thẻ microSD, nút lùi / dừng / tiếp
8. Cài Wi-Fi: mã QR + 3 bước
9. 10 biểu cảm robot, tự chớp mắt
10. Cài đặt nhanh: vuốt xuống hoặc giữ 1-3 giây - âm lượng, độ sáng, giao diện, Wi-Fi, pin
11. Pin yếu: khung thông báo trắng viền cam

## Ra lệnh bằng giọng nói (MCP)
- "Hẹn giờ học 25 phút", "Thêm 5 phút", "Tạm dừng hẹn giờ", "Còn bao lâu?", "Huỷ hẹn giờ"
- "Mở nhạc", "Phát bài <tên>", "Bài tiếp", "Tắt nhạc", "Trong thẻ nhớ có bài gì?"
- "Đặt thành phố thời tiết là Đà Nẵng"
- "Chuyển giao diện sáng/tối", "Tăng độ sáng", "Giảm âm lượng"

## Nhạc trên thẻ nhớ
- Thẻ microSD định dạng FAT32, chép file `.ogg` vào thư mục gốc hoặc `/music`.
- Đổi mp3 sang ogg (Opus mono 16 kHz):
  `ffmpeg -i bai_hat.mp3 -c:a libopus -b:a 32k -ac 1 -ar 16000 -frame_duration 60 bai_hat.ogg`
- Đặt tên file không dấu để hiển thị đúng.

## Thao tác chạm
- Chạm 1 lần: bắt đầu / dừng trò chuyện
- Chạm 2 lần: bắt đầu nghe
- Vuốt xuống, hoặc giữ 1-3 giây: cài đặt nhanh
- Giữ hơn 3 giây: vào chế độ cài Wi-Fi
- Cảm ứng lệch: chỉnh `TOUCH_SWAP_XY / TOUCH_MIRROR_X / TOUCH_MIRROR_Y` trong `config.h`

## Build
```bash
python scripts/build.py ltkk-esp32s3-2.8 --language vi-VN
```
Hoặc dùng GitHub Actions: workflow "LTKK - Build firmware".
Nạp file `build/merged-binary.bin` vào địa chỉ `0x0`.
