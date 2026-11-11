// ============================================================================
// cardputer_hal.h — M5Stack Cardputer / Cardputer ADV hardware shim
// ============================================================================
// Upstream OUI-SPY targets a XIAO ESP32-S3 with a piezo on GPIO3, a NeoPixel
// on GPIO4 and an inverted status LED on GPIO21. On the Cardputer GPIO3/4 are
// keyboard matrix lines and GPIO21 is the on-board WS2812, so the hardware
// calls are routed here instead:
//
//   piezo (LEDC ch 0)     -> M5 Speaker (I2S NS4168 / ES8311 on the ADV)
//   NeoPixel on GPIO4     -> on-board WS2812 on GPIO21 via M5.Led
//   status LED on GPIO21  -> status dot on the display (g_statusLed)
//   BOOT button GPIO0     -> unchanged (Cardputer G0 button)
#pragma once
#include <M5Cardputer.h>

// ---------------------------------------------------------------- speaker
static uint16_t g_buzzFreq   = 2000;
static bool     g_buzzOn     = false;
static uint8_t  g_spkVolume  = 160;   // 0-255, adjustable from the keyboard

static inline void buzzSetFreq(uint32_t f) {
    g_buzzFreq = (uint16_t)f;
    if (g_buzzOn) M5Cardputer.Speaker.tone(g_buzzFreq);
}

extern bool buzzerEnabled;

// Mirrors ledcWrite(0, duty): any duty > 0 = tone on, 0 = off. Mute wins
// everywhere on the Cardputer (upstream's G0-hold and fleet "identify"
// beeps bypass the setting; here they stay silent when muted).
static inline void buzzWrite(uint32_t duty) {
    if (duty > 0 && buzzerEnabled) {
        if (!g_buzzOn) M5Cardputer.Speaker.tone(g_buzzFreq);
        g_buzzOn = true;
    } else {
        if (g_buzzOn) M5Cardputer.Speaker.stop();
        g_buzzOn = false;
    }
}

// --------------------------------------------------- status "LED" (screen)
static volatile bool g_statusLed = false;
static inline void statusLedWrite(bool on) { g_statusLed = on; }

// ----------------------------------------------------- RGB LED (WS2812 G21)
static bool g_rgbMute = false;   // LED disabled or stealth mode (set by the UI)
// Drop-in for the subset of Adafruit_NeoPixel the upstream code uses.
class CardRgbLed {
public:
    void begin() { M5.Led.setAutoDisplay(false); M5.Led.setBrightness(255); }
    void setBrightness(uint8_t b) { _bright = b; }
    void clear() { _r = _g = _b = 0; }
    static uint32_t Color(uint8_t r, uint8_t g, uint8_t b) {
        return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
    }
    void setPixelColor(uint16_t, uint32_t c) {
        _r = (c >> 16) & 0xFF; _g = (c >> 8) & 0xFF; _b = c & 0xFF;
    }
    void show() {
        if (!M5.Led.isEnabled()) return;
        if (g_rgbMute) { M5.Led.setAllColor(0, 0, 0); M5.Led.display(); return; }
        // Adafruit's setBrightness() scales every pixel; keep that behaviour.
        uint16_t s = (uint16_t)_bright + 1;
        M5.Led.setAllColor((uint8_t)((_r * s) >> 8), (uint8_t)((_g * s) >> 8),
                           (uint8_t)((_b * s) >> 8));
        M5.Led.display();
    }
    // Last colour written (unscaled), for the on-screen indicator.
    uint32_t current() const { return Color(_r, _g, _b); }
private:
    uint8_t _r = 0, _g = 0, _b = 0, _bright = 255;
};

// ------------------------------------------------------------------- init
static inline void cardputerBegin() {
    auto cfg = M5.config();
    cfg.serial_baudrate = 0;                       // upstream sizes + opens Serial itself
    cfg.fallback_board  = m5::board_t::board_M5Cardputer;
    M5Cardputer.begin(cfg, true);
    M5Cardputer.Speaker.setVolume(g_spkVolume);
    // M5GFX >= 0.3 already applies the Cardputer's landscape offset at
    // rotation 0; older releases need setRotation(1). Go by the result.
    if (M5Cardputer.Display.width() < M5Cardputer.Display.height())
        M5Cardputer.Display.setRotation(1);
    M5Cardputer.Display.setBrightness(120);
    M5Cardputer.Display.fillScreen(TFT_BLACK);
}
