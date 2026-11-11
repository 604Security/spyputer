// ============================================================================
// cardputer_ui.h — on-device menu, display and keyboard for the Cardputer
// ============================================================================
// The Cardputer runs OUI-SPY from its own screen and keyboard: it boots into
// a menu (no WiFi AP), filters are typed in or picked from the vendor
// database, and scanning starts from the menu. The upstream web portal is
// still there as a menu item for MQTT / fleet settings.
//
// Included by main.cpp right before setup(), after every global it uses, so
// it shares main.cpp's translation unit and statics. Everything runs from
// loop() via cardTick(); never from the BLE/WiFi callbacks. The screen is
// drawn into an 8-bit sprite (32 KB — the Cardputer has no PSRAM).
//
// Keys:  ; .  up/down     , /  left/right     ENTER select     ` or DEL back
#pragma once
#include "vendor_db.h"

// SpyPuter logo (pyfiglet "small_slant"), shared by the splash and the serial banner.
static const char* const SPYPUTER_ART[] = {
    "   ____          ___       __         ",
    "  / __/__  __ __/ _ \\__ __/ /____ ____",
    " _\\ \\/ _ \\/ // / ___/ // / __/ -_) __/",
    "/___/ .__/\\_, /_/   \\_,_/\\__/\\__/_/   ",
    "   /_/   /___/",
};
static const int SPYPUTER_ART_LINES = sizeof(SPYPUTER_ART) / sizeof(SPYPUTER_ART[0]);

static M5Canvas cardCanvas(&M5Cardputer.Display);

// Crash breadcrumbs: survive a panic / watchdog reset (not power loss) and
// are printed at the next boot.
RTC_NOINIT_ATTR static uint32_t cardCrumbMagic;
RTC_NOINIT_ATTR static uint8_t  cardCrumbView, cardCrumbKey, cardCrumbStage;
RTC_NOINIT_ATTR static uint32_t cardCrumbAt;
static inline void cardCrumb(uint8_t stage) { cardCrumbMagic = 0xC0FFEE42; cardCrumbStage = stage; cardCrumbAt = millis(); }
static bool cardCanvasOk = false;

enum CardView : uint8_t {
    CV_MENU, CV_FILTERS, CV_ADD, CV_VENDORS, CV_SETTINGS, CV_HELP,
    CV_PORTAL, CV_SCAN, CV_DETAIL
};
static CardView cardView = CV_MENU;
static bool     cardPortalUp = false;     // upstream web portal (AP) running
static bool     cardStealth = false;
static uint8_t  cardBrightIdx = 2;
static const uint8_t CARD_BRIGHT[] = {20, 60, 120, 200, 255};
static unsigned long cardConfirmAt = 0;   // two-press confirmations
static int      cardConfirmWhat = -1;
static unsigned long cardToastUntil = 0;
static char          cardToast[44] = "";
static bool          cardLocked = false;   // upstream burn-in lock (boots to scan, no menu)
static bool          cardAutoScan = true;  // scan on boot when there are filters
static int  cardSel[CV_DETAIL + 1] = {0};  // cursor per view
static uint8_t cardAlertMin = 1;           // 0 all, 1 MED+, 2 HIGH only (SquachWatch ALERT FILTER)

// ------------------------------------------------------------- ignore list
// Devices that never take over the screen again (still listed). Persisted.
static const int CARD_IGN_MAX = 32;
static uint8_t cardIgn[CARD_IGN_MAX][6];
static int     cardIgnCount = 0;

static bool cardParseMac(const char* s, uint8_t out[6]) {
    unsigned v[6];
    if (sscanf(s, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) return false;
    for (int i = 0; i < 6; i++) out[i] = (uint8_t)v[i];
    return true;
}
static int cardIgnIndex(const char* mac) {
    uint8_t m[6];
    if (!cardParseMac(mac, m)) return -1;
    for (int i = 0; i < cardIgnCount; i++) if (!memcmp(cardIgn[i], m, 6)) return i;
    return -1;
}
static bool cardIgnored(const char* mac) { return cardIgnIndex(mac) >= 0; }
static void cardIgnSave() {
    preferences.begin("ouispy", false);
    preferences.putBytes("cp_ign", cardIgn, cardIgnCount * 6);
    preferences.end();
}
static bool cardIgnToggle(const char* mac) {    // returns true if now ignored
    int i = cardIgnIndex(mac);
    if (i >= 0) {
        memmove(cardIgn[i], cardIgn[i + 1], (cardIgnCount - i - 1) * 6);
        cardIgnCount--;
        cardIgnSave();
        return false;
    }
    uint8_t m[6];
    if (!cardParseMac(mac, m)) return false;
    if (cardIgnCount == CARD_IGN_MAX) { memmove(cardIgn[0], cardIgn[1], (CARD_IGN_MAX - 1) * 6); cardIgnCount--; }
    memcpy(cardIgn[cardIgnCount++], m, 6);
    cardIgnSave();
    return true;
}

// Confidence of a detection: SquachWatch hits carry it in the description
// ("[LOW]"); OUI-type filter hits inherit SquachWatch's grade for that OUI
// (a filter on a generic Espressif block is still LOW); anything else is a
// deliberate filter, so HIGH.
static uint8_t cardDetConf(FilterType t, const char* mac, const String& desc) {
    if (desc.indexOf("[LOW]") >= 0)  return 0;
    if (desc.indexOf("[MED]") >= 0)  return 1;
    if (desc.indexOf("[HIGH]") >= 0) return 2;
    if (t == FT_MAC_PREFIX || t == FT_WIFI_PROBE || t == FT_WIFI_BEACON) {
        uint8_t m[6];
        if (cardParseMac(mac, m)) { int c = swOuiConf(m); if (c >= 0) return (uint8_t)c; }
    }
    return 2;
}
static const char* CARD_CONF_NAME[] = {"LOW", "MED", "HIGH"};

static const uint16_t CARD_PINK   = 0xF81F;
static const uint16_t CARD_PURPLE = 0x801F;
static const uint16_t CARD_DIM    = 0x8410;
static const uint16_t CARD_SELBG  = 0x4A69;   // survives the 8-bit (rgb332) sprite

static void cardToastf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cardToast, sizeof(cardToast), fmt, ap);
    va_end(ap);
    cardToastUntil = millis() + 1800;
}

// Two presses of the same action within 3 s.
static bool cardConfirm(int what, const char* prompt) {
    if (cardConfirmWhat == what && millis() - cardConfirmAt < 3000) {
        cardConfirmWhat = -1;
        return true;
    }
    cardConfirmWhat = what;
    cardConfirmAt = millis();
    cardToastf("%s", prompt);
    return false;
}

static void cardSavePref(const char* key, bool v) {
    preferences.begin("ouispy", false);
    preferences.putBool(key, v);
    preferences.end();
}

static void cardFiltersChanged() {
    saveConfiguration();
    rebuildWifiOuiTable();
}

// ================================================================ detections
struct CardDet {
    char mac[18];
    char label[40];     // alias, else filter description
    char type[8];       // OUI / PROBE / ...
    char node[7];       // mesh node that saw it ("" = this one)
    int  rssi;
    bool wifi;
    unsigned long at;
    uint16_t hits;
    uint8_t conf;       // 0 LOW, 1 MED, 2 HIGH
};
static const int CARD_DET_MAX = 24;
static CardDet  cardDets[CARD_DET_MAX];
static int      cardDetCount = 0;       // newest at index 0
static uint32_t cardTotalAlerts = 0;
static unsigned long cardAlertUntil = 0;

