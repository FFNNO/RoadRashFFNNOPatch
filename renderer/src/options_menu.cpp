#include "options_menu.h"

#include <windows.h>
#include <cstring>

namespace {

constexpr size_t kOffLabel = 0x08;
constexpr size_t kOffNavDown = 0x1f;
constexpr size_t kOffNavTab = 0x20;
constexpr size_t kOffFlags = 0x24;
constexpr size_t kOffEnabled = 0x28;
constexpr size_t kOffHandler = 0x2c;
constexpr size_t kOffHelp = 0x30;
constexpr size_t kOffExtra = 0x34;
constexpr uint32_t kItemExtra = 0x4852;  // +0x34 on every visible menu 19 item

// Slots 4..6 of menu 19 as shipped in both exe builds (0x005a0988..0x005a0a2f), HWND dwords zero,
// slot 4 handler at the preferred base.
constexpr uint8_t kShippedSlots[kGraphicsMenuSlotsSize] = {
    // slot 4 "Pantalla": label 0x3de, rect (0, 326, 180, 48), nav 04 04 03 06 06 03, flags 0xc,
    // enabled, handler 0x45b4b0, help 0x3df
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xde, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x46, 0x01, 0x00, 0x00, 0xb4, 0x00, 0x00, 0x00, 0x30, 0x00, 0x00, 0x00, 0x04, 0x04, 0x03, 0x06,
    0x06, 0x03, 0x00, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xb0, 0xb4, 0x45, 0x00,
    0xdf, 0x03, 0x00, 0x00, 0x52, 0x48, 0x00, 0x00,
    // slot 5: empty, rect (0, 374, 180, 48), nav 05 05 04 06 06 04
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x76, 0x01, 0x00, 0x00, 0xb4, 0x00, 0x00, 0x00, 0x30, 0x00, 0x00, 0x00, 0x05, 0x05, 0x04, 0x06,
    0x06, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    // slot 6 "Ya esta": id 16, label 0x56, rect (0, 422, 180, 58), nav 06 06 05 00 00 05, flags
    // 0x800c, enabled, no handler, help 0x57
    0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x56, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xa6, 0x01, 0x00, 0x00, 0xb4, 0x00, 0x00, 0x00, 0x3a, 0x00, 0x00, 0x00, 0x06, 0x06, 0x05, 0x00,
    0x00, 0x05, 0x00, 0x00, 0x0c, 0x80, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x57, 0x00, 0x00, 0x00, 0x52, 0x48, 0x00, 0x00,
};

void PutU32(uint8_t* p, uint32_t v) {
    std::memcpy(p, &v, sizeof(v));
}

}  // namespace

void ShippedGraphicsMenuSlots(intptr_t rebaseDelta, uint8_t* out) {
    std::memcpy(out, kShippedSlots, kGraphicsMenuSlotsSize);
    PutU32(out + kOffHandler,
           static_cast<uint32_t>(static_cast<intptr_t>(kVaPantallaSelectHandler) + rebaseDelta));
}

bool GraphicsMenuSlotsAreShipped(const uint8_t* slots, intptr_t rebaseDelta) {
    if (slots == nullptr) {
        return false;
    }
    uint8_t expected[kGraphicsMenuSlotsSize];
    ShippedGraphicsMenuSlots(rebaseDelta, expected);
    for (size_t i = 0; i < kGraphicsMenuSlotsSize; ++i) {
        if ((i % kMenuItemSize) < 4) {
            continue;  // HWND dword
        }
        if (slots[i] != expected[i]) {
            return false;
        }
    }
    return true;
}

void BuildWidescreenMenuSlots(const uint8_t* shipped, uint32_t selectHandler, uint8_t* out) {
    std::memmove(out, shipped, kGraphicsMenuSlotsSize);
    uint8_t* slot4 = out;
    uint8_t* slot5 = out + kMenuItemSize;
    slot4[kOffNavDown] = static_cast<uint8_t>(kGraphicsMenuWidescreenSlot);
    slot4[kOffNavTab] = static_cast<uint8_t>(kGraphicsMenuWidescreenSlot);
    PutU32(slot5 + 0x04, 0);
    PutU32(slot5 + kOffLabel, kWidescreenMenuLabelId);
    PutU32(slot5 + kOffFlags, kGraphicsMenuButtonFlags | kMenuFlagValueText);
    PutU32(slot5 + kOffEnabled, 1);
    PutU32(slot5 + kOffHandler, selectHandler);
    PutU32(slot5 + kOffHelp, 0);
    PutU32(slot5 + kOffExtra, kItemExtra);
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

std::string WidescreenMenuLabel() {
    return EncodeCp1252(L"Pantalla panor\u00E1mica");
}

std::string WidescreenMenuText(bool on, const char* onText, const char* offText) {
    std::string text = WidescreenMenuLabel();
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
    HKEY key = nullptr;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, kGameRegistryKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        return kWidescreenRegistryAbsent;
    }
    DWORD type = 0;
    uint8_t data[8] = {};
    DWORD size = sizeof(data);
    const LSTATUS status = RegQueryValueExA(key, kWidescreenRegistryValueName, nullptr, &type, data, &size);
    RegCloseKey(key);
    if (status == ERROR_FILE_NOT_FOUND) {
        return kWidescreenRegistryAbsent;
    }
    if (status != ERROR_SUCCESS) {
        return kWidescreenRegistryInvalid;
    }
    return ParseWidescreenRegistryValue(type, data, size);
}

bool WriteWidescreenRegistryValue(bool on) {
    // Same calls and access mask (0x2001f) as the game's FUN_0045bd20.
    HKEY key = nullptr;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, kGameRegistryKey, 0, nullptr, 0, 0x2001f, nullptr, &key, nullptr) !=
        ERROR_SUCCESS) {
        return false;
    }
    const BYTE value = on ? 1 : 0;
    const LSTATUS status = RegSetValueExA(key, kWidescreenRegistryValueName, 0, REG_BINARY, &value, 1);
    RegCloseKey(key);
    return status == ERROR_SUCCESS;
}
