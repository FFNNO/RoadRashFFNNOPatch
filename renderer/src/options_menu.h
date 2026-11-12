#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include "widescreen.h"

// Mod items in the graphics options menu: widescreen ("Pantalla panoramica" in Spanish; feature
// rr95-widescreen, task W3; labelled in the game's language since rr95-gpu-hud-icons I4) and far
// view ("Vista lejana"; feature rr95-draw-distance, task D4).
//
// Menu id 19 "Opciones Graficas y de Sonido" is a column of 0x38-byte items at 0x005a08a8. The menu
// table at DAT_00594558 holds 0x14 bytes per menu id: +0x00 item count, +0x04 item array pointer,
// +0x08 title string id, +0x10 last selected index. FUN_0043ce30's menu open (decomp 30622-30636)
// reads the count and the pointer each time a menu opens; the window procedure's WM_CREATE copies
// the count of menu 19 from 0x005a0a70 (8) into the table once (decomp 32052). The only absolute
// reference to the item array is the table's pointer 0x005946d8 (the .reloc section has no other
// relocation into 0x005a08a8..0x005a0a68), so the mod points the table at a copy in this DLL with a
// ninth item. Shipped items (label; rect x, y, w, h; nav left right up down Tab Shift+Tab):
//   0 0x48  (0, 134, 180, 48) 00 00 06 01 01 06   4 0x3de "Pantalla" (0, 326, 180, 48) 04 04 03 06 06 03
//   1 0x18c (0, 182, 180, 48) 01 01 00 02 02 00   5 empty, disabled  (0, 374, 180, 48) 05 05 04 06 06 04
//   2 0x52  (0, 230, 180, 48) 02 02 01 03 03 01   6 0x56 "Ya esta"   (0, 422, 180, 58) 06 06 05 00 00 05
//   3 0x1af (0, 278, 180, 48) 03 03 02 04 04 02   7 spacer, disabled (0, 114, 180, 20) 00 00 00 00 00 00
// Item layout (out/decomp.c):
//   +0x00 HWND of the button window (written by the menu open for every item)
//   +0x04 id (FUN_0043ce30 returns it as the next menu when there is no handler)
//   +0x08 label string id (DAT_005cd49c[id]); FUN_0045b810/FUN_0045b890 (left/right arrows and
//         clicks on flag-0x100000 items) adjust a volume slider unless the label is 0xd0 or 0x3da
//   +0x0c x, +0x10 y, +0x14 width, +0x18 height
//   +0x1c left, +0x1d right, +0x1e up, +0x1f down, +0x20 Tab, +0x21 Shift+Tab: target slot indexes;
//         FUN_0043cea0 keeps following the chain past items whose enabled byte is 0
//   +0x24 flags: 0x100000 = text comes from FUN_0045b930(item) instead of DAT_005cd49c[label];
//         0x8000 = the item Escape selects (found by a scan, not by index)
//   +0x28 enabled byte (EnableWindow in FUN_0043a630)
//   +0x2c select handler, int __fastcall (item in ECX), called by FUN_0043ce30; nonzero = next menu
//   +0x30 help string id (used only with flag 0x2), +0x34 0x4852 on every visible item
//
// The relocated menu (9 items) restacks the visible rows on the 42-px pitch of the game's own sound
// menu 17 (rows from y 134, the last one 52 high, ending at 480), so one more row fits:
//   0..4 as shipped, at y 134 + 42 i, height 42
//   5 widescreen (y 344): label 0x3da, flag 0x100000 (text from the hooked FUN_0045b930), mod handler
//   6 far view (y 386): the same kind of item
//   7 the shipped spacer, unchanged
//   8 "Ya esta" (y 428, height 52), moved from slot 6 so that End (last enabled index) still lands
//     on it
// Navigation: 0 <-> 1 <-> 2 <-> 3 <-> 4 <-> 5 <-> 6 <-> 8, and 8 -> 0 / 0 -> 8 wrapping like the
// shipped menu (down/Tab forward, up/Shift+Tab back); left/right stay on the item.

