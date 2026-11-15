#pragma once
// Public mod API of the Road Rash 95 general mod (the proxy ddraw.dll).
//
// Self-contained on purpose: only <stdint.h> types and POD structs, valid in C and C++. An add-on
// DLL includes this file alone and resolves RR95_GetModApi with GetProcAddress on the loaded
// ddraw.dll (or by linking the import library).
//
// Contract:
//  - RR95_GetModApi(version) returns a pointer to a static table for a supported version, null for
//    any other version. The table starts with version and size so later versions can append
//    function pointers; a client must check version and size before using a field.
//  - Version 1 was unreleased while the music mod was developed, so fields were appended to it
//    (getMenuState and isItemTextDetourInstalled) instead of moving to a new version. The field
//    order of the original version 1 prefix is unchanged.
//  - Version 1 has one client slot per capability. A second registration of the same capability is
//    rejected with RR95_STATUS_REJECTED. A null callback clears that capability
//    (RR95_STATUS_OK) while its slot is still open.
//  - Callbacks and providers run on the game thread only, from the general mod's own detours, and
//    never after the general mod's DLL_PROCESS_DETACH.
//  - Register on the game thread. The general mod installs its hooks on the game's first call to
//    the proxy's DirectDrawCreate and builds its graphics menu at the game's first menu frame, so a
//    client that binds from the first game call into its own proxy export (for example its
//    DirectSoundCreate forwarding) is early enough for everything, including the graphics menu
//    row. Never bind from DllMain: it runs under the loader lock.
//  - When no client registers anything, the general mod behaves exactly as without this API.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Version of the table this header describes.
enum { RR95_MOD_API_VERSION = 1 };

// Status codes returned by the version 1 functions.
enum {
    RR95_STATUS_OK = 0,               // done
    RR95_STATUS_REJECTED = 1,         // the capability already has a client
    RR95_STATUS_INVALID_ARGUMENT = 2, // missing handler, text provider or descriptor
    RR95_STATUS_TOO_LATE = 3          // the graphics menu was already built
};

// Values returned by RR95ModApi::getHookState.
enum {
    RR95_HOOK_STATE_NOT_INSTALLED = 0, // the general mod has not installed its hooks yet
    RR95_HOOK_STATE_ACTIVE = 1,        // the shared hooks are installed and will call the client
    RR95_HOOK_STATE_FAILED = 2         // the shared hooks are not installed and never will be
};

// Values returned by RR95ModApi::getMenuState. The general mod relocates graphics menu 19 and
// installs its item-text detour only with GpuWorld and the presenter on; a client uses the API for
// the menu row and item text only when this reports PENDING or APPLIED.
enum {
    RR95_MENU_STATE_NOT_USED = 0, // the general mod will not relocate the menu
    RR95_MENU_STATE_PENDING = 1,  // the deferred build is requested, first menu frame will run it
    RR95_MENU_STATE_APPLIED = 2,  // the menu is relocated
    RR95_MENU_STATE_FAILED = 3,   // the deferred build refused; the menu stays shipped
    // Appended in version 1 (unreleased): no install decision has run yet. A client that binds
    // before the general mod's InstallHooks must wait instead of installing its own menu hooks.
    RR95_MENU_STATE_UNKNOWN = 4
};

// One overlay frame, in the client's own pixels.
typedef struct RR95OverlayFrame {
    // BGRA, premultiplied, row-major, width * height pixels. The pointer must stay valid until the
    // provider returns from the next call; the presenter copies it while it draws.
    const uint32_t* pixels;
    int32_t width;  // pixels, 1..640
    int32_t height; // pixels, 1..480
    int32_t x;      // left edge in 640x480 game coordinates
    int32_t y;      // top edge in 640x480 game coordinates
    // Increment whenever the pixels change. The presenter uploads the texture only when this
    // counter (or the size) changes.
    uint32_t changeCounter;
} RR95OverlayFrame;

// One extra row for the general mod's graphics options menu, placed after its own rows and before
// "Ya esta". The row always uses the engine's value-text flag (0x100000), so its label comes from
// the general mod's item text detour, which calls text(ctx).
typedef struct RR95MenuRowDesc {
    // Address of an int __fastcall(void* item) select handler, written into the item's +0x2c
    // handler field exactly like the game's own handlers (the item pointer is in ECX).
    uint32_t selectHandler;
    // Row label in the game's ANSI code page; null is not allowed for a registration. Returned
    // text must stay valid until the next call (a static buffer is fine). A null return draws an
    // empty label.
    const char* (*text)(void* ctx);
    void* ctx;
} RR95MenuRowDesc;

typedef struct RR95ModApi {
    uint32_t version; // RR95_MOD_API_VERSION
    uint32_t size;    // sizeof(RR95ModApi) at build time

    // Called once per race frame, from the race present detour, before the frame is presented.
    // Only the last non-null registration is kept for that slot in version 1.
    int32_t (*registerRaceFrameCallback)(void (*callback)(void* ctx), void* ctx);

    // The same once per menu frame from the menu present detour.
    int32_t (*registerMenuFrameCallback)(void (*callback)(void* ctx), void* ctx);
    // Adds the one client row to the relocated graphics menu. The row appears only when the
    // general mod relocates menu 19 (GpuWorld and its D3D11 presenter enabled). The call must
    // arrive before the general mod builds that menu, at the game's first menu frame; afterwards
    // it returns RR95_STATUS_TOO_LATE. A null descriptor clears the pending row.
    int32_t (*addGraphicsMenuRow)(const RR95MenuRowDesc* row);
    // Text for menu items the general mod does not own (its own two rows keep their text). Asked
    // first inside the item text detour; a non-null return is used and a null return falls back to
    // the game's own function. Only called while the general mod's item text detour is installed
    // (with the graphics menu, so GpuWorld and the presenter must be on).
    int32_t (*registerItemTextProvider)(const char* (*provider)(const void* item, void* ctx), void* ctx);
    // One overlay layer above the HUD and below the save/load toast, drawn on every presented
    // frame, scaled and letterboxed like the game image. A null source clears the layer. The
    // source runs on the game thread once per presented frame; a null return draws nothing, and a
    // frame with null pixels or a size outside 1..640 by 1..480 is skipped for that frame only
    // (the layer stays registered).
    int32_t (*setOverlayLayer)(const RR95OverlayFrame* (*source)(void* ctx), void* ctx);
    // RR95_HOOK_STATE_*: whether the general mod's shared hooks are or will ever be installed. A
    // client may install its own hooks when this says failed.
    int32_t (*getHookState)(void);
    // RR95_MENU_STATE_*: whether the general mod owns the graphics menu and its item-text site.
    // NOT_USED means the client may install its own hooks for the in-place menu row and item text.
    // Appended in version 1 (unreleased): a client must check size before calling it.
    int32_t (*getMenuState)(void);
    // Nonzero while the general mod's item-text detour on FUN_0045b930 is installed.
    // Appended in version 1 (unreleased): a client must check size before calling it.
    int32_t (*isItemTextDetourInstalled)(void);
} RR95ModApi;

// The ddraw.dll export. Returns null for versions other than RR95_MOD_API_VERSION. The returned
// table is static and lives as long as the module.
const RR95ModApi* RR95_GetModApi(uint32_t version);

#ifdef __cplusplus
} // extern "C"
#endif
