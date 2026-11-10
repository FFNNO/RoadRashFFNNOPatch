#pragma once
#include <cstdint>

// Widescreen race view (feature rr95-widescreen, task W1).
//
// The engine projects and culls the world for a single projection width, DAT_00591c0c (320 in the
// shipped exe; screen centre X = width / 2, object and road culls test against width << 16), and
// race init FUN_00457ef0 sets the clip rect to (0, 0, width << HiRes, (240 << HiRes) - 1) from it.
// DAT_00591c04 (also 320) feeds the clip rect set at startup by FUN_0046d620. Widening the
// projection width before race init widens the horizontal field of view with the same vertical
// scale. The software surface stays 640x480, so a wider clip rect is only safe while the software
// world rasterizer FUN_0042e850 and the flat fill FUN_00467cb0 never run (GPU world only).

enum class WidescreenMode {
    Off = 0,
    Auto = 1,
    Fixed = 2
};

// [World] Widescreen = off | auto | <even integer 320..640>.
struct WidescreenSetting {
    WidescreenMode mode = WidescreenMode::Off;
    int fixedWidth = 320;  // projection width for WidescreenMode::Fixed
};

inline constexpr int kEngineProjectionWidth = 320;  // shipped DAT_00591c0c / DAT_00591c04
inline constexpr int kMaxProjectionWidth = 640;      // 32:9 at 1280 HiRes source pixels
inline constexpr int kSoftwareSurfaceWidth = 640;    // DAT_005cd4b0 8bpp surface (HiRes)
inline constexpr int kSoftwareSurfaceHeight = 480;

// Parses the ini value (case-insensitive, surrounding spaces ignored). Returns false for anything
// that is not off, auto, or an even integer in [320, 640]; out is then set to Off.
bool ParseWidescreenSetting(const wchar_t* str, WidescreenSetting& out);

// auto: 2 * round(120 * clientW / clientH) (halves round up), i.e. round(240 * aspect) made even,
// clamped to [320, 640]. 1920x1080 -> 426, 4:3 -> 320, 21:9 -> 560. An invalid size gives 320.
int DeriveAutoProjectionWidth(int clientW, int clientH);

enum class WidescreenReason {
    Applied = 0,       // width > 320 applied
    SettingOff,        // [World] Widescreen=off, or auto resolved to 320 (4:3 or narrower window)
    GpuWorldOff,       // GpuWorld off, presenter off, latched off, or world hooks missing
    LowRes,            // HiRes flag DAT_005a292a is 0
    Inset,             // low-res inset mode DAT_005cd47c is set
    NoWindowSize       // auto without a valid client size
};

struct WidescreenResolution {
    int width = kEngineProjectionWidth;
    WidescreenReason reason = WidescreenReason::SettingOff;
};

// The projection width a race starts with. Anything other than Applied is 320.
WidescreenResolution ResolveRaceProjectionWidth(const WidescreenSetting& setting,
                                                bool gpuWorldEffective,
                                                bool hiRes,
                                                bool inset,
                                                int clientW,
                                                int clientH);
const char* WidescreenReasonToString(WidescreenReason reason);

// True whenever a CPU write by the world rasterizers could leave the 640-wide surface: a widened
// projection width is applied for this race, or the live clip rect right edge (DAT_006036c8,
// half-open) lies past the surface.
bool SoftwareWorldWritesForbidden(int appliedProjectionWidth, int32_t clipRight);

// Presenter mapping of the GPU world frame.
// - Off (projectionWidth <= 320): world source 640x480 drawn into internalW x internalH, software
//   layers over the whole target (the B4..B6 path, unchanged).
// - Widescreen: world source (projectionWidth << 1) x 480 drawn into a target of height internalH
//   and width internalH * srcW / 480, or the window-aspect width internalH * clientW / clientH when
//   that is within 1% (so a 16:9 window is filled without bars), made even. The 640-wide software
//   layers (horizon snapshot, HUD, dashboard) cover the target X range that the centred 640 source
//   columns map to: [round(((srcW - 640) / 2) * targetW / srcW), round(((srcW + 640) / 2) * targetW / srcW)).
struct WorldTargetLayout {
    bool widescreen = false;
    int worldSrcW = kSoftwareSurfaceWidth;
    int worldSrcH = kSoftwareSurfaceHeight;
    int targetW = 0;
    int targetH = 0;
    int layerLeft = 0;
    int layerRight = 0;
};

WorldTargetLayout ComputeWorldTargetLayout(int projectionWidth,
                                           int internalW,
                                           int internalH,
                                           int clientW,
                                           int clientH);

// Runtime state: the projection width written at the last race start (320 when inactive).
// Written by the race-init detour on the game thread, read by capture and the presenter.
void WidescreenSetAppliedWidth(int width);
int WidescreenGetAppliedWidth();
bool WidescreenIsActive();
