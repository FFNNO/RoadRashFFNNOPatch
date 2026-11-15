#include "options_menu.h"

#include <windows.h>
#include <cstring>

#include "draw_distance.h"

static_assert(kFarViewOnSegments == rr95::kMaxDrawSegments, "far view on = the longest draw distance");
static_assert(kFarViewOffSegments == rr95::kEngineDrawSegments, "far view off = the engine's distance");

namespace {

constexpr size_t kOffId = 0x04;
constexpr size_t kOffLabel = 0x08;
constexpr size_t kOffY = 0x10;
constexpr size_t kOffWidth = 0x14;
constexpr size_t kOffHeight = 0x18;
constexpr size_t kOffNav = 0x1c;  // left, right, up, down, Tab, Shift+Tab
constexpr size_t kOffFlags = 0x24;
constexpr size_t kOffEnabled = 0x28;
constexpr size_t kOffHandler = 0x2c;
constexpr size_t kOffHelp = 0x30;
constexpr size_t kOffExtra = 0x34;
constexpr uint32_t kItemExtra = 0x4852;  // +0x34 on every visible menu 19 item
constexpr uint32_t kPreferredImageBase = 0x00400000;

// The 8 items of menu 19 as shipped in both exe builds (0x005a08a8..0x005a0a67), HWND dwords zero,
// handlers at the preferred base. renderer/tools/check_hook_sites.py compares this table with the exes.
constexpr uint8_t kShippedGraphicsMenuItems[kGraphicsMenuItemsSize] = {
    // 0 label 0x48
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x48, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x86, 0x00, 0x00, 0x00, 0xb4, 0x00, 0x00, 0x00, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x06, 0x01,
    0x01, 0x06, 0x00, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xc0, 0xb3, 0x45, 0x00,
    0x49, 0x00, 0x00, 0x00, 0x52, 0x48, 0x00, 0x00,
    // 1 label 0x18c
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x8c, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xb6, 0x00, 0x00, 0x00, 0xb4, 0x00, 0x00, 0x00, 0x30, 0x00, 0x00, 0x00, 0x01, 0x01, 0x00, 0x02,
    0x02, 0x00, 0x00, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0xb4, 0x45, 0x00,
    0x8d, 0x01, 0x00, 0x00, 0x52, 0x48, 0x00, 0x00,
    // 2 label 0x52, id 20 (no handler)
    0x00, 0x00, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00, 0x52, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xe6, 0x00, 0x00, 0x00, 0xb4, 0x00, 0x00, 0x00, 0x30, 0x00, 0x00, 0x00, 0x02, 0x02, 0x01, 0x03,
    0x03, 0x01, 0x00, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x53, 0x00, 0x00, 0x00, 0x52, 0x48, 0x00, 0x00,
    // 3 label 0x1af
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xaf, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x16, 0x01, 0x00, 0x00, 0xb4, 0x00, 0x00, 0x00, 0x30, 0x00, 0x00, 0x00, 0x03, 0x03, 0x02, 0x04,
    0x04, 0x02, 0x00, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x80, 0xb4, 0x45, 0x00,
    0xb0, 0x01, 0x00, 0x00, 0x52, 0x48, 0x00, 0x00,
    // 4 "Pantalla", label 0x3de
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xde, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x46, 0x01, 0x00, 0x00, 0xb4, 0x00, 0x00, 0x00, 0x30, 0x00, 0x00, 0x00, 0x04, 0x04, 0x03, 0x06,
    0x06, 0x03, 0x00, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xb0, 0xb4, 0x45, 0x00,
    0xdf, 0x03, 0x00, 0x00, 0x52, 0x48, 0x00, 0x00,
    // 5 empty, disabled
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x76, 0x01, 0x00, 0x00, 0xb4, 0x00, 0x00, 0x00, 0x30, 0x00, 0x00, 0x00, 0x05, 0x05, 0x04, 0x06,
    0x06, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    // 6 "Ya esta", id 16, flags 0x800c
    0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x56, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xa6, 0x01, 0x00, 0x00, 0xb4, 0x00, 0x00, 0x00, 0x3a, 0x00, 0x00, 0x00, 0x06, 0x06, 0x05, 0x00,
    0x00, 0x05, 0x00, 0x00, 0x0c, 0x80, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x57, 0x00, 0x00, 0x00, 0x52, 0x48, 0x00, 0x00,
    // 7 spacer, disabled
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x72, 0x00, 0x00, 0x00, 0xb4, 0x00, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

// Advance widths in pixels of CP-1252 bytes 0x20..0xff in the menu font, Futura Condensed Bold from
// Text/FUTB.DLL created like the game does (CreateFontA(24, 0, ..., 4, "Futura Condensed Bold")),
// measured with GetTextExtentPoint32A one byte at a time. The sum over a label equals GDI's extent
// of the whole label for every menu 19 label in the five languages (no kerning).
constexpr uint8_t kMenuFontAdvance[0x100 - 0x20] = {
    4, 6, 8, 10, 10, 17, 13, 5, 6, 6, 8, 10, 7, 7, 6, 11,
    10, 6, 10, 10, 10, 9, 10, 10, 10, 10, 6, 7, 11, 10, 10, 10,
    16, 11, 9, 10, 10, 8, 8, 12, 10, 5, 7, 10, 8, 14, 11, 12,
    10, 12, 10, 9, 9, 10, 11, 16, 10, 10, 10, 7, 11, 7, 7, 12,
    6, 9, 9, 7, 9, 9, 7, 9, 9, 6, 6, 9, 5, 13, 9, 9,
    9, 9, 7, 8, 7, 9, 9, 13, 9, 9, 8, 7, 4, 7, 9, 14,
    14, 0, 7, 10, 10, 17, 9, 6, 6, 14, 14, 14, 15, 0, 14, 0,
    0, 7, 7, 10, 10, 9, 9, 16, 6, 14, 14, 14, 14, 0, 14, 11,
    8, 6, 7, 11, 14, 11, 14, 10, 9, 16, 7, 11, 16, 14, 16, 14,
    6, 10, 14, 14, 5, 12, 11, 6, 14, 14, 8, 11, 14, 14, 14, 10,
    11, 11, 11, 11, 11, 11, 14, 10, 8, 8, 8, 8, 6, 6, 6, 6,
    14, 11, 12, 12, 12, 12, 12, 0, 12, 10, 10, 10, 10, 14, 14, 9,
    9, 9, 9, 9, 9, 9, 14, 7, 9, 8, 9, 9, 6, 6, 6, 6,
    14, 8, 9, 9, 9, 9, 9, 14, 9, 8, 8, 8, 9, 0, 14, 9,
};

// "Ya esta" keeps the shipped y 428 and 52-px height at both pitches: the 42-px rows 0..6 put it
// there without a client row, and the client row's 36-px pitch leaves the 6-px gap above it.
constexpr int kRelocatedMenuDoneY = kRelocatedMenuFirstY + kRelocatedMenuRowHeight * (kGraphicsMenuFarViewSlot + 1);

// Visible order 0..6, [client row], "Ya esta"; the spacer (slot 7) keeps its zero bytes. Navigation
// (left, right, up, down, Tab, Shift+Tab) follows this order with the shipped wrap.
int RelocatedVisibleSlotCount(bool withClientRow) {
    return withClientRow ? 9 : 8;
}

int RelocatedVisibleSlotAt(int index, bool withClientRow) {
    if (index < kGraphicsMenuSpacerSlot) {
        return index;
    }
    if (withClientRow && index == kGraphicsMenuSpacerSlot) {
        return kGraphicsMenuClientSlot;
    }
    return RelocatedGraphicsMenuDoneSlot(withClientRow);
}

void PutU32(uint8_t* p, uint32_t v) {
    std::memcpy(p, &v, sizeof(v));
}

uint32_t GetU32(const uint8_t* p) {
    uint32_t v = 0;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

uint8_t* At(uint8_t* module, uint32_t va) {
    return module + (va - kPreferredImageBase);
}

const uint8_t* At(const uint8_t* module, uint32_t va) {
    return module + (va - kPreferredImageBase);
}

intptr_t RebaseDeltaOf(const uint8_t* module) {
    return reinterpret_cast<intptr_t>(module) - static_cast<intptr_t>(kPreferredImageBase);
}

void MakeModItem(uint8_t* item, uint32_t handler) {
    PutU32(item + kOffId, 0);
    PutU32(item + kOffLabel, kModMenuItemLabelId);
    PutU32(item + kOffFlags, kGraphicsMenuButtonFlags | kMenuFlagValueText);
    PutU32(item + kOffEnabled, 1);
    PutU32(item + kOffHandler, handler);
    PutU32(item + kOffHelp, 0);
    PutU32(item + kOffExtra, kItemExtra);
}

}  // namespace

void ShippedGraphicsMenuItems(intptr_t rebaseDelta, uint8_t* out) {
    std::memcpy(out, kShippedGraphicsMenuItems, kGraphicsMenuItemsSize);
    for (size_t i = 0; i < kGraphicsMenuCount; ++i) {
        if (kGraphicsMenuHandlerVas[i] != 0) {
            PutU32(out + i * kMenuItemSize + kOffHandler,
                   static_cast<uint32_t>(static_cast<intptr_t>(kGraphicsMenuHandlerVas[i]) + rebaseDelta));
        }
    }
}

bool GraphicsMenuItemsAreShipped(const uint8_t* items, intptr_t rebaseDelta) {
    if (items == nullptr) {
        return false;
    }
    uint8_t expected[kGraphicsMenuItemsSize];
    ShippedGraphicsMenuItems(rebaseDelta, expected);
    for (size_t i = 0; i < kGraphicsMenuItemsSize; ++i) {
        if ((i % kMenuItemSize) < 4) {
            continue;  // HWND dword
        }
        if (items[i] != expected[i]) {
            return false;
        }
    }
    return true;
}

int RelocatedGraphicsMenuCount(bool withClientRow) {
    return withClientRow ? static_cast<int>(kRelocatedGraphicsMenuMaxCount)
                         : static_cast<int>(kRelocatedGraphicsMenuCount);
}

int RelocatedMenuPitch(bool withClientRow) {
    return withClientRow ? kRelocatedMenuClientRowHeight : kRelocatedMenuRowHeight;
}

int RelocatedGraphicsMenuDoneSlot(bool withClientRow) {
    return withClientRow ? kGraphicsMenuClientDoneSlot : kGraphicsMenuDoneSlot;
}

GraphicsMenuBuild BuildRelocatedGraphicsMenu(const uint8_t* shipped, uint32_t widescreenHandler,
                                             uint32_t farViewHandler, uint8_t* out, size_t capacity,
                                             bool withClientRow, uint32_t clientHandler) {
    const size_t needed = static_cast<size_t>(RelocatedGraphicsMenuCount(withClientRow)) * kMenuItemSize;
    if (out == nullptr || capacity < needed) {
        return GraphicsMenuBuild::NotEnoughSpace;
    }
    uint8_t items[kRelocatedGraphicsMenuMaxSize];
    std::memset(items, 0, sizeof(items));
    // Slots 0..5 and 7 keep their index; "Ya esta" moves past the far view row (and past the client
    // row when there is one); slot 6 starts as a copy of the empty slot 5 (rect width and x) and
    // becomes the far view item.
    std::memcpy(items, shipped, kGraphicsMenuItemsSize);
    std::memcpy(items + RelocatedGraphicsMenuDoneSlot(withClientRow) * kMenuItemSize,
                shipped + kShippedGraphicsMenuDoneSlot * kMenuItemSize, kMenuItemSize);
    std::memcpy(items + kGraphicsMenuFarViewSlot * kMenuItemSize, shipped + kGraphicsMenuWidescreenSlot * kMenuItemSize,
                kMenuItemSize);
    PutU32(items + kGraphicsMenuFarViewSlot * kMenuItemSize, 0);  // no HWND until the menu opens
    MakeModItem(items + kGraphicsMenuWidescreenSlot * kMenuItemSize, widescreenHandler);
    MakeModItem(items + kGraphicsMenuFarViewSlot * kMenuItemSize, farViewHandler);
    if (withClientRow) {
        PutU32(items + kGraphicsMenuClientSlot * kMenuItemSize, 0);  // no HWND until the menu opens
        MakeModItem(items + kGraphicsMenuClientSlot * kMenuItemSize, clientHandler);
    }
    const int pitch = RelocatedMenuPitch(withClientRow);
    const int doneSlot = RelocatedGraphicsMenuDoneSlot(withClientRow);
    const int visibleCount = RelocatedVisibleSlotCount(withClientRow);
    for (int i = 0; i < visibleCount; ++i) {
        const int slot = RelocatedVisibleSlotAt(i, withClientRow);
        if (slot == kGraphicsMenuSpacerSlot) {
            continue;
        }
        uint8_t* item = items + static_cast<size_t>(slot) * kMenuItemSize;
        const bool done = slot == doneSlot;
        PutU32(item + kOffY, static_cast<uint32_t>(done ? kRelocatedMenuDoneY : kRelocatedMenuFirstY + pitch * i));
        PutU32(item + kOffWidth, static_cast<uint32_t>(kRelocatedMenuItemWidth));
        PutU32(item + kOffHeight, static_cast<uint32_t>(done ? kRelocatedMenuDoneHeight : pitch));
        const uint8_t prev = static_cast<uint8_t>(RelocatedVisibleSlotAt((i + visibleCount - 1) % visibleCount, withClientRow));
        const uint8_t next = static_cast<uint8_t>(RelocatedVisibleSlotAt((i + 1) % visibleCount, withClientRow));
        const uint8_t slotByte = static_cast<uint8_t>(slot);
        const uint8_t nav[6] = {slotByte, slotByte, prev, next, next, prev};
        std::memcpy(item + kOffNav, nav, sizeof(nav));
    }
    std::memcpy(out, items, needed);
    return GraphicsMenuBuild::Ok;
}

int MenuLabelPixelWidth(const std::string& cp1252) {
    int width = 0;
    for (const char c : cp1252) {
        const uint8_t b = static_cast<uint8_t>(c);
        if (b >= 0x20) {
            width += kMenuFontAdvance[b - 0x20];
        }
    }
    return width;
}

const char* GraphicsMenuCheckToString(GraphicsMenuCheck check) {
    switch (check) {
    case GraphicsMenuCheck::Ok:
        return "ok";
    case GraphicsMenuCheck::StaticCount:
        return "item count at 0x5a0a70";
    case GraphicsMenuCheck::TableCount:
        return "menu table count";
    case GraphicsMenuCheck::TableItems:
        return "menu table item pointer";
    case GraphicsMenuCheck::Capacity:
        return "destination capacity";
    case GraphicsMenuCheck::Items:
    default:
        return "items";
    }
}

GraphicsMenuCheck CheckGraphicsMenu(const uint8_t* module) {
    const intptr_t delta = RebaseDeltaOf(module);
    if (GetU32(At(module, kVaGraphicsMenuCount)) != kGraphicsMenuCount) {
        return GraphicsMenuCheck::StaticCount;
    }
    const uint32_t tableCount = GetU32(At(module, kVaGraphicsMenuTableCount));
    if (tableCount != 0 && tableCount != kGraphicsMenuCount) {
        return GraphicsMenuCheck::TableCount;
    }
    if (GetU32(At(module, kVaGraphicsMenuTableItems)) !=
        static_cast<uint32_t>(static_cast<intptr_t>(kVaGraphicsMenuItems) + delta)) {
        return GraphicsMenuCheck::TableItems;
    }
    if (!GraphicsMenuItemsAreShipped(At(module, kVaGraphicsMenuItems), delta)) {
        return GraphicsMenuCheck::Items;
    }
    return GraphicsMenuCheck::Ok;
}

GraphicsMenuCheck RelocateGraphicsMenu(uint8_t* module, uint8_t* newItems, size_t newItemsCapacity,
                                       uint32_t widescreenHandler, uint32_t farViewHandler,
                                       GraphicsMenuRelocation& state, bool withClientRow, uint32_t clientHandler) {
    const GraphicsMenuCheck check = CheckGraphicsMenu(module);
    if (check != GraphicsMenuCheck::Ok || state.applied) {
        return check;
    }
    if (BuildRelocatedGraphicsMenu(At(module, kVaGraphicsMenuItems), widescreenHandler, farViewHandler, newItems,
                                   newItemsCapacity, withClientRow,
                                   clientHandler) != GraphicsMenuBuild::Ok) {
        return GraphicsMenuCheck::Capacity;
    }
    state.savedTableItems = GetU32(At(module, kVaGraphicsMenuTableItems));
    state.items = newItems;
    state.applied = true;
    PutU32(At(module, kVaGraphicsMenuTableItems), static_cast<uint32_t>(reinterpret_cast<uintptr_t>(newItems)));
    if (GetU32(At(module, kVaGraphicsMenuTableCount)) == kGraphicsMenuCount) {
        PutU32(At(module, kVaGraphicsMenuTableCount), static_cast<uint32_t>(RelocatedGraphicsMenuCount(withClientRow)));
    }
    PutU32(At(module, kVaGraphicsMenuCount), static_cast<uint32_t>(RelocatedGraphicsMenuCount(withClientRow)));
    return GraphicsMenuCheck::Ok;
}

void RestoreGraphicsMenu(uint8_t* module, GraphicsMenuRelocation& state) {
    if (!state.applied) {
        return;
    }
    PutU32(At(module, kVaGraphicsMenuCount), kGraphicsMenuCount);
    const uint32_t tableCount = GetU32(At(module, kVaGraphicsMenuTableCount));
    if (tableCount == kRelocatedGraphicsMenuCount || tableCount == kRelocatedGraphicsMenuMaxCount) {
        PutU32(At(module, kVaGraphicsMenuTableCount), kGraphicsMenuCount);
    }
    PutU32(At(module, kVaGraphicsMenuTableItems), state.savedTableItems);
    state.applied = false;
    state.items = nullptr;
}

void RequestGraphicsMenuInstall(GraphicsMenuInstall& state) {
    if (state.phase == GraphicsMenuPhase::Idle) {
        state.phase = GraphicsMenuPhase::Pending;
    }
}

bool TakeGraphicsMenuInstallRequest(GraphicsMenuInstall& state) {
    if (state.phase != GraphicsMenuPhase::Pending) {
        return false;
    }
    state.phase = GraphicsMenuPhase::Building;
    return true;
}

void FinishGraphicsMenuInstall(GraphicsMenuInstall& state, bool applied) {
    if (state.phase != GraphicsMenuPhase::Building) {
        return;
    }
    state.phase = applied ? GraphicsMenuPhase::Applied : GraphicsMenuPhase::Failed;
}

void ResetGraphicsMenuInstall(GraphicsMenuInstall& state) {
    state.phase = GraphicsMenuPhase::Idle;
}

bool GraphicsMenuRowSlotOpen(const GraphicsMenuInstall& state) {
    return state.phase == GraphicsMenuPhase::Pending || state.phase == GraphicsMenuPhase::Building;
}

std::string EncodeCp1252(const wchar_t* text) {
    if (text == nullptr || *text == L'\0') {
        return {};
    }
    BOOL usedDefault = FALSE;
    const int n = WideCharToMultiByte(1252, WC_NO_BEST_FIT_CHARS, text, -1, nullptr, 0, nullptr, &usedDefault);
    if (n <= 1 || usedDefault) {
        return {};
    }
    std::string out(static_cast<size_t>(n), '\0');
    usedDefault = FALSE;
    if (WideCharToMultiByte(1252, WC_NO_BEST_FIT_CHARS, text, -1, out.data(), n, nullptr, &usedDefault) != n ||
        usedDefault) {
        return {};
    }
    out.resize(static_cast<size_t>(n - 1));
    return out;
}

std::string WidescreenMenuLabel(uint32_t gameLanguage) {
    switch (gameLanguage) {
    case kGameLanguageFrench:
        return EncodeCp1252(L"\u00C9cran large");
    case kGameLanguageGerman:
        return EncodeCp1252(L"Breitbild");
    case kGameLanguageItalian:
        return EncodeCp1252(L"Schermo panoramico");
    case kGameLanguageSpanish:
        return EncodeCp1252(L"Pantalla panor\u00E1mica");
    default:
        return EncodeCp1252(L"Widescreen");
    }
}

std::string WidescreenMenuText(bool on, const char* onText, const char* offText, uint32_t gameLanguage) {
    std::string text = WidescreenMenuLabel(gameLanguage);
    text += ": ";
    const char* state = on ? onText : offText;
    text += state ? state : "";
    return text;
}

int ParseWidescreenRegistryValue(uint32_t type, const uint8_t* data, uint32_t size) {
    if (data == nullptr) {
        return kWidescreenRegistryInvalid;
    }
    uint32_t value = 0xFFFFFFFFu;
    if (type == REG_BINARY && size == 1) {
        value = data[0];
    } else if (type == REG_DWORD && size == 4) {
        std::memcpy(&value, data, 4);
    }
    if (value == 0 || value == 1) {
        return static_cast<int>(value);
    }
    return kWidescreenRegistryInvalid;
}

EffectiveWidescreenSetting ResolveEffectiveWidescreen(const WidescreenSetting& ini, int registryValue) {
    EffectiveWidescreenSetting e;
    if (registryValue == 0 || registryValue == 1) {
        e.setting.mode = registryValue == 1 ? WidescreenMode::Auto : WidescreenMode::Off;
        e.source = WidescreenSettingSource::Registry;
    } else {
        e.setting = ini;
        e.source = WidescreenSettingSource::Ini;
    }
    return e;
}

std::string DescribeWidescreenSetting(const WidescreenSetting& setting) {
    switch (setting.mode) {
    case WidescreenMode::Auto:
        return "auto";
    case WidescreenMode::Fixed:
        return std::to_string(setting.fixedWidth);
    case WidescreenMode::Off:
    default:
        return "off";
    }
}

const char* WidescreenSettingSourceToString(WidescreenSettingSource source) {
    return source == WidescreenSettingSource::Registry ? "registry" : "ini";
}

int ReadWidescreenRegistryValue() {
    return ReadOnOffRegistryValue(kWidescreenRegistryValueName);
}

bool WriteWidescreenRegistryValue(bool on) {
    return WriteOnOffRegistryValue(kWidescreenRegistryValueName, on);
}

int ReadOnOffRegistryValue(const char* valueName) {
    HKEY key = nullptr;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, kGameRegistryKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        return kWidescreenRegistryAbsent;
    }
    DWORD type = 0;
    uint8_t data[8] = {};
    DWORD size = sizeof(data);
    const LSTATUS status = RegQueryValueExA(key, valueName, nullptr, &type, data, &size);
    RegCloseKey(key);
    if (status == ERROR_FILE_NOT_FOUND) {
        return kWidescreenRegistryAbsent;
    }
    if (status != ERROR_SUCCESS) {
        return kWidescreenRegistryInvalid;
    }
    return ParseWidescreenRegistryValue(type, data, size);
}

bool WriteOnOffRegistryValue(const char* valueName, bool on) {
    // Same calls and access mask (0x2001f) as the game's FUN_0045bd20.
    HKEY key = nullptr;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, kGameRegistryKey, 0, nullptr, 0, 0x2001f, nullptr, &key, nullptr) !=
        ERROR_SUCCESS) {
        return false;
    }
    const BYTE value = on ? 1 : 0;
    const LSTATUS status = RegSetValueExA(key, valueName, 0, REG_BINARY, &value, 1);
    RegCloseKey(key);
    return status == ERROR_SUCCESS;
}

