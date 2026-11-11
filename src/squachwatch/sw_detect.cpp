// sw_detect.cpp — see sw_detect.h. GPL-3.0 (derived from SquachWatch-CYD).
#include "sw_detect.h"
#include "signatures.h"
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <string.h>

bool swEnabled = true;
bool swIBeacon = false;

static portMUX_TYPE swMux = portMUX_INITIALIZER_UNLOCKED;

static void confInto(SwHit& h, Confidence c) { h.conf = (uint8_t)c; }
static void labelInto(SwHit& h, const char* s) { strlcpy(h.label, s ? s : "", sizeof(h.label)); }

const char* swShortType(uint8_t t) {
    switch ((DetectionType)t) {
        case DetectionType::SAMSUNG_TAG: return "SMTAG";
        case DetectionType::GOOGLE_TAG:  return "GTAG";
        case DetectionType::EVILTWIN:    return "EVILTWN";
        default:                         return detectionTypeName((DetectionType)t);
    }
}

const char* swConfTag(uint8_t c) {
    return c >= (uint8_t)Confidence::HIGH_CONF ? "HIGH" : c == (uint8_t)Confidence::MED_CONF ? "MED" : "LOW";
}

int swOuiConf(const uint8_t* mac) {
    Confidence c;
    if (lookupOui(mac, &c) == DetectionType::UNKNOWN) return -1;
    return (int)c;
}

// ------------------------------------------------------------- rate limit
struct SwRecent { uint8_t mac[6]; uint8_t type; uint32_t at; };
static SwRecent swRecent[32];
static uint8_t  swRecentNext = 0;

bool swRateLimited(const uint8_t* mac, uint8_t type) {
    uint32_t now = millis();
    bool drop = false;
    portENTER_CRITICAL(&swMux);
    int slot = -1;
    for (int i = 0; i < 32; i++) {
        if (swRecent[i].type == type && memcmp(swRecent[i].mac, mac, 6) == 0) { slot = i; break; }
    }
    if (slot >= 0 && now - swRecent[slot].at < 2000) {
        drop = true;
    } else {
        if (slot < 0) { slot = swRecentNext; swRecentNext = (swRecentNext + 1) % 32; }
        memcpy(swRecent[slot].mac, mac, 6);
        swRecent[slot].type = type;
        swRecent[slot].at = now;
    }
    portEXIT_CRITICAL(&swMux);
    return drop;
}

// ------------------------------------------------------------------- BLE
template <typename F>
static bool swForEachAd(const uint8_t* p, uint8_t len, F f) {
    if (!p) return false;
    for (uint8_t i = 0; i + 1 < len; ) {
        const uint8_t l = p[i];
        if (l == 0 || i + 1 + l > len) break;
        if (f(p[i + 1], p + i + 2, (uint8_t)(l - 1))) return true;
        i = (uint8_t)(i + 1 + l);
    }
    return false;
}

// ASTM F3411 over BLE: service data 0xFFFA.
static bool swRemoteIdPresent(const uint8_t* p, uint8_t len) {
    return swForEachAd(p, len, [](uint8_t type, const uint8_t* d, uint8_t n) {
        return type == 0x16 && n >= 3 && d[0] == 0xFA && d[1] == 0xFF;
    });
}

