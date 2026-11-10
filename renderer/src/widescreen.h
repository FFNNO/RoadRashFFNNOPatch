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

// Horizon across the full width (task W2).
//
// FUN_00469770 (decomp.c:58046-58115) blits the sky from two 8bpp system-memory surfaces,
// DAT_005cd4c4 and DAT_005cd4c8, each half (DAT_005ce344) of the panorama width DAT_005ce340 (720
// in HiRes, 360 otherwise; FUN_00468560, decomp.c:57285-57289). The panorama file rows hold the
// DAT_005cd4c4 row followed by the DAT_005cd4c8 row (decomp.c:57305-57313), so panorama column x is
// DAT_005cd4c4 column x for x < half and DAT_005cd4c8 column x - half otherwise. The blit covers
// DAT_005a2934 (640) columns starting at DAT_005cd4b4 and, with up to three BltFast calls, puts
// panorama column (d - scroll) mod width at blit column d. Rows [cropTop, DAT_006036a4) go to
// surface rows DAT_005cd4b8 + [cropTop, DAT_006036a4).
inline constexpr int kHorizonBlitWidth = 640;                          // DAT_005a2934 (HiRes)
inline constexpr int kMaxHorizonPanoramaWidth = 720;                   // DAT_005ce340 (HiRes)
inline constexpr int kMaxWideHorizonWidth = kMaxProjectionWidth * 2;   // 1280 source pixels

// The scroll FUN_0045ac50 passes to FUN_00469770 in ECX (0x45ace9..0x45ad06, same bytes in both
// exe builds): HiRes -(DAT_00612838 >> 15) (arithmetic), otherwise -(int16 at 0x0061283a), i.e.
// -(camera >> 16) on the high word. EDX, the vertical crop, is always 0 there.
int HorizonScrollFromCamera(int32_t camera, bool hiRes);

// Panorama column shown at engineX, a column relative to the blit origin DAT_005cd4b4 (it may lie
// outside [0, 640) for the widescreen sides): (engineX - scroll) mod panoramaWidth, in
// [0, panoramaWidth). panoramaWidth <= 0 gives -1.
int HorizonPanoramaColumn(int engineX, int scroll, int panoramaWidth);

// Engine column of column wideX of a wideWidth-wide horizon layer whose centred 640 columns are
// the software surface: wideX - (wideWidth - 640) / 2. A 640-wide layer is the surface itself.
int HorizonWideColumnToEngineX(int wideX, int wideWidth);

// Panorama row blitted to surface row surfaceY: surfaceY - blitY when surfaceY lies in
// [blitY + cropTop, blitY + panoramaHeight), otherwise -1 (the blit did not write that row).
int HorizonPanoramaRow(int surfaceY, int blitY, int cropTop, int panoramaHeight);

struct HorizonPanoramaView {
    const uint8_t* pixels = nullptr;  // width x height 8bpp palette indices
    int width = 0;
    int height = 0;
    int pitch = 0;
};

struct WideHorizonInput {
    // 640 x 480 horizon snapshot (pitch 640): surface rows [0, snapshotRows) as the engine left
    // them after the blit, rows past that repeat row snapshotRows - 1 (SnapshotHorizon...).
    const uint8_t* snapshot = nullptr;
    int snapshotRows = 0;
    HorizonPanoramaView panorama;  // pixels == nullptr: no panorama available
    int scroll = 0;                // FUN_00469770 param_1, unwrapped
    int blitY = 0;                 // DAT_005cd4b8
    int cropTop = 0;               // FUN_00469770 param_2
    int wideWidth = kSoftwareSurfaceWidth;
};

enum class WideHorizonResult {
    Invalid = 0,  // bad input; out untouched
    Copied,       // wideWidth == 640: out is the snapshot, byte for byte
    Panorama,     // centre = snapshot, sides continued from the panorama
    EdgeRepeat    // centre = snapshot, sides repeat the edge columns (the W1 look)
};

// Builds the wideWidth x 480 horizon layer (pitch wideWidth) into out. The centre 640 columns are
// always the snapshot. The sides come from the panorama only when every snapshot pixel the blit
// wrote equals the panorama pixel the mapping predicts for it (scroll and panorama verified against
// what the engine actually drew); otherwise they repeat the edge columns. wideWidth must be even
// and in [640, kMaxWideHorizonWidth].
WideHorizonResult ComposeWideHorizon(const WideHorizonInput& in, uint8_t* out);

// Runtime state: the projection width written at the last race start (320 when inactive).
// Written by the race-init detour on the game thread, read by capture and the presenter.
void WidescreenSetAppliedWidth(int width);
int WidescreenGetAppliedWidth();
bool WidescreenIsActive();