// From loop() for every detection that is not a pure signal update.
static bool cardDemoFreeze = false;   // carddemo: sample rows only (screenshots)
static void cardOnDetection(const String& mac, const String& alias, const String& desc,
                            int rssi, const char* type, bool wifi, const char* node, bool alert,
                            uint8_t conf, bool demo = false) {
    if (cardDemoFreeze && !demo) return;
    int found = -1;
    for (int i = 0; i < cardDetCount; i++) {
        if (strcasecmp(cardDets[i].mac, mac.c_str()) == 0) { found = i; break; }   // one row per device
    }
    CardDet d;
    if (found >= 0) {
        d = cardDets[found];
        memmove(&cardDets[1], &cardDets[0], sizeof(CardDet) * found);
        d.hits++;
    } else {
        memset(&d, 0, sizeof(d));
        if (cardDetCount < CARD_DET_MAX) cardDetCount++;
        memmove(&cardDets[1], &cardDets[0], sizeof(CardDet) * (cardDetCount - 1));
        d.hits = 1;
    }
    // Same device seen again by another filter / signature: keep the row's
    // existing label and type unless the new match is more confident.
    bool relabel = found < 0 || conf > d.conf;
    strlcpy(d.mac, mac.c_str(), sizeof(d.mac));
    if (relabel) {
    String lbl = alias.length() ? alias : desc;
    int tag = lbl.lastIndexOf(" [mesh ");
    if (tag > 0) lbl = lbl.substring(0, tag);
    int ct = lbl.lastIndexOf(" [");
    if (ct > 0 && lbl.endsWith("]")) lbl = lbl.substring(0, ct);
    strlcpy(d.label, lbl.length() ? lbl.c_str() : "(unnamed)", sizeof(d.label));
    strlcpy(d.type, type, sizeof(d.type));
    d.conf = conf;
    }
    strlcpy(d.node, node ? node : "", sizeof(d.node));
    d.rssi = rssi;
    d.wifi = wifi;
    d.at = millis();
    cardDets[0] = d;
    int& sel = cardSel[CV_SCAN];
    if (sel && found != 0) sel = min(sel + 1, cardDetCount - 1);
    if (alert) {
        cardTotalAlerts++;
        if (!cardStealth) cardAlertUntil = millis() + 2500;
    }
}

// ================================================================ text entry
struct CardField {
    char buf[40];
    uint8_t len;
    void clear() { buf[0] = 0; len = 0; }
    void set(const char* s) { strlcpy(buf, s, sizeof(buf)); len = strlen(buf); }
    void put(char c) { if (len < sizeof(buf) - 1) { buf[len++] = c; buf[len] = 0; } }
    void back() { if (len) buf[--len] = 0; }
};

// Add-filter form. Types the user can pick, in order.
struct CardAddType { const char* code; const char* hint; uint8_t idLen; };   // idLen 0 = free text
static const CardAddType CARD_ADD_TYPES[] = {
    { "OUI",    "BLE+WiFi OUI  e.g. 00:25:DF", 6 },
    { "OUI",    "BLE OUI       e.g. 00:25:DF", 6 },
    { "MAC",    "full BLE MAC  AA:BB:CC:DD:EE:FF", 12 },
    { "CID",    "BLE company ID  e.g. 0D53", 4 },
    { "SVC",    "BLE 16-bit service UUID  FD5F", 4 },
    { "NAME",   "BLE name contains...", 0 },
    { "PROBE",  "WiFi probe request OUI", 6 },
    { "BEACON", "WiFi beacon OUI", 6 },
};
static const int CARD_ADD_TYPE_COUNT = sizeof(CARD_ADD_TYPES) / sizeof(CARD_ADD_TYPES[0]);
static int       cardAddType = 0;
static int       cardAddField = 0;     // 0 type, 1 id, 2 description
static CardField cardAddId, cardAddDesc;

static String cardHexOnly(const char* in) {
    String o;
    for (const char* p = in; *p; p++) {
        char c = toupper(*p);
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F')) o += c;
        else if (c == ':' || c == '-' || c == ' ') continue;
        else if (c == 'X' && o == "0") o = "";      // 0x prefix
        else return "";                              // not hex at all
    }
    return o;
}

static bool cardHasFilter(FilterType t, const String& id) {
    for (auto& f : targetFilters) if (f.type == t && f.identifier.equalsIgnoreCase(id)) return true;
    return false;
}

static void cardPushFilter(FilterType t, const String& id, const String& desc) {
    if (cardHasFilter(t, id)) return;
    TargetFilter f;
    f.type = t;
    f.identifier = id;
    f.isFullMAC = (t == FT_FULL_MAC);
    f.description = desc;
    targetFilters.push_back(f);
}

static String cardColons(const String& hex) {
    String o;
    for (unsigned i = 0; i < hex.length(); i += 2) { if (i) o += ':'; o += hex.substring(i, i + 2); }
    return o;
}

// Validates the form and installs the filter(s). Returns true on success.
static bool cardAddCommit() {
    const CardAddType& t = CARD_ADD_TYPES[cardAddType];
    String id;
    if (t.idLen) {
        id = cardHexOnly(cardAddId.buf);
        if ((int)id.length() != t.idLen) {
            cardToastf("%s needs %d hex digits", t.code, t.idLen);
            cardAddField = 1;
            return false;
        }
    } else {
        id = cardAddId.buf;
        id.trim();
        if (!id.length()) { cardToastf("type a name to match"); cardAddField = 1; return false; }
    }
    if ((int)targetFilters.size() >= FILTER_MAX) { cardToastf("filter list full (%d)", FILTER_MAX); return false; }
    String desc = cardAddDesc.buf;
    desc.trim();
    String shown = t.idLen == 6 || t.idLen == 12 ? cardColons(id) : id;
    size_t before = targetFilters.size();
    switch (cardAddType) {
        case 0:
            cardPushFilter(FT_MAC_PREFIX, id, desc.length() ? desc : "OUI: " + shown);
            cardPushFilter(FT_WIFI_PROBE, id, desc.length() ? desc : "OUI: " + shown + " (WiFi)");
            break;
        case 1: cardPushFilter(FT_MAC_PREFIX, id, desc.length() ? desc : "OUI: " + shown); break;
        case 2: cardPushFilter(FT_FULL_MAC, id, desc.length() ? desc : "MAC: " + shown); break;
        case 3: cardPushFilter(FT_COMPANY_ID, id, desc.length() ? desc : "CID 0x" + id); break;
        case 4: cardPushFilter(FT_SERVICE_UUID_16, id, desc.length() ? desc : "SVC 0x" + id); break;
        case 5: cardPushFilter(FT_NAME_SUBSTRING, id, desc.length() ? desc : "Name: " + id); break;
        case 6: cardPushFilter(FT_WIFI_PROBE, id, desc.length() ? desc : "OUI: " + shown + " (WiFi)"); break;
        case 7: cardPushFilter(FT_WIFI_BEACON, id, desc.length() ? desc : "OUI: " + shown + " (beacon)"); break;
    }
    if (targetFilters.size() == before) { cardToastf("already in the list"); return false; }
    cardFiltersChanged();
    cardToastf("added %s %s", t.code, shown.c_str());
    return true;
}

// ============================================================ vendor picker
// Signature packs (upstream presets: OUIs + company IDs + service UUIDs +
// WiFi frames) first, then the OUI Database cards (vendor_db.h).
struct CardPack { const char* name; const PresetEntry* table; const size_t* count; const char* category; };
static const CardPack CARD_PACKS[] = {
    { "META / RAY-BAN (full)", PRESET_META,    &PRESET_META_COUNT,    "Smart glasses: CID+UUID composite, names, OUIs" },
    { "AXON (full)",           PRESET_AXON,    &PRESET_AXON_COUNT,    "Body cams: OUI, CID, UUID, WiFi probe+beacon" },
    { "I-PRO",                 PRESET_IPRO,    &PRESET_IPRO_COUNT,    "Body cams: BLE OUI + WiFi probe" },
    { "VERKADA",               PRESET_VERKADA, &PRESET_VERKADA_COUNT, "Security cameras: WiFi probe + beacon" },
};
static const int CARD_PACK_COUNT = sizeof(CARD_PACKS) / sizeof(CARD_PACKS[0]);
static int cardVendorTotal() { return CARD_PACK_COUNT + CARD_VENDOR_COUNT; }

