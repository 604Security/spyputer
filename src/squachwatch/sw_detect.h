// sw_detect.h — SquachWatch signature engine, adapted for OUI-SPY
// ============================================================================
// The classification order and the deauth / evil-twin detectors follow
// SquachWatch-CYD's src/detection.cpp (GPL-3.0, skizzophrenic/SquachWatch-CYD
// @ d1f03e7). The signature tables themselves are SquachWatch's
// signatures.{h,cpp}, unmodified.
//
// OUI-SPY's own filters run first; this layer catches what they don't and
// reports each hit with a DetectionType and a confidence grade, so a LOW
// match (a generic Espressif block Flock also builds on) can be logged
// without taking over the screen.
//
// swClassifyBle() runs in the NimBLE host task; swWifiFrame() runs in the
// WiFi driver's promiscuous callback. Neither allocates or blocks; shared
// tables sit behind spinlocks.
#pragma once
#include <stdint.h>
#include <stddef.h>

class NimBLEAdvertisedDevice;

extern bool swEnabled;     // master switch (Settings > Built-in signatures)
extern bool swIBeacon;     // iBeacons: off by default (retail floods), as SquachWatch ships

struct SwHit {
    uint8_t mac[6];
    uint8_t type;          // DetectionType
    uint8_t conf;          // Confidence: 0 LOW, 1 MED, 2 HIGH
    char    label[20];     // matched row ("Flock-Liteo", "RayBanMeta", "EvilTwin"...)
    char    name[33];      // advertised name / SSID / pwnagotchi name, if any
    int8_t  rssi;
    uint8_t channel;
    bool    wifi;
};
typedef void (*SwEmit)(const SwHit& h);

// BLE advert -> hit. skipDrone: OUI-SPY's own Remote ID decoder is on.
bool swClassifyBle(NimBLEAdvertisedDevice* adv, bool skipDrone, SwHit& h);

// One promiscuous frame (MGMT or DATA). May emit 0..2 hits.
void swWifiFrame(const uint8_t* frame, int len, int8_t rssi, uint8_t ch, SwEmit emit);

// True if this (mac, type) was emitted in the last 2 s — callers drop it so
// a chatty device can't fill OUI-SPY's 16-slot detection ring.
bool swRateLimited(const uint8_t* mac, uint8_t type);

const char* swShortType(uint8_t type);   // <= 7 chars: FLOCK, SMTAG, EVILTWN...
const char* swConfTag(uint8_t conf);     // LOW / MED / HIGH
int         swOuiConf(const uint8_t* mac);   // SquachWatch OUI grade, -1 if not listed