inline constexpr uint32_t kVaGraphicsMenuItems = 0x005a08a8;
inline constexpr uint32_t kVaGraphicsMenuCount = 0x005a0a70;       // copied into the table at WM_CREATE
inline constexpr uint32_t kVaGraphicsMenuTableCount = 0x005946d4;  // DAT_00594558 + 19 * 0x14
inline constexpr uint32_t kVaGraphicsMenuTableItems = 0x005946d8;  // the array's only relocation
inline constexpr uint32_t kGraphicsMenuCount = 8;
inline constexpr uint32_t kRelocatedGraphicsMenuCount = 9;
inline constexpr size_t kMenuItemSize = 0x38;
inline constexpr size_t kGraphicsMenuItemsSize = kGraphicsMenuCount * kMenuItemSize;
inline constexpr size_t kRelocatedGraphicsMenuSize = kRelocatedGraphicsMenuCount * kMenuItemSize;
inline constexpr int kGraphicsMenuWidescreenSlot = 5;
inline constexpr int kGraphicsMenuFarViewSlot = 6;
inline constexpr int kGraphicsMenuSpacerSlot = 7;
inline constexpr int kGraphicsMenuDoneSlot = 8;          // "Ya esta" in the relocated menu
inline constexpr int kShippedGraphicsMenuDoneSlot = 6;   // "Ya esta" as shipped
inline constexpr int kRelocatedMenuFirstY = 134;
inline constexpr int kRelocatedMenuRowHeight = 42;       // menu 17's pitch
inline constexpr int kRelocatedMenuDoneHeight = 52;      // menu 17's last row

// Select handlers of the shipped items at the preferred base (relocated by the loader), 0 = none.
inline constexpr uint32_t kGraphicsMenuHandlerVas[kGraphicsMenuCount] = {
    0x0045b3c0, 0x0045b400, 0, 0x0045b480, 0x0045b4b0, 0, 0, 0};
inline constexpr uint32_t kModMenuItemLabelId = 0x3da;   // see +0x08 above
inline constexpr uint32_t kWidescreenMenuLabelId = kModMenuItemLabelId;
inline constexpr uint32_t kMenuFlagValueText = 0x100000;
inline constexpr uint32_t kGraphicsMenuButtonFlags = 0x0c;  // flags of the shipped buttons

// The shipped items (HWND dwords zero) with the select handlers relocated by rebaseDelta (module
// base - 0x00400000).
void ShippedGraphicsMenuItems(intptr_t rebaseDelta, uint8_t* out);

// True when the kGraphicsMenuItemsSize bytes at items are exactly the shipped items, ignoring the
// HWND dword at +0x00 of each item (the engine fills it when a menu opens).
bool GraphicsMenuItemsAreShipped(const uint8_t* items, intptr_t rebaseDelta);

// Writes the relocated menu (kRelocatedGraphicsMenuSize bytes, layout above) built from the live
// shipped items (their HWND dwords are copied as they are).
void BuildRelocatedGraphicsMenu(const uint8_t* shipped, uint32_t widescreenHandler, uint32_t farViewHandler,
                                uint8_t* out);

// Result of checking the engine's menu 19 before relocating it.
enum class GraphicsMenuCheck {
    Ok = 0,
    StaticCount,  // 0x005a0a70 is not 8
    TableCount,   // the table count is neither 0 (before WM_CREATE) nor 8
    TableItems,   // the table pointer is not the relocated 0x005a08a8
    Items         // the item bytes differ from the shipped ones
};
const char* GraphicsMenuCheckToString(GraphicsMenuCheck check);

// module is the image base the engine runs at (VA v lives at module + v - 0x00400000), so tests can
// pass a fake image. Reads the two counts, the table pointer and the item array.
GraphicsMenuCheck CheckGraphicsMenu(const uint8_t* module);

struct GraphicsMenuRelocation {
    bool applied = false;
    uint32_t savedTableItems = 0;  // the engine's pointer, written back on restore
    uint8_t* items = nullptr;      // the relocated array (caller-owned, kRelocatedGraphicsMenuSize)
};

// Checks the menu and, when it is the shipped one, builds the relocated array into newItems and
// points the engine at it: table pointer first, then the counts (the table count only when WM_CREATE
// has already copied it, 0x005a0a70 always), so a menu opening in between never reads 9 items from
// the 8-item array. Nothing is written unless the check is Ok.
GraphicsMenuCheck RelocateGraphicsMenu(uint8_t* module, uint8_t* newItems, uint32_t widescreenHandler,
                                       uint32_t farViewHandler, GraphicsMenuRelocation& state);