static int cardVendorInstalled(int i, int& total) {
    int have = 0;
    if (i < CARD_PACK_COUNT) {
        const CardPack& p = CARD_PACKS[i];
        total = (int)*p.count;
        for (size_t k = 0; k < *p.count; k++) have += cardHasFilter(p.table[k].type, p.table[k].identifier);
        return have;
    }
    const CardVendor& v = CARD_VENDORS[i - CARD_PACK_COUNT];
    total = v.count * ((v.ble ? 1 : 0) + (v.wifi ? 1 : 0));
    for (int k = 0; k < v.count; k++) {
        String o = String(v.ouis + k * 6).substring(0, 6);
        if (v.ble)  have += cardHasFilter(FT_MAC_PREFIX, o);
        if (v.wifi) have += cardHasFilter(FT_WIFI_PROBE, o);
    }
    return have;
}

static void cardVendorToggle(int i) {
    int total, have = cardVendorInstalled(i, total);
    if (i < CARD_PACK_COUNT) {
        const CardPack& p = CARD_PACKS[i];
        if (have) { int n = removePreset(p.table, *p.count); rebuildWifiOuiTable(); cardToastf("removed %d", n); }
        else      { int n = applyPreset(p.table, *p.count, p.name); cardToastf("added %d signatures", n); }
        return;
    }
    const CardVendor& v = CARD_VENDORS[i - CARD_PACK_COUNT];
    int n = 0;
    for (int k = 0; k < v.count; k++) {
        String o = String(v.ouis + k * 6).substring(0, 6);
        for (int d = 0; d < 2; d++) {
            if ((d == 0 && !v.ble) || (d == 1 && !v.wifi)) continue;
            FilterType t = d == 0 ? FT_MAC_PREFIX : FT_WIFI_PROBE;
            if (have) {
                for (size_t j = 0; j < targetFilters.size(); ) {
                    if (targetFilters[j].type == t && targetFilters[j].identifier.equalsIgnoreCase(o)) {
                        targetFilters.erase(targetFilters.begin() + j); n++;
                    } else j++;
                }
            } else if (!cardHasFilter(t, o) && (int)targetFilters.size() < FILTER_MAX) {
                cardPushFilter(t, o, String(v.name) + (d ? " (WiFi)" : ""));
                n++;
            }
        }
    }
    cardFiltersChanged();
    cardToastf(have ? "removed %d" : "added %d filters", n);
}

// Every signature pack + every OUI Database vendor. Returns filters added.
static int cardInstallAll() {
    size_t before = targetFilters.size();
    for (int i = 0; i < CARD_PACK_COUNT; i++) {
        const CardPack& p = CARD_PACKS[i];
        for (size_t k = 0; k < *p.count && (int)targetFilters.size() < FILTER_MAX; k++)
            cardPushFilter(p.table[k].type, p.table[k].identifier, String(p.name) + ": " + p.table[k].description);
    }
    for (int i = 0; i < CARD_VENDOR_COUNT; i++) {
        const CardVendor& v = CARD_VENDORS[i];
        for (int k = 0; k < v.count; k++) {
            String o = String(v.ouis + k * 6).substring(0, 6);
            if (v.ble  && (int)targetFilters.size() < FILTER_MAX) cardPushFilter(FT_MAC_PREFIX, o, v.name);
            if (v.wifi && (int)targetFilters.size() < FILTER_MAX) cardPushFilter(FT_WIFI_PROBE, o, String(v.name) + " (WiFi)");
        }
    }
    cardFiltersChanged();
    return (int)(targetFilters.size() - before);
}

// ================================================================= settings
enum CardSetting : uint8_t {
    ST_SOUND, ST_VOLUME, ST_LED, ST_BRIGHT, ST_SIGS, ST_ALERTLVL, ST_IBEACON, ST_CLEAR_IGN, ST_RID, ST_MESH, ST_MESH_ROLE, ST_MESH_CH,
    ST_AUTOSCAN, ST_CLEAR_HIST, ST_FACTORY, ST_COUNT
};

static void cardSettingLabel(int i, char* name, size_t nn, char* val, size_t vn) {
    switch (i) {
        case ST_SOUND:     strlcpy(name, "Sound", nn);           strlcpy(val, buzzerEnabled ? "on" : "muted", vn); break;
        case ST_VOLUME:    strlcpy(name, "Volume", nn);          snprintf(val, vn, "%d%%", g_spkVolume * 100 / 255); break;
        case ST_LED:       strlcpy(name, "RGB LED", nn);         strlcpy(val, ledEnabled ? "on" : "off", vn); break;
        case ST_BRIGHT:    strlcpy(name, "Screen brightness", nn); snprintf(val, vn, "%d/5", cardBrightIdx + 1); break;
        case ST_SIGS:      strlcpy(name, "SquachWatch signatures", nn); strlcpy(val, swEnabled ? "on" : "off", vn); break;
        case ST_ALERTLVL:  strlcpy(name, "Alert on", nn); strlcpy(val, cardAlertMin == 0 ? "all" : cardAlertMin == 1 ? "MED+" : "HIGH only", vn); break;
        case ST_IBEACON:   strlcpy(name, "iBeacons", nn);        strlcpy(val, swIBeacon ? "on" : "off", vn); break;
        case ST_CLEAR_IGN: strlcpy(name, "Clear ignored devices", nn); snprintf(val, vn, "%d", cardIgnCount); break;
        case ST_RID:       strlcpy(name, "Drone Remote ID", nn); strlcpy(val, ridEnabled ? "on" : "off", vn); break;
        case ST_MESH:      strlcpy(name, "Mesh (ESP-NOW LR)", nn); strlcpy(val, meshCfg.enabled ? "on" : "off", vn); break;
        case ST_MESH_ROLE: strlcpy(name, "Mesh role", nn);       strlcpy(val, meshCfg.home ? "home" : "remote", vn); break;
        case ST_MESH_CH:   strlcpy(name, "Mesh channel", nn);    snprintf(val, vn, "%d", meshCfg.channel); break;
        case ST_AUTOSCAN:  strlcpy(name, "Scan on boot", nn);    strlcpy(val, cardAutoScan ? "on" : "off", vn); break;
        case ST_CLEAR_HIST:strlcpy(name, "Clear detection history", nn); strlcpy(val, "", vn); break;
        case ST_FACTORY:   strlcpy(name, "Factory reset", nn);   strlcpy(val, "", vn); break;
    }
}

