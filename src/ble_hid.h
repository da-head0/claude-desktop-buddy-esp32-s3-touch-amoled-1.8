#pragma once
#include <stdint.h>

// HOGP (HID over GATT) Boot Keyboard, attached to the same BLEServer that
// hosts the NUS Hardware Buddy bridge. Pairing is shared — the LE Secure
// Connections bond made for NUS encrypts the HID characteristics too, so
// macOS only sees one pairing dance.
//
// Used by main.cpp to inject a "Right ⌘ Command twice" sequence (the
// macOS Dictation activation chosen by the user). The chip-level details
// of the device-side double-tap timing live in bleHidDoubleTapModifier().

class BLEServer;
class BLEAdvertising;

void bleHidAttach(BLEServer* server, BLEAdvertising* adv);
bool bleHidReady();

// Single keystroke: hold (modifier + keycode) for ~30 ms, then release.
// Modifier is the boot-keyboard modifier byte; keycode is a USB HID
// Usage Page 0x07 (Keyboard) ID. Pass keycode = 0 to send modifier-only.
void bleHidTap(uint8_t modifiers, uint8_t keycode);

// Two short modifier-only taps with an inter-tap gap that falls inside
// macOS's "double-tap modifier" detection window. Used for the
// Dictation activation "Press Right ⌘ Command twice".
void bleHidDoubleTapModifier(uint8_t modifier);

// Type a UTF-8 string by driving macOS's "Unicode Hex Input" layout:
// for each codepoint we hold Option, send four hex digits (BMP only —
// U+0000…U+FFFF, which covers Hangul U+AC00…U+D7AF and all common
// Latin/punctuation), then release Option. The host **must** have
// "Unicode Hex Input" selected as the active input source — with any
// other layout (US, Korean 두벌식, etc.), Option+hex emits unrelated
// glyphs (º¢∞ etc.). Non-BMP codepoints (some emoji) are skipped with
// a serial warning. Synchronous; ~80 ms per character.
void bleHidTypeUtf8(const char* utf8);

// Boot-keyboard modifier byte bits (USB HID 1.11 § 8.3 — keyboard input
// report byte 0). LCTRL/LSHIFT/LALT/LGUI are the "left" half; the upper
// nibble is the right half (RGUI = Right ⌘ Command on macOS).
static const uint8_t HID_MOD_LCTRL  = 0x01;
static const uint8_t HID_MOD_LSHIFT = 0x02;
static const uint8_t HID_MOD_LALT   = 0x04;   // Option on macOS
static const uint8_t HID_MOD_LGUI   = 0x08;   // Cmd on macOS
static const uint8_t HID_MOD_RCTRL  = 0x10;
static const uint8_t HID_MOD_RSHIFT = 0x20;
static const uint8_t HID_MOD_RALT   = 0x40;
static const uint8_t HID_MOD_RGUI   = 0x80;   // Right Cmd on macOS

// A few keycodes for occasional use; most flows use the modifier-only
// double-tap above.
static const uint8_t HID_KEY_F13 = 0x68;
// F23/F24 frame the PTT capture: F24 always selects Unicode Hex Input
// (so the board can type Option+hex Hangul) and F23 always selects the
// user's primary layout afterward. Two keys instead of one toggle so
// the result is independent of the input-source state when PTT begins.
static const uint8_t HID_KEY_F23 = 0x72;
static const uint8_t HID_KEY_F24 = 0x73;