bool swClassifyBle(NimBLEAdvertisedDevice* adv, bool skipDrone, SwHit& h) {
    if (!swEnabled || !adv) return false;
    memset(&h, 0, sizeof(h));
    const uint8_t* le = adv->getAddress().getNative();
    for (int k = 0; k < 6; k++) h.mac[k] = le[5 - k];
    h.rssi = (int8_t)adv->getRSSI();
    const std::string advName = adv->getName();     // held: getName() returns a temporary
    strlcpy(h.name, advName.c_str(), sizeof(h.name));

    const uint8_t* raw = adv->getPayload();
    const uint8_t rawLen = (uint8_t)adv->getPayloadLength();
    DetectionType t = DetectionType::UNKNOWN;
    const char* label = nullptr;

    // Manufacturer data. Apple's company ID alone is every Apple device:
    // confirm a Find My tag, else maybe an iBeacon, else nothing.
    if (adv->haveManufacturerData()) {
        std::string mfg = adv->getManufacturerData();
        if (mfg.size() >= 2) {
            uint16_t mfgId = (uint8_t)mfg[0] | ((uint8_t)mfg[1] << 8);
            t = lookupMfgId(mfgId);
            label = mfgIdName(mfgId);
            if (t == DetectionType::AIRTAG && !isAirTagPayload(raw, rawLen)) {
                t = DetectionType::UNKNOWN;
                label = nullptr;
                const uint8_t* b = (const uint8_t*)mfg.data();
                if (swIBeacon && isIBeacon(b, (uint8_t)mfg.size())) {
                    t = DetectionType::IBEACON;
                    label = "iBeacon";
                    snprintf(h.name, sizeof(h.name), "%02X%02X%02X %u.%u", b[4], b[5], b[6],
                             (unsigned)((b[20] << 8) | b[21]), (unsigned)((b[22] << 8) | b[23]));
                }
            }
        }
    }
    if (t == DetectionType::UNKNOWN && isAirTagPayload(raw, rawLen)) { t = DetectionType::AIRTAG; label = "Apple"; }
    if (t == DetectionType::UNKNOWN && !skipDrone && swRemoteIdPresent(raw, rawLen)) { t = DetectionType::DRONE; label = "DroneID"; }
    if (t == DetectionType::UNKNOWN && isAxonBwcPayload(raw, rawLen)) { t = DetectionType::AXON; label = "Axon-BWC"; }
    if (t == DetectionType::UNKNOWN && isFmdnPayload(raw, rawLen))    { t = DetectionType::GOOGLE_TAG; label = "FindHub"; }
    if (t == DetectionType::UNKNOWN) {
        if (const uint16_t u = serviceDataUuid(raw, rawLen)) { t = lookupUuid(u); label = uuidName(u); }
    }
    if (t == DetectionType::UNKNOWN && adv->haveServiceUUID()) {
        static const NimBLEUUID kFlockGatt("e8ccbb38-9532-46a8-9fe5-1814df172e6f");
        for (int j = 0; j < (int)adv->getServiceUUIDCount() && t == DetectionType::UNKNOWN; j++) {
            NimBLEUUID u = adv->getServiceUUID(j);
            if (u.bitSize() == 16) {
                for (uint16_t i = 0; i < kUuidCount; i++) {
                    const uint16_t k = kUuidTable[i].uuid;
                    if (k == 0xFEAA) continue;                        // any Eddystone: Find Hub checked above
                    if (k == 0xFFFA && skipDrone) continue;
                    if (u.equals(NimBLEUUID((uint16_t)k))) { t = kUuidTable[i].type; label = kUuidTable[i].name; break; }
                }
            } else if (u.bitSize() == 128 && u.equals(kFlockGatt)) {
                t = DetectionType::FLOCK;
                label = "Flock-GATT";
            }
        }
    }
    bool byName = false;
    if (t == DetectionType::UNKNOWN && h.name[0]) {
        t = lookupBtName(h.name);
        byName = (t != DetectionType::UNKNOWN);
        label = byName ? "name" : nullptr;
    }
    if (t == DetectionType::UNKNOWN) return false;
    if (t == DetectionType::IBEACON && !swIBeacon) return false;

    Confidence c = confidenceFor(t);
    if (t == DetectionType::HACKER) c = byName ? Confidence::MED_CONF : Confidence::HIGH_CONF;
    if (t == DetectionType::FLOCK && byName && isBareSerialName(h.name)) c = Confidence::LOW_CONF;
    h.type = (uint8_t)t;
    confInto(h, c);
    labelInto(h, label ? label : detectionTypeName(t));
    h.wifi = false;
    return true;
}

// ------------------------------------------------------------------ WiFi
// Evil twin: one SSID from a second BSSID of another vendor that disagrees
// about encryption (SquachWatch DetectionEngine::noteApBeacon).
struct SwAp { char ssid[33]; uint8_t bssid[6]; bool enc; };
static SwAp    swAps[24];
static uint8_t swApCount = 0, swApNext = 0;

static bool swSameVendor(const uint8_t* a, const uint8_t* b) {
    return ((a[0] & ~0x02) == (b[0] & ~0x02)) && a[1] == b[1] && a[2] == b[2];
}

static bool swNoteAp(const uint8_t* bssid, const char* ssid, bool enc) {
    bool twin = false, found = false;
    portENTER_CRITICAL(&swMux);
    for (uint8_t i = 0; i < swApCount; i++) {
        if (strncmp(swAps[i].ssid, ssid, 32) != 0) continue;
        found = true;
        if (memcmp(swAps[i].bssid, bssid, 6) == 0) { swAps[i].enc = enc; break; }
        if (swSameVendor(swAps[i].bssid, bssid)) break;
        if (swAps[i].enc == enc) break;
        twin = true;
        break;
    }
    if (!found) {
        SwAp& s = (swApCount < 24) ? swAps[swApCount++] : swAps[swApNext];
        if (swApCount >= 24) swApNext = (swApNext + 1) % 24;
        strlcpy(s.ssid, ssid, sizeof(s.ssid));
        memcpy(s.bssid, bssid, 6);
        s.enc = enc;
    }
    portEXIT_CRITICAL(&swMux);
    return twin;
}

// Deauth / disassoc flood: 12 from one sender in 3 s, or 6 to broadcast.
struct SwDeauth { uint8_t mac[6]; uint32_t winStart; uint8_t count, bcast; };
static SwDeauth swDe[8];
static uint8_t  swDeNext = 0;
static uint32_t swDeLastFire = 0;