// dir: 0 = ENTER, -1/+1 = left/right
static void cardSettingChange(int i, int dir) {
    switch (i) {
        case ST_SOUND:
            buzzerEnabled = !buzzerEnabled;
            if (!buzzerEnabled) buzzWrite(0);
            cardSavePref("buzzerEnabled", buzzerEnabled);
            break;
        case ST_VOLUME: {
            int v = g_spkVolume + (dir < 0 ? -32 : 32);
            if (dir == 0 && g_spkVolume >= 255) v = 32;
            g_spkVolume = (uint8_t)constrain(v, 0, 255);
            M5Cardputer.Speaker.setVolume(g_spkVolume);
            preferences.begin("ouispy", false); preferences.putUChar("cp_vol", g_spkVolume); preferences.end();
            if (buzzerEnabled) M5Cardputer.Speaker.tone(BUZZER_FREQ, 60);
            break;
        }
        case ST_LED:
            ledEnabled = !ledEnabled;
            cardSavePref("ledEnabled", ledEnabled);
            break;
        case ST_BRIGHT:
            cardBrightIdx = (cardBrightIdx + (dir < 0 ? sizeof(CARD_BRIGHT) - 1 : 1)) % sizeof(CARD_BRIGHT);
            M5Cardputer.Display.setBrightness(CARD_BRIGHT[cardBrightIdx]);
            preferences.begin("ouispy", false); preferences.putUChar("cp_bright", cardBrightIdx); preferences.end();
            break;
        case ST_SIGS:
            swEnabled = !swEnabled;
            cardSavePref("cp_sw", swEnabled);
            break;
        case ST_ALERTLVL:
            cardAlertMin = (uint8_t)((cardAlertMin + (dir < 0 ? 2 : 1)) % 3);
            preferences.begin("ouispy", false); preferences.putUChar("cp_alertmin", cardAlertMin); preferences.end();
            break;
        case ST_IBEACON:
            swIBeacon = !swIBeacon;
            cardSavePref("cp_ibeacon", swIBeacon);
            break;
        case ST_CLEAR_IGN:
            cardIgnCount = 0;
            cardIgnSave();
            cardToastf("ignore list cleared");
            break;
        case ST_RID:
#ifdef SKYSPY_NODE
            cardToastf("always on in a Sky-Spy build");
#else
            ridEnabled = !ridEnabled;
            cardSavePref("rid", ridEnabled);
#endif
            break;
        case ST_MESH:      meshCfg.enabled = !meshCfg.enabled; mesh_saveConfig(); break;
        case ST_MESH_ROLE: meshCfg.home = !meshCfg.home;       mesh_saveConfig(); break;
        case ST_MESH_CH:
            meshCfg.channel = (uint8_t)((meshCfg.channel - 1 + (dir < 0 ? 10 : 1)) % 11 + 1);
            mesh_saveConfig();
            break;
        case ST_AUTOSCAN:
            cardAutoScan = !cardAutoScan;
            cardSavePref("cp_autoscan", cardAutoScan);
            cardToastf(cardAutoScan ? "scans on power-up" : "opens this menu on power-up");
            break;
        case ST_CLEAR_HIST:
            if (cardConfirm(100 + i, "ENTER again to clear history")) {
                clearDetectedDevices();
                cardDetCount = 0;
                cardToastf("history cleared");
            }
            break;
        case ST_FACTORY:
            if (cardConfirm(100 + i, "ENTER again: ERASE everything")) {
                preferences.begin("ouispy", false);
                preferences.putBool("factoryReset", true);
                preferences.end();
                cardToastf("factory reset, rebooting...");
                delay(400);
                ESP.restart();
            }
            break;
    }
}

// ================================================================== actions
static void cardStartScan() {
    if (targetFilters.empty() && !meshCfg.enabled && !ridEnabled && !swEnabled) {
        cardToastf("no filters: add some first");
        return;
    }
    cardView = CV_SCAN;
    cardSel[CV_SCAN] = 0;
    cardToastf("starting scan...");
    saveConfiguration();
    rebuildWifiOuiTable();
    if (cardPortalUp) {
        modeSwitchScheduled = millis();          // upstream path: tears the portal down
    } else {
        startScanningMode();
    }
}

// Scanning / portal -> back to the menu. The radio stacks are not built to
// be torn down and rebuilt, so this is a reboot (the lock is cleared).
static void cardBackToMenu() {
    preferences.begin("ouispy", false);
    preferences.remove("configLocked");
    preferences.putBool("cp_menu", true);    // one-shot: next boot opens the menu
    preferences.end();
    cardToastf("back to menu...");
    if (meshCfg.enabled) mesh_deinit();
    delay(300);
    ESP.restart();
}

static void cardStartPortal() {
    cardPortalUp = true;
    cardView = CV_PORTAL;
    startConfigMode();
}

// =================================================================== drawing
static uint16_t cardRssiColor(int rssi) {
    if (rssi >= -60) return TFT_RED;
    if (rssi >= -75) return TFT_ORANGE;
    return TFT_YELLOW;
}

static void cardAgo(char* out, size_t n, unsigned long at) {
    unsigned long s = (millis() - at) / 1000;
    if (s < 60)        snprintf(out, n, "%lus", s);
    else if (s < 3600) snprintf(out, n, "%lum", s / 60);
    else               snprintf(out, n, "%luh", s / 3600);
}

static void cardFilterCounts(int& ble, int& wifi) {
    ble = wifi = 0;
    for (auto& f : targetFilters) (isWifiDomain(f.type) ? wifi : ble)++;
}

