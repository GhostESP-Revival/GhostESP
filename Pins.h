constexpr int TFT_SCLK_PIN = 7;
constexpr int TFT_MOSI_PIN = 6;
constexpr int TFT_CS_PIN = 5;
constexpr int TFT_DC_PIN = 4;
constexpr int TFT_RST_PIN = 8;
constexpr int TFT_BL_PIN = 15;

constexpr int JOYSTICK_X_PIN = 13;
constexpr int JOYSTICK_Y_PIN = 12;
constexpr int JOYSTICK_BUTTON_PIN = 11;
constexpr int BACK_BUTTON_PIN = 39;

constexpr int NEOPIXEL_PIN = 48;
constexpr int NEOPIXEL_COUNT = 1;

// ESP32-S3 boards with OPI PSRAM reserve GPIO33-GPIO37 for flash/PSRAM.
// The SD card stays on its own remapped SPI bus, while CC1101 and PN532
// share a separate SPI bus.
constexpr int SD_MISO_PIN = 9;
constexpr int SD_SCLK_PIN = 10;
constexpr int SD_MOSI_PIN = 14;
constexpr int SD_CS_PIN = 38;

constexpr int CC1101_SPI_MOSI_PIN = 14;
constexpr int CC1101_SPI_MISO_PIN = 9;
constexpr int CC1101_SPI_SCLK_PIN = 10;

constexpr int CC1101_CS_PIN = 16;
constexpr int CC1101_GDO0_PIN = 17;
constexpr int CC1101_GDO2_PIN = 47;

// sorry scl is on pin 21 and sda is on pin 18

constexpr int PN532_SCL_PIN = 21;
constexpr int PN532_SDA_PIN = 18;


//Coming soon: I have not wired up the IR receiver and transmitter yet, so these pins are not in use.
// constexpr int IR_RX_PIN = 40;
// constexpr int IR_TX_PIN = 41;