static bool swNoteDeauth(const uint8_t* mac, bool bcast) {
    uint32_t now = millis();
    bool fire = false;
    portENTER_CRITICAL(&swMux);
    SwDeauth* s = nullptr;
    for (int i = 0; i < 8; i++) if (memcmp(swDe[i].mac, mac, 6) == 0) { s = &swDe[i]; break; }
    if (!s) {
        s = &swDe[swDeNext];
        swDeNext = (swDeNext + 1) % 8;
        memcpy(s->mac, mac, 6);
        s->winStart = now; s->count = 0; s->bcast = 0;
    }
    if (now - s->winStart > 3000) { s->winStart = now; s->count = 0; s->bcast = 0; }
    if (s->count < 255) s->count++;
    if (bcast && s->bcast < 255) s->bcast++;
    if ((s->count >= 12 || s->bcast >= 6) && now - swDeLastFire > 15000) {
        swDeLastFire = now;
        fire = true;
    }
    portEXIT_CRITICAL(&swMux);
    return fire;
}

static void swEmitOui(const uint8_t* mac, int8_t rssi, uint8_t ch, SwEmit emit, const char* ssid) {
    Confidence c;
    DetectionType t = lookupOui(mac, &c);
    const char* label = nullptr;
    if (t != DetectionType::UNKNOWN) {
        for (uint16_t i = 0; i < kOuiCount; i++)
            if (!memcmp(kOuiTable[i].b, mac, 3)) { label = kOuiTable[i].name; break; }
    } else if (ssid && ssid[0]) {
        t = lookupSsid(ssid);
        if (t == DetectionType::UNKNOWN) return;
        c = confidenceFor(t);
        label = ssidVendorName(ssid);
    } else {
        return;
    }
    SwHit h;
    memset(&h, 0, sizeof(h));
    memcpy(h.mac, mac, 6);
    h.type = (uint8_t)t;
    confInto(h, c);
    labelInto(h, label ? label : detectionTypeName(t));
    if (ssid) strlcpy(h.name, ssid, sizeof(h.name));
    h.rssi = rssi; h.channel = ch; h.wifi = true;
    emit(h);
}

void swWifiFrame(const uint8_t* frame, int len, int8_t rssi, uint8_t ch, SwEmit emit) {
    if (!swEnabled || !frame || len < 24) return;
    const uint8_t fc0 = frame[0];
    const uint8_t type = (fc0 & 0x0C) >> 2, subtype = (fc0 & 0xF0) >> 4;

    if (type == 0 && subtype == 4) {                       // probe request: transmitter
        swEmitOui(frame + 10, rssi, ch, emit, nullptr);
    } else if (type == 2) {                                // data: receiver + transmitter
        swEmitOui(frame + 4,  rssi, ch, emit, nullptr);
        swEmitOui(frame + 10, rssi, ch, emit, nullptr);
    } else if (type == 0 && subtype == 8 && len > 36) {    // beacon
        const uint8_t* ies = frame + 36;
        int ieLen = len - 36;
        char ssid[33] = {0};
        if (ieLen >= 2 && ies[0] == 0x00) {
            uint8_t n = ies[1] > 32 ? 32 : ies[1];
            if (2 + n <= ieLen) { memcpy(ssid, ies + 2, n); ssid[n] = 0; }
        }
        const bool enc = (frame[34] & 0x10) != 0;
        char pwn[33];
        if (pwnagotchiName(frame, (uint32_t)(len > 512 ? 512 : len), pwn, sizeof(pwn))) {
            SwHit h;
            memset(&h, 0, sizeof(h));
            memcpy(h.mac, frame + 10, 6);
            h.type = (uint8_t)DetectionType::HACKER;
            confInto(h, Confidence::HIGH_CONF);
            labelInto(h, "Pwnagotchi");
            strlcpy(h.name, pwn, sizeof(h.name));
            h.rssi = rssi; h.channel = ch; h.wifi = true;
            emit(h);
        } else if (ssid[0] && swNoteAp(frame + 16, ssid, enc)) {
            SwHit h;
            memset(&h, 0, sizeof(h));
            memcpy(h.mac, frame + 16, 6);
            h.type = (uint8_t)DetectionType::EVILTWIN;
            confInto(h, confidenceFor(DetectionType::EVILTWIN));
            labelInto(h, "EvilTwin");
            strlcpy(h.name, ssid, sizeof(h.name));
            h.rssi = rssi; h.channel = ch; h.wifi = true;
            emit(h);
        } else {
            swEmitOui(frame + 16, rssi, ch, emit, ssid);
        }
    } else if (type == 0 && (subtype == 12 || subtype == 10)) {   // deauth / disassoc
        bool bcast = true;
        for (int i = 4; i < 10; i++) if (frame[i] != 0xFF) { bcast = false; break; }
        if (swNoteDeauth(frame + 10, bcast)) {
            SwHit h;
            memset(&h, 0, sizeof(h));
            memcpy(h.mac, frame + 10, 6);
            h.type = (uint8_t)DetectionType::DEAUTH;
            confInto(h, confidenceFor(DetectionType::DEAUTH));
            labelInto(h, bcast ? "Deauth-bcast" : "Deauth");
            h.rssi = rssi; h.channel = ch; h.wifi = true;
            emit(h);
        }
    }
}
