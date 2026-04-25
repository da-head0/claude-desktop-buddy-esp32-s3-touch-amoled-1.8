#include "ble_hid.h"
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEService.h>
#include <BLECharacteristic.h>
#include <BLEDescriptor.h>
#include <BLE2902.h>
#include <BLEAdvertising.h>
#include <Arduino.h>
#include <esp_gatt_defs.h>

// ─── HOGP (HID over GATT) — Boot Keyboard subset ───────────────────────
//
// Three services attached to the existing BLEServer:
//   0x180A  Device Information Service  (Manufacturer, PnP ID)
//   0x180F  Battery Service              (level, mostly cosmetic)
//   0x1812  HID Service                  (Report Map + Input/Boot reports)
//
// Every char is gated with ESP_GATT_PERM_*_ENCRYPTED so macOS reuses the
// LE Secure Connections LTK from the NUS bond — no second pairing.
// Without these perms macOS treats HID as unbonded and silently drops
// notifications.
//
// Report Map: standard 8-byte boot keyboard, no Report ID. Adding a
// non-zero Report ID would require Set-Protocol toggling and breaks
// macOS's Boot Keyboard fallback during the security handshake.

static const uint8_t REPORT_MAP[] = {
  0x05, 0x01,        // Usage Page (Generic Desktop)
  0x09, 0x06,        // Usage (Keyboard)
  0xA1, 0x01,        // Collection (Application)
  0x05, 0x07,        //   Usage Page (Key Codes)
  0x19, 0xE0,        //   Usage Minimum (224 — LeftControl)
  0x29, 0xE7,        //   Usage Maximum (231 — RightGUI)
  0x15, 0x00,        //   Logical Min (0)
  0x25, 0x01,        //   Logical Max (1)
  0x75, 0x01,        //   Report Size (1)
  0x95, 0x08,        //   Report Count (8)
  0x81, 0x02,        //   Input (Data, Var, Abs)        — modifier byte
  0x95, 0x01,        //   Report Count (1)
  0x75, 0x08,        //   Report Size (8)
  0x81, 0x01,        //   Input (Const, Array, Abs)     — reserved byte
  0x95, 0x05,        //   Report Count (5)
  0x75, 0x01,        //   Report Size (1)
  0x05, 0x08,        //   Usage Page (LEDs)
  0x19, 0x01,        //   Usage Min (Num Lock)
  0x29, 0x05,        //   Usage Max (Kana)
  0x91, 0x02,        //   Output (Data, Var, Abs)       — host LED state
  0x95, 0x01,        //   Report Count (1)
  0x75, 0x03,        //   Report Size (3)
  0x91, 0x01,        //   Output (Const, Array, Abs)    — LED padding
  0x95, 0x06,        //   Report Count (6)
  0x75, 0x08,        //   Report Size (8)
  0x15, 0x00,        //   Logical Min (0)
  0x25, 0x65,        //   Logical Max (101)
  0x05, 0x07,        //   Usage Page (Key Codes)
  0x19, 0x00,        //   Usage Min (0)
  0x29, 0x65,        //   Usage Max (101)
  0x81, 0x00,        //   Input (Data, Array)           — 6 keycode slots
  0xC0               // End Collection
};

static BLECharacteristic* s_inputChar = nullptr;
static BLECharacteristic* s_bootChar  = nullptr;
static volatile bool      s_subscribed = false;

// macOS subscribes to the Input Report's CCCD as part of HID enumeration.
// Until that subscription lands, notify() goes nowhere. main.cpp gates
// the dictation tap on bleHidReady() to avoid silent drops on the very
// first connection.
class CccdCb : public BLEDescriptorCallbacks {
public:
  void onWrite(BLEDescriptor* d) override {
    uint8_t* v = d->getValue();
    size_t len = d->getLength();
    s_subscribed = (len >= 1 && (v[0] & 0x01) != 0);
    Serial.printf("[hid] cccd=%s\n", s_subscribed ? "on" : "off");
  }
};

