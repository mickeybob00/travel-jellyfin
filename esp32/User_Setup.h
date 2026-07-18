// ============================================================
// TFT_eSPI User_Setup.h — ESP32-035 (3.5" CYD resistive)
// ST7796 display + XPT2046 touch (shared SPI bus)
// Portrait: 320 wide x 480 tall
// ============================================================

#define USER_SETUP_INFO "ESP32-035"

// ---- Display driver ----
#define ST7796_DRIVER
#define TFT_WIDTH  320
#define TFT_HEIGHT 480
#define TFT_RGB_ORDER TFT_BGR
#define TFT_INVERSION_OFF

// ---- Display SPI pins ----
#define TFT_MISO 12
#define TFT_MOSI 13
#define TFT_SCLK 14
#define TFT_CS   15
#define TFT_DC    2
#define TFT_RST  -1

// ---- Backlight ----
#define TFT_BL   27
#define TFT_BACKLIGHT_ON HIGH

// ---- Touch (XPT2046 resistive, shares SPI bus) ----
#define TOUCH_CS   33
#define TOUCH_CLK  14
#define TOUCH_MOSI 13
#define TOUCH_MISO 12
#define TOUCH_IRQ  36

// ---- SPI speeds ----
#define SPI_FREQUENCY       65000000
#define SPI_READ_FREQUENCY  20000000
#define SPI_TOUCH_FREQUENCY 2500000

// ---- Smooth fonts (optional, uses flash) ----
#define SMOOTH_FONT