// Restores the shipped menu: counts back to 8 first (the table count only while it is 9), then the
// engine's pointer. No-op unless applied.
void RestoreGraphicsMenu(uint8_t* module, GraphicsMenuRelocation& state);

// The game's own on/off strings, "On" and "Off" (.rdata 0x005064a8 / 0x005064ac, not localized; the
// sound menu shows them for "Intercambiar altavoces" in the Spanish build).
inline constexpr uint32_t kVaOnOffStrings = 0x005064a8;
inline constexpr uint8_t kShippedOnOffStrings[8] = {'O', 'n', 0, 0, 'O', 'f', 'f', 0};

// Wide string to CP-1252 bytes (the 8-bit code page the engine's strings use: ESP.DLL holds them as
// UTF-16, LoadStringA converts them, e.g. 'o' acute = 0xF3 in "Resolucion"). Empty when a character
// has no CP-1252 byte.
std::string EncodeCp1252(const wchar_t* text);

// The game's language: DAT_00595968, set at startup from the HKLM\SOFTWARE\Electronic Arts\
// RoadRash 95 "Language" value before Text_ENU/FRA/DEU/ITA/ESP.dll is loaded. It is the string id
// base the engine adds to LoadStringA ids; any other language name gives 0xffffffff.
inline constexpr uint32_t kVaGameLanguage = 0x00595968;
inline constexpr uint32_t kGameLanguageEnglish = 0x00;
inline constexpr uint32_t kGameLanguageFrench = 0x10;
inline constexpr uint32_t kGameLanguageGerman = 0x20;
inline constexpr uint32_t kGameLanguageItalian = 0x30;
inline constexpr uint32_t kGameLanguageSpanish = 0x40;

// Item label in the game's language, CP-1252: "Widescreen" (English and unknown values),
// "Ecran large" (E acute), "Breitbild", "Schermo panoramico", "Pantalla panoramica" (a acute).
// Kept short: the 180 px button clips longer text.
std::string WidescreenMenuLabel(uint32_t gameLanguage);

// The item text. Menu 19 buttons are 180 px wide and FUN_00439680 draws the "\x01" value part at
// x = 300, outside them, so the state goes in the single label part: "<label>: <on|off>".
std::string WidescreenMenuText(bool on, const char* onText, const char* offText, uint32_t gameLanguage);

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

// Registry access for any 0/1 value under kGameRegistryKey (same API and REG_BINARY 1 byte as the
// widescreen value). Read returns 0, 1, kWidescreenRegistryAbsent or kWidescreenRegistryInvalid.
int ReadOnOffRegistryValue(const char* valueName);
bool WriteOnOffRegistryValue(const char* valueName, bool on);

// Far view item (feature rr95-draw-distance, task D4): on = the longer road draw distance with the
// distance fade, off = the engine's 48 segments with no fade. KeepThinQuads is a bug fix and stays
// controlled only by the ini.
inline constexpr const char* kFarViewRegistryValueName = "FarView";
inline constexpr int kFarViewOnSegments = 72;   // rr95::kMaxDrawSegments
inline constexpr int kFarViewOffSegments = 48;  // rr95::kEngineDrawSegments

// Label in the game's language, CP-1252: "Far view" (English and unknown values), "Vue lointaine",
// "Fernsicht", "Vista lontana", "Vista lejana".
std::string FarViewMenuLabel(uint32_t gameLanguage);
// "<label>: <on|off>" like WidescreenMenuText.
std::string FarViewMenuText(bool on, const char* onText, const char* offText, uint32_t gameLanguage);

struct EffectiveFarViewSetting {
    int drawSegments = kFarViewOffSegments;
    bool distanceFade = false;
    WidescreenSettingSource source = WidescreenSettingSource::Ini;
};

// registryValue 1 -> 72 segments with the fade, 0 -> 48 without it (source Registry); absent or
// invalid -> the ini's [World] DrawSegments and DistanceFade (source Ini).
EffectiveFarViewSetting ResolveEffectiveFarView(int iniDrawSegments, bool iniDistanceFade, int registryValue);

// What the menu shows: on when the setting draws more than the engine's 48 segments.
bool FarViewSettingIsOn(const EffectiveFarViewSetting& setting);