void bleHidAttach(BLEServer* server, BLEAdvertising* adv) {
  // ─── Device Information Service ─────────────────────────────────────
  BLEService* dis = server->createService(BLEUUID((uint16_t)0x180A));

  BLECharacteristic* mfr = dis->createCharacteristic(
      BLEUUID((uint16_t)0x2A29), BLECharacteristic::PROPERTY_READ);
  mfr->setAccessPermissions(ESP_GATT_PERM_READ_ENCRYPTED);
  mfr->setValue("Anthropic");

  BLECharacteristic* pnp = dis->createCharacteristic(
      BLEUUID((uint16_t)0x2A50), BLECharacteristic::PROPERTY_READ);
  pnp->setAccessPermissions(ESP_GATT_PERM_READ_ENCRYPTED);
  // PnP ID layout (Bluetooth SIG): vendor source (1=Bluetooth SIG, 2=USB),
  // VID (2 bytes LE), PID (2 bytes LE), Product version (2 bytes LE).
  // Espressif's USB VID 0x303A; PID/version arbitrary.
  uint8_t pnpVal[] = { 0x02, 0x3A, 0x30, 0xC1, 0x03, 0x01, 0x00 };
  pnp->setValue(pnpVal, sizeof(pnpVal));
  dis->start();

  // ─── Battery Service ────────────────────────────────────────────────
  BLEService* batt = server->createService(BLEUUID((uint16_t)0x180F));
  BLECharacteristic* lvl = batt->createCharacteristic(
      BLEUUID((uint16_t)0x2A19),
      BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  lvl->setAccessPermissions(ESP_GATT_PERM_READ_ENCRYPTED);
  uint8_t pct = 100;
  lvl->setValue(&pct, 1);
  BLE2902* battCccd = new BLE2902();
  battCccd->setAccessPermissions(
      ESP_GATT_PERM_READ_ENCRYPTED | ESP_GATT_PERM_WRITE_ENCRYPTED);
  lvl->addDescriptor(battCccd);
  batt->start();

  // ─── HID Service ────────────────────────────────────────────────────
  // Handle count bumped to 30 — default 15 isn't enough for HID Info,
  // Report Map, Control Point, Protocol Mode, Input + CCCD + Report Ref,
  // and Boot Input + CCCD all in one service.
  BLEService* hid = server->createService(BLEUUID((uint16_t)0x1812), 30);

  BLECharacteristic* hidInfo = hid->createCharacteristic(
      BLEUUID((uint16_t)0x2A4A), BLECharacteristic::PROPERTY_READ);
  hidInfo->setAccessPermissions(ESP_GATT_PERM_READ_ENCRYPTED);
  // bcdHID 1.11, country 0 (none), flags 0x01 (RemoteWake).
  uint8_t hidInfoVal[] = { 0x11, 0x01, 0x00, 0x01 };
  hidInfo->setValue(hidInfoVal, sizeof(hidInfoVal));

  BLECharacteristic* repMap = hid->createCharacteristic(
      BLEUUID((uint16_t)0x2A4B), BLECharacteristic::PROPERTY_READ);
  repMap->setAccessPermissions(ESP_GATT_PERM_READ_ENCRYPTED);
  repMap->setValue((uint8_t*)REPORT_MAP, sizeof(REPORT_MAP));

  // HID Control Point (write-only suspend/exit-suspend signal). We don't
  // act on it but it must exist for spec conformance.
  BLECharacteristic* hidCtrl = hid->createCharacteristic(
      BLEUUID((uint16_t)0x2A4C), BLECharacteristic::PROPERTY_WRITE_NR);
  hidCtrl->setAccessPermissions(ESP_GATT_PERM_WRITE_ENCRYPTED);
  uint8_t ctrlInit = 0;
  hidCtrl->setValue(&ctrlInit, 1);

  // Protocol Mode (1 = Report, 0 = Boot). macOS happily takes Report.
  BLECharacteristic* proto = hid->createCharacteristic(
      BLEUUID((uint16_t)0x2A4E),
      BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE_NR);
  proto->setAccessPermissions(
      ESP_GATT_PERM_READ_ENCRYPTED | ESP_GATT_PERM_WRITE_ENCRYPTED);
  uint8_t protoVal = 1;
  proto->setValue(&protoVal, 1);

  // Input Report — the channel macOS subscribes to and reads keystrokes
  // from. Report Reference (0x2908) tags it as report id 0, type input.
  s_inputChar = hid->createCharacteristic(
      BLEUUID((uint16_t)0x2A4D),
      BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  s_inputChar->setAccessPermissions(ESP_GATT_PERM_READ_ENCRYPTED);
  uint8_t zero[8] = {0,0,0,0,0,0,0,0};
  s_inputChar->setValue(zero, 8);

  BLE2902* inCccd = new BLE2902();
  inCccd->setAccessPermissions(
      ESP_GATT_PERM_READ_ENCRYPTED | ESP_GATT_PERM_WRITE_ENCRYPTED);
  inCccd->setCallbacks(new CccdCb());
  s_inputChar->addDescriptor(inCccd);

  BLEDescriptor* repRef = new BLEDescriptor(BLEUUID((uint16_t)0x2908));
  repRef->setAccessPermissions(ESP_GATT_PERM_READ_ENCRYPTED);
  uint8_t repRefVal[] = { 0x00, 0x01 };  // report id 0, type 1 (input)
  repRef->setValue(repRefVal, sizeof(repRefVal));
  s_inputChar->addDescriptor(repRef);

  // Boot Keyboard Input Report — separate fixed-format channel some hosts
  // fall back to before HID enumeration completes. We mirror the same
  // bytes here so it doesn't matter which one macOS reads from.
  s_bootChar = hid->createCharacteristic(
      BLEUUID((uint16_t)0x2A22),
      BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  s_bootChar->setAccessPermissions(ESP_GATT_PERM_READ_ENCRYPTED);
  s_bootChar->setValue(zero, 8);
  BLE2902* bootCccd = new BLE2902();
  bootCccd->setAccessPermissions(
      ESP_GATT_PERM_READ_ENCRYPTED | ESP_GATT_PERM_WRITE_ENCRYPTED);
  s_bootChar->addDescriptor(bootCccd);

  hid->start();

  // ─── Advertising ────────────────────────────────────────────────────
  // Appearance 0x03C1 = HID Keyboard. macOS uses the appearance to put us
  // in the "Keyboards" category and to auto-bond on first connect.
  adv->setAppearance(0x03C1);
  adv->addServiceUUID(BLEUUID((uint16_t)0x1812));

  Serial.println("[hid] attached (DIS + Battery + HID Keyboard)");
}

bool bleHidReady() {
  return s_subscribed && s_inputChar != nullptr;
}

// Write one boot-keyboard input report to both Input and Boot characteristics.
// Caller is responsible for pacing — macOS coalesces back-to-back identical
// reports, so a press/release pair must have a gap (delay 30 ms is enough).
static void writeReport(uint8_t modifier, uint8_t keycode) {
  if (!s_inputChar) return;
  uint8_t r[8] = { modifier, 0, keycode, 0, 0, 0, 0, 0 };
  s_inputChar->setValue(r, 8);
  s_inputChar->notify();
  if (s_bootChar) {
    s_bootChar->setValue(r, 8);
    s_bootChar->notify();
  }
}

void bleHidTap(uint8_t modifiers, uint8_t keycode) {
  writeReport(modifiers, keycode);
  delay(30);
  writeReport(0, 0);
}

void bleHidDoubleTapModifier(uint8_t modifier) {
  // macOS's "double-tap a modifier" detection window is roughly 300–400 ms
  // between releases. 30 ms hold + 80 ms gap puts the second tap well
  // inside that window without looking like a held-then-released gesture.
  writeReport(modifier, 0);
  delay(30);
  writeReport(0, 0);
  delay(80);
  writeReport(modifier, 0);
  delay(30);
  writeReport(0, 0);
}

// Map a hex digit (0..15) to the USB HID Usage Page 0x07 keycode that
// produces the matching ASCII char on a US layout (numerals row + a..f).
static uint8_t hexDigitKey(uint8_t v) {
  static const uint8_t digit[10] = {
    0x27, 0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26,
  };
  static const uint8_t letter[6] = {
    0x04, 0x05, 0x06, 0x07, 0x08, 0x09,
  };
  return v < 10 ? digit[v] : letter[v - 10];
}

// Inter-key timing inside one Hex-Input character. Tighter than
// bleHidTap's 30 ms because Option stays held — macOS only needs to
// see distinct reports, and ~10 ms is comfortably above its polling
// floor without making the whole transcript feel sluggish.
static constexpr uint32_t HEX_HOLD_MS = 10;
static constexpr uint32_t HEX_GAP_MS  = 8;

static void typeBmpCodepoint(uint16_t cp) {
  // Option down (modifier-only report — Hex Input expects Option held
  // across all four digits).
  writeReport(HID_MOD_LALT, 0);
  delay(HEX_GAP_MS);
  for (int shift = 12; shift >= 0; shift -= 4) {
    uint8_t nibble = (cp >> shift) & 0xF;
    uint8_t key = hexDigitKey(nibble);
    writeReport(HID_MOD_LALT, key);
    delay(HEX_HOLD_MS);
    writeReport(HID_MOD_LALT, 0);
    delay(HEX_GAP_MS);
  }
  writeReport(0, 0);
  delay(HEX_GAP_MS);
}

void bleHidTypeUtf8(const char* utf8) {
  if (!utf8 || !bleHidReady()) return;

  // Warm up the HID pipe: the first BLE HID notify after a long idle
  // period (push-to-talk had ~5 s of no keyboard activity while we were
  // recording + posting) consistently drops the very first hex digit
  // sent to macOS — observed as the leading 'D' of "테" (U+D14C) being
  // eaten while the rest of the transcript typed correctly. A no-op
  // empty report + 40 ms of settling time primes the BLE link and
  // macOS's event loop so the real sequence lands cleanly.
  writeReport(0, 0);
  delay(40);

  const uint8_t* p = (const uint8_t*)utf8;
  size_t typed = 0, skipped = 0;
  while (*p) {
    uint32_t cp = 0;
    int extra = 0;
    if (*p < 0x80) {
      cp = *p++;
    } else if ((*p & 0xE0) == 0xC0) {
      cp = *p++ & 0x1F; extra = 1;
    } else if ((*p & 0xF0) == 0xE0) {
      cp = *p++ & 0x0F; extra = 2;
    } else if ((*p & 0xF8) == 0xF0) {
      cp = *p++ & 0x07; extra = 3;
    } else {
      // Invalid UTF-8 lead byte — skip and resync.
      p++;
      skipped++;
      continue;
    }
    bool valid = true;
    for (int i = 0; i < extra; i++) {
      if ((*p & 0xC0) != 0x80) { valid = false; break; }
      cp = (cp << 6) | (*p++ & 0x3F);
    }
    if (!valid) { skipped++; continue; }
    if (cp > 0xFFFF) {
      // Outside BMP — Hex Input maxes out at 4 digits. Skip.
      skipped++;
      continue;
    }
    typeBmpCodepoint((uint16_t)cp);
    typed++;
  }
  Serial.printf("[hid] typed %u chars (%u skipped non-BMP/invalid)\n",
                (unsigned)typed, (unsigned)skipped);
}