static void cardHeader(M5Canvas& c, const char* title) {
    c.fillRect(0, 0, 240, 15, CARD_PURPLE);
    c.setTextSize(1);
    c.setTextColor(TFT_WHITE);
    c.setCursor(4, 4);
    c.print(title);

    int x = 236;
    char b[16];
    int bat = M5Cardputer.Power.getBatteryLevel();
    if (bat >= 0) {
        snprintf(b, sizeof(b), "%d%%", bat);
        x -= c.textWidth(b);
        c.setTextColor(bat < 20 ? TFT_RED : TFT_WHITE);
        c.setCursor(x, 4);
        c.print(b);
        x -= 6;
    }
    uint32_t rgb = strip.current();
    uint16_t dot = (ledEnabled && rgb) ? c.color565((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF) : CARD_DIM;
    c.fillCircle(x - 3, 7, 3, dot);
    x -= 10;
    const char* snd = buzzerEnabled ? "SND" : "MUTE";
    x -= c.textWidth(snd);
    c.setTextColor(buzzerEnabled ? TFT_WHITE : TFT_RED);
    c.setCursor(x, 4);
    c.print(snd);
    x -= 6;
    if (meshCfg.enabled && currentMode == SCANNING_MODE) {
        snprintf(b, sizeof(b), "M%d", (int)meshNodeCount());
        x -= c.textWidth(b);
        c.setTextColor(g_statusLed ? TFT_GREEN : TFT_WHITE);
        c.setCursor(x, 4);
        c.print(b);
    }
}

static void cardFooter(M5Canvas& c, const char* hint) {
    c.fillRect(0, 124, 240, 11, TFT_BLACK);
    c.setTextSize(1);
    c.setTextColor(CARD_DIM);
    c.setCursor(4, 126);
    c.print(hint);
}

// Generic scrolling list. rows visible = 8 at 13 px.
template <typename F>
static void cardList(M5Canvas& c, int count, int sel, F drawRow, int top = 18, int rowH = 13, int rows = 8) {
    int first = sel < rows ? 0 : sel - rows + 1;
    for (int i = 0; i < rows && first + i < count; i++) {
        int y = top + i * rowH;
        bool s = (first + i) == sel;
        if (s) c.fillRect(0, y - 2, 240, rowH, CARD_SELBG);
        drawRow(first + i, y, s);
    }
    if (count > rows) {   // scrollbar
        int h = max(6, (rows * rows * rowH) / count);
        int y = top - 2 + (rows * rowH - h) * first / max(1, count - rows);
        c.fillRect(237, y, 3, h, CARD_DIM);
    }
}

static const char* CARD_MENU_ITEMS[] = {
    "Start scanning", "Filters", "Vendor database", "Settings", "Web portal (WiFi AP)", "Help",
};
static const int CARD_MENU_COUNT = 6;

static void cardDrawMenu(M5Canvas& c) {
    cardHeader(c, "SPYPUTER  menu");
    int ble, wifi;
    cardFilterCounts(ble, wifi);
    cardList(c, CARD_MENU_COUNT, cardSel[CV_MENU], [&](int i, int y, bool s) {
        c.setTextColor(s ? TFT_WHITE : 0xC618);
        c.setCursor(10, y + 3);
        c.setTextSize(1);
        c.print(s ? "> " : "  ");
        c.print(CARD_MENU_ITEMS[i]);
        char v[32] = "";
        if (i == 0) snprintf(v, sizeof(v), "%d filters", ble + wifi);
        if (i == 1) snprintf(v, sizeof(v), "BLE %d  WiFi %d", ble, wifi);
        if (v[0]) {
            c.setTextColor(CARD_DIM);
            c.setCursor(232 - c.textWidth(v), y + 3);
            c.print(v);
        }
    }, 22, 16, 6);
    cardFooter(c, ";. move  ENTER select");
}

static void cardDrawFilters(M5Canvas& c) {
    char t[32];
    snprintf(t, sizeof(t), "Filters  (%u)", (unsigned)targetFilters.size());
    cardHeader(c, t);
    if (targetFilters.empty()) {
        c.setTextColor(CARD_DIM);
        c.setCursor(8, 40); c.print("No filters yet.");
        c.setCursor(8, 56); c.print("A  add one by hand");
        c.setCursor(8, 68); c.print("V  pick from the vendor database");
    } else {
        int& sel = cardSel[CV_FILTERS];
        sel = constrain(sel, 0, (int)targetFilters.size() - 1);
        cardList(c, targetFilters.size(), sel, [&](int i, int y, bool s) {
            const TargetFilter& f = targetFilters[i];
            c.setTextColor(isWifiDomain(f.type) ? TFT_CYAN : CARD_PINK);
            c.setCursor(4, y);
            c.print(filterTypeCode(f.type));
            c.setTextColor(TFT_WHITE);
            c.setCursor(46, y);
            String id = f.identifier;
            if ((f.type == FT_MAC_PREFIX || f.type == FT_WIFI_PROBE || f.type == FT_WIFI_BEACON) && id.length() == 6) id = cardColons(id);
            if (id.length() > 17) id = id.substring(0, 17);
            c.print(id);
            c.setTextColor(s ? 0xC618 : CARD_DIM);
            c.setCursor(152, y);
            String d = f.description;
            if (d.length() > 13) d = d.substring(0, 13);
            c.print(d);
        });
    }
    cardFooter(c, "A add  V vendors  X delete  ` back");
}

static void cardDrawAdd(M5Canvas& c) {
    cardHeader(c, "Add filter");
    const CardAddType& t = CARD_ADD_TYPES[cardAddType];
    bool blink = (millis() / 400) & 1;
    const char* labels[] = {"type", "match", "label"};
    for (int f = 0; f < 3; f++) {
        int y = 22 + f * 24;
        bool on = cardAddField == f;
        c.setTextColor(on ? CARD_PINK : CARD_DIM);
        c.setCursor(4, y + 4);
        c.print(labels[f]);
        c.drawRect(42, y, 194, 16, on ? CARD_PINK : 0x4208);
        c.setTextColor(TFT_WHITE);
        c.setCursor(46, y + 4);
        if (f == 0) {
            c.printf("< %s%s >", t.code, cardAddType == 0 ? " BLE+WiFi" : "");
        } else {
            const CardField& fld = f == 1 ? cardAddId : cardAddDesc;
            const char* s = fld.buf;
            int maxc = 30;
            if (fld.len > maxc) s += fld.len - maxc;
            c.print(s);
            if (on && blink) c.print("_");
            if (!fld.len && !on) { c.setTextColor(0x4208); c.print(f == 2 ? "(optional)" : ""); }
        }
    }
    c.setTextColor(CARD_DIM);
    c.setCursor(4, 96);
    c.print(t.hint);
    cardFooter(c, cardAddField == 0 ? ", / type  TAB/ENTER next  ` cancel"
                                    : "TAB next  ENTER save  ` cancel");
}

static void cardDrawVendors(M5Canvas& c) {
    cardHeader(c, "Vendor database");
    int& sel = cardSel[CV_VENDORS];
    int n = cardVendorTotal();
    cardList(c, n, sel, [&](int i, int y, bool s) {
        int total, have = cardVendorInstalled(i, total);
        const char* name = i < CARD_PACK_COUNT ? CARD_PACKS[i].name : CARD_VENDORS[i - CARD_PACK_COUNT].name;
        c.setTextColor(have ? TFT_GREEN : CARD_DIM);
        c.setCursor(4, y);
        c.print(have == total ? "[x]" : have ? "[~]" : "[ ]");
        c.setTextColor(s ? TFT_WHITE : 0xC618);
        c.setCursor(28, y);
        char nm[30];
        strlcpy(nm, name, sizeof(nm));
        c.print(nm);
        char v[12];
        snprintf(v, sizeof(v), "%d", total);
        c.setTextColor(CARD_DIM);
        c.setCursor(232 - c.textWidth(v), y);
        c.print(v);
    }, 18, 13, 7);
    const char* cat = sel < CARD_PACK_COUNT ? CARD_PACKS[sel].category : CARD_VENDORS[sel - CARD_PACK_COUNT].category;
    char cb[40];
    strlcpy(cb, cat, sizeof(cb));
    c.setTextColor(TFT_CYAN);
    c.setCursor(4, 112);
    c.print(cb);
    cardFooter(c, "ENTER toggle  A add all  R R clear  `");
}

static void cardDrawSettings(M5Canvas& c) {
    cardHeader(c, "Settings");
    cardList(c, ST_COUNT, cardSel[CV_SETTINGS], [&](int i, int y, bool s) {
        char name[32], val[16];
        cardSettingLabel(i, name, sizeof(name), val, sizeof(val));
        bool meshDim = (i == ST_MESH_ROLE || i == ST_MESH_CH) && !meshCfg.enabled;
        c.setTextColor(meshDim ? 0x4208 : s ? TFT_WHITE : 0xC618);
        c.setCursor(6, y);
        c.print(name);
        c.setTextColor(i == ST_FACTORY ? TFT_RED : TFT_CYAN);
        c.setCursor(232 - c.textWidth(val), y);
        c.print(val);
    });
    cardFooter(c, "ENTER change  , / adjust  ` back");
}

static void cardDrawHelp(M5Canvas& c) {
    cardHeader(c, "Help");
    static const char* lines[] = {
        "; .   up / down        , /  left/right",
        "ENTER select           ` or DEL  back",
        "Scanning:  ; . select   D details",
        "  M mute   S stealth   ` ` stop scan",
        "Filters:  A add   X delete   V vendors",
        "G0 button hold 1.5s: unlock + menu",
        "Detail: I ignore device (no alerts)",
        "Serial 115200: upstream console + TLM",
    };
    for (int i = 0; i < 8; i++) {
        c.setTextColor(i % 2 ? TFT_WHITE : 0xDEFB);
        c.setCursor(4, 20 + i * 13);
        c.print(lines[i]);
    }
    cardFooter(c, "any key: back");
}

static void cardDrawPortal(M5Canvas& c) {
    cardHeader(c, "Web portal");
    int ble, wifi;
    cardFilterCounts(ble, wifi);
    c.setTextSize(1);
    c.setTextColor(CARD_PINK);
    c.setCursor(4, 20);  c.print("Join WiFi:");
    c.setTextColor(TFT_WHITE);
    c.setTextSize(2);
    c.setCursor(4, 32);  c.print(AP_SSID);
    c.setTextSize(1);
    c.setTextColor(CARD_DIM);
    c.setCursor(4, 52);  c.print("pass ");
    c.setTextColor(TFT_WHITE); c.print(AP_PASSWORD);
    c.setTextColor(CARD_DIM);
    c.setCursor(4, 64);  c.print("open ");
    c.setTextColor(TFT_CYAN);  c.print("http://192.168.4.1");
    c.setTextColor(CARD_DIM);
    c.setCursor(4, 80);  c.printf("filters  BLE %d  WiFi %d", ble, wifi);
    c.setCursor(4, 92);  c.printf("clients  %d", (int)WiFi.softAPgetStationNum());
    cardFooter(c, "ENTER start scan   ` close portal");
}

static void cardDrawScan(M5Canvas& c) {
    cardHeader(c, cardLocked ? "SPYPUTER  scanning [locked]" : "SPYPUTER  scanning");
    bool hw, hb;
    scanDomains(hw, hb);
    c.setTextSize(1);
    c.setTextColor(CARD_PINK);
    c.setCursor(4, 19);
    if (!hw && !hb)                         c.print(meshCfg.enabled ? "mesh relay only" : "idle: no filters");
    else if (g_radioPhase == PHASE_WIFI_SWEEP) c.printf("WiFi sweep ch%d", g_wifiChannel);
    else if (g_radioPhase == PHASE_BLE_SCAN)   c.print("BLE scan");
    else                                       c.print("starting...");
    char r[40];
    snprintf(r, sizeof(r), "dev %u  alerts %lu", (unsigned)devices.size(), (unsigned long)cardTotalAlerts);
    c.setTextColor(CARD_DIM);
    c.setCursor(236 - c.textWidth(r), 19);
    c.print(r);
    c.drawFastHLine(0, 29, 240, 0x2104);

    if (!cardDetCount) {
        c.setTextColor(CARD_DIM);
        c.setCursor(4, 60);
        c.printf("Listening for %u filter(s)...", (unsigned)targetFilters.size());
        cardFooter(c, "M mute  S stealth  `` stop  H help");
        return;
    }
    int& sel = cardSel[CV_SCAN];
    sel = constrain(sel, 0, cardDetCount - 1);
    cardList(c, cardDetCount, sel, [&](int i, int y, bool s) {
        const CardDet& d = cardDets[i];
        c.fillRect(0, y - 2, 3, 18, cardRssiColor(d.rssi));
        c.setTextColor(d.wifi ? TFT_CYAN : CARD_PINK);
        c.setCursor(6, y);
        c.print(d.type);
        c.setTextColor(d.conf == 0 ? CARD_DIM : d.conf == 1 ? 0xFFE0 : TFT_WHITE);
        c.setCursor(52, y);
        char lbl[28];
        strlcpy(lbl, d.label, sizeof(lbl));
        c.print(lbl);
        c.setTextColor(CARD_DIM);
        c.setCursor(6, y + 8);
        c.print(d.mac);
        if (d.node[0]) { c.print(" @"); c.print(d.node); }
        char ago[8], rs[16];
        cardAgo(ago, sizeof(ago), d.at);
        snprintf(rs, sizeof(rs), "%d %s", d.rssi, ago);
        c.setTextColor(cardRssiColor(d.rssi));
        c.setCursor(234 - c.textWidth(rs), y + 8);
        c.print(rs);
    }, 33, 18, 5);
    cardFooter(c, ";. select  D detail  M mute  `` stop");
}

static void cardDrawDetail(M5Canvas& c) {
    if (!cardDetCount) { cardView = CV_SCAN; return; }
    const CardDet& d = cardDets[constrain(cardSel[CV_SCAN], 0, cardDetCount - 1)];
    cardHeader(c, "Detection");
    c.setTextSize(1);
    c.setTextColor(CARD_PINK);
    c.setCursor(4, 20);  c.printf("%s  (%s)", d.type, d.wifi ? "WiFi" : "BLE");
    c.setTextColor(TFT_WHITE);
    c.setTextSize(2);
    c.setCursor(4, 32);  c.print(d.mac);
    c.setTextSize(1);
    c.setCursor(4, 52);  c.print(d.label);
    String alias = getDeviceAlias(String(d.mac));
    char ago[8];
    cardAgo(ago, sizeof(ago), d.at);
    c.setTextColor(CARD_DIM);
    c.setCursor(4, 66);  c.printf("alias  %s", alias.length() ? alias.c_str() : "-");
    c.setCursor(4, 78);  c.print("rssi   ");
    c.setTextColor(cardRssiColor(d.rssi)); c.printf("%d dBm", d.rssi);
    c.setTextColor(CARD_DIM);
    c.setCursor(4, 90);  c.printf("seen   %s ago, %u time(s)", ago, d.hits);
    c.setCursor(4, 102); c.printf("node   %s", d.node[0] ? d.node : "this one");
    c.setCursor(140, 78); c.printf("conf %s", CARD_CONF_NAME[d.conf % 3]);
    if (cardIgnored(d.mac)) { c.setTextColor(TFT_ORANGE); c.setCursor(140, 90); c.print("IGNORED"); }
    cardFooter(c, "; . prev/next  I ignore  any key: back");
}

static void cardDrawAlert(M5Canvas& c) {
    const CardDet& d = cardDets[0];
    bool blink = ((millis() / 250) & 1);
    c.fillScreen(blink ? TFT_RED : CARD_PURPLE);
    c.setTextColor(TFT_WHITE);
    c.setTextSize(3);
    c.setCursor(12, 12);
    c.print("DETECTED");
    c.setTextSize(1);
    c.setCursor(12, 46);
    c.printf("%s  %s  %d dBm  %s conf", d.type, d.wifi ? "WiFi" : "BLE", d.rssi, CARD_CONF_NAME[d.conf % 3]);
    c.setTextSize(2);
    c.setCursor(12, 62);
    char lbl[19];
    strlcpy(lbl, d.label, sizeof(lbl));
    c.print(lbl);
    c.setTextSize(1);
    c.setCursor(12, 86);
    c.print(d.mac);
    if (d.node[0]) { c.print("  via mesh "); c.print(d.node); }
    c.setCursor(12, 112);
    c.print("any key to dismiss");
}

static void cardRender() {
    if (cardStealth || !cardCanvasOk) return;
    M5Canvas& c = cardCanvas;
    c.fillScreen(TFT_BLACK);
    c.setTextSize(1);
    if (currentMode == SCANNING_MODE && cardDetCount && (long)(cardAlertUntil - millis()) > 0) {
        cardDrawAlert(c);
    } else {
        switch (cardView) {
            case CV_MENU:     cardDrawMenu(c);     break;
            case CV_FILTERS:  cardDrawFilters(c);  break;
            case CV_ADD:      cardDrawAdd(c);      break;
            case CV_VENDORS:  cardDrawVendors(c);  break;
            case CV_SETTINGS: cardDrawSettings(c); break;
            case CV_HELP:     cardDrawHelp(c);     break;
            case CV_PORTAL:   cardDrawPortal(c);   break;
            case CV_SCAN:     cardDrawScan(c);     break;
            case CV_DETAIL:   cardDrawDetail(c);   break;
        }
    }
    if ((long)(cardToastUntil - millis()) > 0) {
        c.setTextSize(1);
        int w = c.textWidth(cardToast) + 12;
        c.fillRoundRect((240 - w) / 2, 56, w, 20, 4, TFT_WHITE);
        c.setTextColor(TFT_BLACK);
        c.setCursor((240 - w) / 2 + 6, 62);
        c.print(cardToast);
    }
    c.pushSprite(0, 0);
}

// ===================================================================== input
static void cardSetStealth(bool on) {
    cardStealth = on;
    M5Cardputer.Display.setBrightness(on ? 0 : CARD_BRIGHT[cardBrightIdx]);
    if (on) M5Cardputer.Display.fillScreen(TFT_BLACK);
}

static void cardMove(int& sel, int count, int d) {
    if (count <= 0) { sel = 0; return; }
    sel = (sel + d + count) % count;
}

// k = printable char (0 if none). Navigation keys arrive as their printable
// glyphs (; . , / `) since that is what the Cardputer keyboard reports.
static void cardHandleKey(char k, const Keyboard_Class::KeysState& st) {
    bool back = (k == '`') || st.del;
    bool up = k == ';', down = k == '.', left = k == ',', right = k == '/';
    char lk = tolower(k);

    if (cardStealth) { cardSetStealth(false); return; }
    if (currentMode == SCANNING_MODE && (long)(cardAlertUntil - millis()) > 0) { cardAlertUntil = 0; return; }

    switch (cardView) {
    case CV_MENU:
        if (up)   cardMove(cardSel[CV_MENU], CARD_MENU_COUNT, -1);
        if (down) cardMove(cardSel[CV_MENU], CARD_MENU_COUNT, +1);
        if (lk == 's') cardStartScan();
        if (lk == 'f') cardView = CV_FILTERS;
        if (lk == 'v') cardView = CV_VENDORS;
        if (st.enter) {
            switch (cardSel[CV_MENU]) {
                case 0: cardStartScan(); break;
                case 1: cardView = CV_FILTERS; break;
                case 2: cardView = CV_VENDORS; break;
                case 3: cardView = CV_SETTINGS; break;
                case 4: cardStartPortal(); break;
                case 5: cardView = CV_HELP; break;
            }
        }
        break;

    case CV_FILTERS: {
        int n = targetFilters.size();
        int& sel = cardSel[CV_FILTERS];
        if (up)   cardMove(sel, n, -1);
        if (down) cardMove(sel, n, +1);
        if (lk == 'a' || (st.enter && !n)) {
            cardAddField = 0; cardAddId.clear(); cardAddDesc.clear();
            cardView = CV_ADD;
        }
        if (lk == 'v') cardView = CV_VENDORS;
        if (lk == 'x' && n && sel < n) {
            if (cardConfirm(200 + sel, "X again to delete")) {
                targetFilters.erase(targetFilters.begin() + sel);
                cardFiltersChanged();
                if (sel >= (int)targetFilters.size()) sel = max(0, (int)targetFilters.size() - 1);
                cardToastf("deleted");
            }
        }
        if (back) cardView = CV_MENU;
        break;
    }

    case CV_ADD:
        if (k == '`') { cardView = CV_FILTERS; break; }
        if (st.tab) { cardAddField = (cardAddField + 1) % 3; break; }
        if (cardAddField == 0) {
            if (left || up)    cardMove(cardAddType, CARD_ADD_TYPE_COUNT, -1);
            if (right || down) cardMove(cardAddType, CARD_ADD_TYPE_COUNT, +1);
            if (st.enter) cardAddField = 1;
            if (st.del) cardView = CV_FILTERS;
            break;
        }
        {
            CardField& f = cardAddField == 1 ? cardAddId : cardAddDesc;
            if (st.enter) {
                if (cardAddField == 1 && !cardAddId.len) { cardToastf("type the value to match"); break; }
                if (cardAddCommit()) cardView = CV_FILTERS;
                break;
            }
            if (st.del) { f.back(); break; }
            if (st.space) { f.put(' '); break; }
            for (char ch : st.word) if (ch >= 0x20 && ch < 0x7f && ch != '"' && ch != '\\') f.put(ch);
        }
        break;

    case CV_VENDORS: {
        int& sel = cardSel[CV_VENDORS];
        if (up)   cardMove(sel, cardVendorTotal(), -1);
        if (down) cardMove(sel, cardVendorTotal(), +1);
        if (st.enter) cardVendorToggle(sel);
        if (lk == 'a') cardToastf("added %d filters (%u total)", cardInstallAll(), (unsigned)targetFilters.size());
        if (lk == 'r' && cardConfirm(400, "R again: remove ALL filters")) {
            targetFilters.clear();
            cardFiltersChanged();
            cardToastf("all filters removed");
        }
        if (back) cardView = CV_MENU;
        break;
    }

    case CV_SETTINGS: {
        int& sel = cardSel[CV_SETTINGS];
        if (up)   cardMove(sel, ST_COUNT, -1);
        if (down) cardMove(sel, ST_COUNT, +1);
        if (st.enter) cardSettingChange(sel, 0);
        if (left)  cardSettingChange(sel, -1);
        if (right) cardSettingChange(sel, +1);
        if (back) cardView = CV_MENU;
        break;
    }

    case CV_HELP:
        cardView = currentMode == SCANNING_MODE ? CV_SCAN : cardPortalUp ? CV_PORTAL : CV_MENU;
        break;

    case CV_PORTAL:
        if (st.enter) cardStartScan();
        if (back && cardConfirm(300, "` again to close the portal")) cardBackToMenu();
        break;

    case CV_SCAN: {
        int& sel = cardSel[CV_SCAN];
        if (up)   cardMove(sel, cardDetCount, -1);
        if (down) cardMove(sel, cardDetCount, +1);
        if ((lk == 'd' || st.enter) && cardDetCount) cardView = CV_DETAIL;
        if (lk == 'h' || k == '?') cardView = CV_HELP;
        if (lk == 's') cardSetStealth(true);
        if (lk == 'm') {
            buzzerEnabled = !buzzerEnabled;
            if (!buzzerEnabled) buzzWrite(0);
            cardSavePref("buzzerEnabled", buzzerEnabled);
            cardToastf(buzzerEnabled ? "sound on" : "muted");
        }
        if (lk == 'l') {
            ledEnabled = !ledEnabled;
            cardSavePref("ledEnabled", ledEnabled);
            cardToastf(ledEnabled ? "LED on" : "LED off");
        }
        if (back && cardConfirm(301, "` again to stop scanning")) cardBackToMenu();
        break;
    }

    case CV_DETAIL:
        if (lk == 'i' && cardDetCount) {
            const CardDet& d = cardDets[constrain(cardSel[CV_SCAN], 0, cardDetCount - 1)];
            cardToastf(cardIgnToggle(d.mac) ? "ignored: no more alerts" : "un-ignored");
            break;
        }
        if (up)        cardMove(cardSel[CV_SCAN], cardDetCount, -1);
        else if (down) cardMove(cardSel[CV_SCAN], cardDetCount, +1);
        else           cardView = CV_SCAN;
        break;
    }
}

// ======================================================= serial test hooks
// cardkey <key>   inject a keypress: a char, or enter|del|tab|esc|up|down|left|right
// cardshot        dump the screen: "CARDSHOT w h" then h lines of RGB565 hex
static void cardSyncView();
static void cardSplashTo(lgfx::LovyanGFX& d, const char* line);
static bool cardSerialCommand(const String& line) {
    if (line.startsWith("cardkey ")) {
        String a = line.substring(8);
        Keyboard_Class::KeysState st;
        char k = 0;
        if      (a == "enter") st.enter = true;
        else if (a == "del")   st.del = true;
        else if (a == "tab")   st.tab = true;
        else if (a == "esc")   k = '`';
        else if (a == "up")    k = ';';
        else if (a == "down")  k = '.';
        else if (a == "left")  k = ',';
        else if (a == "right") k = '/';
        else if (a == "space") st.space = true;
        else for (unsigned i = 0; i < a.length(); i++) st.word.push_back(a[i]);
        if (k) st.word.push_back(k);
        if (!st.word.empty()) k = st.word[0];
        cardHandleKey(k, st);
        cardSyncView();
        cardRender();
        Serial.printf("OK cardkey view=%d\n", (int)cardView);
        return true;
    }
    if (line == "cardpanic") {            // diagnostics: verify crash reporting
        Serial.println("OK panicking");
        Serial.flush();
        delay(100);
        abort();
    }
    if (line.startsWith("carddemo")) {      // fictional sample detections for screenshots
        struct { const char* mac; const char* label; const char* type; int rssi; bool wifi; uint8_t conf; int ago; }
        demo[] = {
            {"b4:1e:52:3a:91:0c", "Flock-MA-L",          "FLOCK",   -58, true,  2, 4},
            {"00:25:df:41:7e:22", "Axon",                "AXON",    -71, false, 2, 9},
            {"6d:12:c4:08:5e:31", "RayBanMeta",          "META",    -66, false, 1, 15},
            {"98:d3:31:f2:10:aa", "Skim-HC05 HC-05",     "SKIMMER", -77, false, 2, 32},
            {"5a:0b:7e:63:29:d4", "Apple",               "AIRTAG",  -82, false, 1, 47},
            {"24:b2:b9:6c:02:5f", "Flock-Liteo",         "FLOCK",   -90, true,  0, 63},
        };
        cardDemoFreeze = true;
        cardDetCount = 0;
        for (int i = (int)(sizeof(demo) / sizeof(demo[0])) - 1; i >= 0; i--) {
            String d = String(demo[i].label) + (demo[i].conf == 0 ? " [LOW]" : demo[i].conf == 1 ? " [MED]" : " [HIGH]");
            cardOnDetection(demo[i].mac, "", d, demo[i].rssi, demo[i].type, demo[i].wifi, "", false, demo[i].conf, true);
            cardDets[0].at = millis() - demo[i].ago * 1000UL;
            cardDets[0].hits = 1 + (i * 3) % 7;
        }
        cardTotalAlerts = 5;
        cardSel[CV_SCAN] = 0;
        cardView = CV_SCAN;
        if (line.endsWith("alert")) cardAlertUntil = millis() + 60000;
        else cardAlertUntil = 0;
        Serial.println("OK carddemo");
        return true;
    }
    if (line == "cardshot splash" && cardCanvasOk) {
        cardSplashTo(cardCanvas, "scanning...");
        int w = cardCanvas.width(), h = cardCanvas.height();
        Serial.printf("CARDSHOT %d %d\n", w, h);
        char row[240 * 4 + 2];
        for (int y = 0; y < h; y++) {
            int n = 0;
            for (int x = 0; x < w && x < 240; x++) n += sprintf(row + n, "%04X", cardCanvas.readPixel(x, y));
            Serial.println(row);
        }
        Serial.println("CARDSHOT END");
        return true;
    }
    if (line == "cardshot") {
        if (!cardCanvasOk) { Serial.println("ERR no screen buffer"); return true; }
        cardRender();
        int w = cardCanvas.width(), h = cardCanvas.height();
        Serial.printf("CARDSHOT %d %d\n", w, h);
        char row[240 * 4 + 2];
        for (int y = 0; y < h; y++) {
            int n = 0;
            for (int x = 0; x < w && x < 240; x++) n += sprintf(row + n, "%04X", cardCanvas.readPixel(x, y));
            Serial.println(row);
        }
        Serial.println("CARDSHOT END");
        return true;
    }
    return false;
}

// ================================================================= lifecycle
static void cardSplashTo(lgfx::LovyanGFX& d, const char* line) {
    d.fillScreen(TFT_BLACK);
    d.setTextSize(1);
    d.setTextColor(CARD_PINK);
    for (int i = 0; i < SPYPUTER_ART_LINES; i++) {   // 38 cols x 6 px = 228 px
        d.setCursor(6, 14 + i * 9);
        d.print(SPYPUTER_ART[i]);
    }
    d.setTextColor(TFT_WHITE);
    d.setCursor(6, 68);
    d.print("OUI-SPY x SquachWatch");
    d.setTextColor(CARD_DIM);
    d.setCursor(6, 80);
    d.print("colonelpanichacks + TalkingSasquach");
    d.setTextColor(TFT_CYAN);
    d.setCursor(6, 104);
    d.print(line);
}
static void cardSplash(const char* line) { cardSplashTo(M5Cardputer.Display, line); }

static void cardUiBegin() {
    preferences.begin("ouispy", true);
    cardLocked    = preferences.getBool("configLocked", false);
    cardAutoScan  = preferences.getBool("cp_autoscan", true);
    swEnabled     = preferences.getBool("cp_sw", true);
    swIBeacon     = preferences.getBool("cp_ibeacon", false);
    cardAlertMin  = preferences.getUChar("cp_alertmin", 1) % 3;
    cardIgnCount  = (int)(preferences.getBytes("cp_ign", cardIgn, sizeof(cardIgn)) / 6);
    g_spkVolume   = preferences.getUChar("cp_vol", g_spkVolume);
    cardBrightIdx = preferences.getUChar("cp_bright", cardBrightIdx) % sizeof(CARD_BRIGHT);
    preferences.end();
    M5Cardputer.Speaker.setVolume(g_spkVolume);
    M5Cardputer.Display.setBrightness(CARD_BRIGHT[cardBrightIdx]);
    cardCanvas.setColorDepth(8);
    cardCanvasOk = cardCanvas.createSprite(M5Cardputer.Display.width(), M5Cardputer.Display.height()) != nullptr;
    if (isSerialConnected()) {
        Serial.printf("[UI] reset reason=%d (1 pwr,3 sw,4 panic,5 int-wdt,6 task-wdt,7 wdt,9 brownout)\n",
                      (int)esp_reset_reason());
        if (cardCrumbMagic == 0xC0FFEE42)
            Serial.printf("[UI] last crumb: stage=%u view=%u key=0x%02X at=%lums\n",
                          cardCrumbStage, cardCrumbView, cardCrumbKey, (unsigned long)cardCrumbAt);
        Serial.printf("[UI] board=%d display=%dx%d rot=%d sprite=%s heap=%u\n",
                      (int)M5.getBoard(), M5Cardputer.Display.width(), M5Cardputer.Display.height(),
                      M5Cardputer.Display.getRotation(), cardCanvasOk ? "ok" : "FAILED",
                      (unsigned)ESP.getFreeHeap());
    }
}

// Boot (no burn-in lock): scan right away unless the user asked for the
// menu (stop scanning / web portal close), auto-scan is off, or there is
// nothing to scan for.
static bool cardShouldAutoScan() {
    preferences.begin("ouispy", false);
    bool menuOnce = preferences.getBool("cp_menu", false);
    if (menuOnce) preferences.remove("cp_menu");
    bool seeded = preferences.getBool("cp_seeded", false);
    if (!seeded) preferences.putBool("cp_seeded", true);
    preferences.end();
    // First boot of a fresh install: scan for everything in the database.
    // Once only — deleting filters later is respected.
    if (!seeded && targetFilters.empty()) {
        int n = cardInstallAll();
        if (isSerialConnected()) Serial.printf("[UI] first boot: installed all %d vendor signatures\n", n);
    }
    if (menuOnce || !cardAutoScan) return false;
    return !targetFilters.empty() || meshCfg.enabled || ridEnabled || swEnabled;
}

// On-device menu, no AP.
static void cardEnterMenu() {
    currentMode = CONFIG_MODE;
    cardPortalUp = false;
    cardView = CV_MENU;
    if (isSerialConnected()) Serial.println("[UI] on-device menu (web portal off; Menu > Web portal to start it)");
}

// Called by upstream startScanningMode() paths (portal save, serial
// provisiondone, burn-in boot) so the screen follows the mode.
static void cardSyncView() {
    if (currentMode == SCANNING_MODE && cardView != CV_SCAN && cardView != CV_DETAIL && cardView != CV_HELP)
        cardView = CV_SCAN;
    if (currentMode == CONFIG_MODE && cardPortalUp && cardView == CV_MENU) cardView = CV_PORTAL;
}

// Top of every loop() pass.
static void cardTick() {
    g_rgbMute = !ledEnabled || cardStealth;
    cardCrumb(1);
    M5Cardputer.update();
    cardCrumb(2);
    if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
        auto& st = M5Cardputer.Keyboard.keysState();
        char k = st.word.empty() ? 0 : st.word[0];
        if (k || st.enter || st.del || st.space || st.tab) {
            cardCrumbKey = st.enter ? 0x0D : st.del ? 0x08 : (uint8_t)k;
            cardCrumbView = cardView;
            cardCrumb(3);
            cardHandleKey(k, st);
            cardCrumb(4);
            cardRender();
            cardCrumb(5);
        }
    }
    cardSyncView();
    static unsigned long last = 0;
    unsigned long now = millis();
    unsigned long every = (long)(cardAlertUntil - now) > 0 ? 120 : (cardView == CV_ADD ? 150 : 300);
    static int lastView = -1;
    if (now - last >= every || lastView != cardView) {
        last = now;
        lastView = cardView;
        cardCrumbView = cardView;
        cardCrumb(6);
        cardRender();
    }
    cardCrumb(7);
}