std::string FarViewMenuLabel(uint32_t gameLanguage) {
    switch (gameLanguage) {
    case kGameLanguageFrench:
        return EncodeCp1252(L"Vue lointaine");
    case kGameLanguageGerman:
        return EncodeCp1252(L"Fernsicht");
    case kGameLanguageItalian:
        return EncodeCp1252(L"Vista lontana");
    case kGameLanguageSpanish:
        return EncodeCp1252(L"Vista lejana");
    default:
        return EncodeCp1252(L"Far view");
    }
}

std::string FarViewMenuText(bool on, const char* onText, const char* offText, uint32_t gameLanguage) {
    std::string text = FarViewMenuLabel(gameLanguage);
    text += ": ";
    const char* state = on ? onText : offText;
    text += state ? state : "";
    return text;
}

EffectiveFarViewSetting ResolveEffectiveFarView(int iniDrawSegments, bool iniDistanceFade, int registryValue) {
    EffectiveFarViewSetting e;
    if (registryValue == 0 || registryValue == 1) {
        e.drawSegments = registryValue == 1 ? kFarViewOnSegments : kFarViewOffSegments;
        e.distanceFade = registryValue == 1;
        e.source = WidescreenSettingSource::Registry;
    } else {
        e.drawSegments = iniDrawSegments;
        e.distanceFade = iniDistanceFade;
        e.source = WidescreenSettingSource::Ini;
    }
    return e;
}

bool FarViewSettingIsOn(const EffectiveFarViewSetting& setting) {
    return setting.drawSegments > kFarViewOffSegments;
}
