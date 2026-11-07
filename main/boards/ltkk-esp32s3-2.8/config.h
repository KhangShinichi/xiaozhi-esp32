#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

// LTKK - Lập Trình Không Khó
// Bo 2.8inch ESP32-S3 Display (LCDWIKI ES3C28P / ES3N28P)
// Sơ đồ chân theo https://www.lcdwiki.com/2.8inch_ESP32-S3_Display

#include <driver/gpio.h>

#define AUDIO_INPUT_SAMPLE_RATE  24000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000

// Âm thanh I2S (codec ES8311)
#define AUDIO_I2S_GPIO_MCLK      GPIO_NUM_4
#define AUDIO_I2S_GPIO_BCLK      GPIO_NUM_5
#define AUDIO_I2S_GPIO_DIN       GPIO_NUM_6
#define AUDIO_I2S_GPIO_WS        GPIO_NUM_7
#define AUDIO_I2S_GPIO_DOUT      GPIO_NUM_8
#define AUDIO_CODEC_PA_PIN       GPIO_NUM_1   // bật ampli FM8002E

// I2C dùng chung cho codec ES8311 và cảm ứng FT6336G
#define AUDIO_CODEC_I2C_NUM      I2C_NUM_0
#define AUDIO_CODEC_I2C_SCL_PIN  GPIO_NUM_15
#define AUDIO_CODEC_I2C_SDA_PIN  GPIO_NUM_16
#define AUDIO_CODEC_ES8311_ADDR  ES8311_CODEC_DEFAULT_ADDR

#define TOUCH_I2C_ADDR           0x38         // FT6336G

// Nút BOOT và đèn LED RGB
#define BOOT_BUTTON_GPIO GPIO_NUM_0
#define BUILTIN_LED_GPIO GPIO_NUM_42

// Màn hình ILI9341V 2.8 inch, SPI
#define DISPLAY_BACKLIGHT_PIN GPIO_NUM_45

#define DISPLAY_RST_PIN       GPIO_NUM_NC
#define DISPLAY_SCK_PIN       GPIO_NUM_12
#define DISPLAY_DC_PIN        GPIO_NUM_46
#define DISPLAY_CS_PIN        GPIO_NUM_10
#define DISPLAY_MOSI_PIN      GPIO_NUM_11
#define DISPLAY_MIS0_PIN      GPIO_NUM_13
#define DISPLAY_SPI_SCLK_HZ   (20 * 1000 * 1000)

#define LCD_SPI_HOST          SPI3_HOST

#define LCD_TYPE_ILI9341_SERIAL
#define DISPLAY_WIDTH         320
#define DISPLAY_HEIGHT        240
#define DISPLAY_MIRROR_X      false
#define DISPLAY_MIRROR_Y      false
#define DISPLAY_SWAP_XY       true
#define DISPLAY_INVERT_COLOR  true
#define DISPLAY_RGB_ORDER     LCD_RGB_ELEMENT_ORDER_BGR
#define DISPLAY_OFFSET_X      0
#define DISPLAY_OFFSET_Y      0
#define DISPLAY_BACKLIGHT_OUTPUT_INVERT false
#define DISPLAY_SPI_MODE      0

#endif  // _BOARD_CONFIG_H_
