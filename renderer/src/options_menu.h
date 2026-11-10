#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include "widescreen.h"

// "Pantalla panoramica" item in the graphics options menu (feature rr95-widescreen, task W3).
//
// Menu id 19 "Opciones Graficas y de Sonido" is a column of 0x38-byte items at 0x005a08a8 (count 8
// at 0x005a0a70). Slot 5 (0x005a09c0), between "Pantalla" (slot 4) and "Ya esta" (slot 6), ships
// empty: label 0, enabled byte 0, no handler, but with a rect (0, 374, 180, 48) and navigation bytes
// already pointing at slots 4 and 6. Item layout (out/decomp.c):
//   +0x00 HWND of the button window (written by FUN_0043a4e0 when a menu opens)
//   +0x04 id (FUN_0043ce30 returns it as the next menu when there is no handler)
//   +0x08 label string id (DAT_005cd49c[id]); FUN_0045b810/FUN_0045b890 (left/right arrows and
//         clicks on flag-0x100000 items) adjust a volume slider unless the label is 0xd0 or 0x3da
//   +0x0c x, +0x10 y, +0x14 width, +0x18 height
//   +0x1c left, +0x1d right, +0x1e up, +0x1f down, +0x20 Tab, +0x21 Shift+Tab: target slot indexes;
//         FUN_0043cea0 keeps following the chain past items whose enabled byte is 0
//   +0x24 flags: 0x100000 = text comes from FUN_0045b930(item) instead of DAT_005cd49c[label]
//   +0x28 enabled byte (EnableWindow in FUN_0043a630)
//   +0x2c select handler, int __fastcall (item in ECX), called by FUN_0043ce30; nonzero = next menu
//   +0x30 help string id (used only with flag 0x2), +0x34 0x4852 on every visible item
//
// The mod turns slot 5 into an on/off item: label 0x3da (so the arrow keys do not touch a volume),
// flag 0x100000 so FUN_0045b930 supplies the text (hooked: for this item it returns the label with
// the state), a mod select handler, and slot 4's down/Tab now lead to slot 5.

inline constexpr uint32_t kVaGraphicsMenuItems = 0x005a08a8;
inline constexpr uint32_t kVaGraphicsMenuCount = 0x005a0a70;
inline constexpr uint32_t kGraphicsMenuCount = 8;
inline constexpr size_t kMenuItemSize = 0x38;
inline constexpr int kGraphicsMenuFirstSlot = 4;   // "Pantalla"
inline constexpr int kGraphicsMenuWidescreenSlot = 5;
inline constexpr int kGraphicsMenuLastSlot = 6;    // "Ya esta"
inline constexpr uint32_t kVaGraphicsMenuSlots =
    kVaGraphicsMenuItems + kGraphicsMenuFirstSlot * static_cast<uint32_t>(kMenuItemSize);
inline constexpr uint32_t kVaWidescreenMenuItem =
    kVaGraphicsMenuItems + kGraphicsMenuWidescreenSlot * static_cast<uint32_t>(kMenuItemSize);
inline constexpr size_t kGraphicsMenuSlotsSize =
    (kGraphicsMenuLastSlot - kGraphicsMenuFirstSlot + 1) * kMenuItemSize;  // slots 4..6

inline constexpr uint32_t kVaPantallaSelectHandler = 0x0045b4b0;  // slot 4 handler (relocated)
inline constexpr uint32_t kWidescreenMenuLabelId = 0x3da;         // see +0x08 above
inline constexpr uint32_t kMenuFlagValueText = 0x100000;
inline constexpr uint32_t kGraphicsMenuButtonFlags = 0x0c;         // slot 4 flags

// The game's own on/off strings, "On" and "Off" (.rdata 0x005064a8 / 0x005064ac, not localized; the
// sound menu shows them for "Intercambiar altavoces" in the Spanish build).
inline constexpr uint32_t kVaOnOffStrings = 0x005064a8;
inline constexpr uint8_t kShippedOnOffStrings[8] = {'O', 'n', 0, 0, 'O', 'f', 'f', 0};

// True when slots 4..6 (kGraphicsMenuSlotsSize bytes from kVaGraphicsMenuSlots) hold exactly the
// shipped items, with slot 4's handler relocated by rebaseDelta (module base - 0x00400000). The
// HWND dword at +0x00 of each item is ignored (the engine fills it when a menu opens).
bool GraphicsMenuSlotsAreShipped(const uint8_t* slots, intptr_t rebaseDelta);

// The shipped bytes of slots 4..6 (HWND dwords zero) with slot 4's handler relocated by rebaseDelta.
void ShippedGraphicsMenuSlots(intptr_t rebaseDelta, uint8_t* out);

// Writes into out (kGraphicsMenuSlotsSize bytes) the slots with the widescreen item enabled: slot 5
// gets the label, flags, enabled byte, selectHandler and +0x34 (its rect and navigation bytes are
// kept: up = 4, down = 6), slot 4's down and Tab become 5. Slot 6 already goes up to 5.
void BuildWidescreenMenuSlots(const uint8_t* shipped, uint32_t selectHandler, uint8_t* out);

// Wide string to CP-1252 bytes (the 8-bit code page the engine's strings use: ESP.DLL holds them as
// UTF-16, LoadStringA converts them, e.g. 'o' acute = 0xF3 in "Resolucion"). Empty when a character
// has no CP-1252 byte.
std::string EncodeCp1252(const wchar_t* text);

// Item label "Pantalla panoramica" (a acute) in CP-1252.
std::string WidescreenMenuLabel();

// The item text. Menu 19 buttons are 180 px wide and FUN_00439680 draws the "\x01" value part at
// x = 300, outside them, so the state goes in the single label part: "<label>: <on|off>".
std::string WidescreenMenuText(bool on, const char* onText, const char* offText);

// Registry value HKCU\SOFTWARE\Electronic Arts\RoadRash 95\Widescreen, REG_BINARY 1 byte like the
// game's own HiRes/Inset (FUN_0045bd20): 1 = on (auto width), 0 = off.
inline constexpr const char* kGameRegistryKey = "SOFTWARE\\Electronic Arts\\RoadRash 95";
inline constexpr const char* kWidescreenRegistryValueName = "Widescreen";
inline constexpr int kWidescreenRegistryAbsent = -1;
inline constexpr int kWidescreenRegistryInvalid = -2;

// 0 or 1 for REG_BINARY of 1 byte or REG_DWORD of 4 bytes holding 0 or 1; kWidescreenRegistryInvalid
// for anything else.
int ParseWidescreenRegistryValue(uint32_t type, const uint8_t* data, uint32_t size);

enum class WidescreenSettingSource {
    Ini = 0,
    Registry = 1
};

struct EffectiveWidescreenSetting {
    WidescreenSetting setting;
    WidescreenSettingSource source = WidescreenSettingSource::Ini;
};

// registryValue 1 -> auto, 0 -> off (source Registry); absent or invalid -> the ini setting.
EffectiveWidescreenSetting ResolveEffectiveWidescreen(const WidescreenSetting& ini, int registryValue);

// "off", "auto" or the fixed width.
std::string DescribeWidescreenSetting(const WidescreenSetting& setting);
const char* WidescreenSettingSourceToString(WidescreenSettingSource source);

// Registry access (same key and API as the game). Read returns 0, 1, kWidescreenRegistryAbsent or
// kWidescreenRegistryInvalid; Write stores REG_BINARY 1 byte and returns false on failure.
int ReadWidescreenRegistryValue();
bool WriteWidescreenRegistryValue(bool on);
