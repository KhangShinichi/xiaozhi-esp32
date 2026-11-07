# LTKK ESP32-S3 2.8-inch Display

Firmware Xiaozhi mang bộ nhận diện **LTKK - Lập Trình Không Khó** cho bo
2.8inch ESP32-S3 Display của LCDWIKI (ES3C28P có cảm ứng / ES3N28P không cảm ứng).

Board này dùng chung sơ đồ chân với `freenove-esp32s3-display-2.8-lcd`.

## Phần cứng
- ESP32-S3, 16MB flash, 8MB PSRAM (OPI)
- Màn hình ILI9341V 2.8" 320x240 (SPI), cảm ứng FT6336G (I2C 0x38)
- Codec ES8311 + ampli FM8002E, micro MEMS
- LED RGB IO42, nút BOOT IO0, đo pin IO9

## Giao diện LTKK
- Màn hình chào: logo LTKK + "Học để làm", 2,5 giây (`ltkk_brand.h`)
- Giao diện tối (mặc định): nền navy có mạch điện, bong bóng chat xanh / cam
- Giao diện sáng: nền trắng xanh, chữ navy
- Đổi màu / tắt màn hình chào: sửa `ltkk_brand.h`

## Thao tác
- Chạm 1 lần: bắt đầu / dừng trò chuyện
- Chạm 2 lần: bắt đầu nghe
- Giữ hơn 3 giây: vào chế độ cài Wi-Fi

## Build
```bash
python scripts/build.py ltkk-esp32s3-2.8 --language vi-VN
```
Hoặc dùng GitHub Actions: workflow "LTKK - Build firmware".
Nạp file `build/merged-binary.bin` vào địa chỉ `0x0`.
