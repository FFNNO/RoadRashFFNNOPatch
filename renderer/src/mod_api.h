#pragma once
#include <cstdint>
#include "rr95_mod_api.h"

// Internal side of the mod API: the registration state and the pure decisions behind it, so
// hook_test can exercise every rule without the game process. hook_sites.cpp and presenter.cpp
// only translate their own data into these calls; the public contract is rr95_mod_api.h.
namespace rr95::modapi {

// One capability slot of version 1. Nothing registered -> Register (or Clear for a null value),
// already registered -> Reject, which the table reports as RR95_STATUS_REJECTED.
enum class SlotAction { Clear, Register, Reject };
SlotAction DecideSlot(bool occupied, bool nullValue);

// Item text detour: the general mod's own items keep their text, then the client row, then the
// client provider, then the game's own function. Every client source needs the relocated menu
// applied; a client row slot pointer only exists then. Client means "ask the provider; a null result
// falls back to Original".
enum class ItemTextSource { Owned, ClientRow, Client, Original };
ItemTextSource DecideItemTextSource(bool ownedItem, bool clientRowSlot, bool menuApplied,
                                    bool clientProviderRegistered);

// Overlay frames come from a client DLL, so every frame is checked before it is uploaded. One
// invalid frame is skipped for that frame only; the layer stays registered.
enum : int {
    kOverlayFrameMaxWidth = 640,  // the game image
    kOverlayFrameMaxHeight = 480
};
enum class OverlayFrameProblem { None = 0, NoPixels, Width, Height };
OverlayFrameProblem CheckOverlayFrame(const RR95OverlayFrame& frame);
bool IsValidOverlayFrame(const RR95OverlayFrame& frame);
const char* OverlayFrameProblemToString(OverlayFrameProblem problem);
// One log line per distinct failure: true the first time problem is seen, then false. None never
// logs and never sets a bit.
bool ShouldLogOverlayProblem(uint32_t& loggedMask, OverlayFrameProblem problem);

// What the presenter remembers of the last overlay upload.
struct OverlayUploadState {
    uint32_t counter = 0;
    int width = 0;
    int height = 0;
    bool uploaded = false;
    // The last attempt failed (texture creation or map). A failed attempt is not retried until the
    // change counter or the size changes, so a persistent failure does not retry every frame.
    bool failed = false;
};

// True when the layer texture must be uploaded again: first frame, changed counter or new size.
bool OverlayNeedsUpload(const OverlayUploadState& state, uint32_t counter, int width, int height);
void RecordOverlayUpload(OverlayUploadState& state, uint32_t counter, int width, int height);
void RecordOverlayUploadFailure(OverlayUploadState& state, uint32_t counter, int width, int height);

// Retry backoff after repeated consecutive overlay texture-creation failures
// (rr95-music-standalone S3a follow-up fdaae4f). A client may bump its change counter every frame,
// which asks for an upload every frame; a persistent CreateTexture2D failure must not run every
// frame. The first failure arms a one-second wait, and a success resets it.
inline constexpr uint64_t kOverlayRetryBackoffMs = 1000;

struct OverlayRetryState {
    uint32_t consecutiveFailures = 0;
    uint64_t nextAttemptMs = 0;
};

bool OverlayRetryDue(const OverlayRetryState& state, uint64_t nowMs);
void RecordOverlayRetryFailure(OverlayRetryState& state, uint64_t nowMs);
void RecordOverlayRetrySuccess(OverlayRetryState& state);

// One log line per distinct texture-creation failure code, so a persistent failure does not log on
// every frame. Up to four distinct codes are remembered; the oldest is replaced after that.
inline constexpr int kOverlayFailureLogCodes = 4;

struct OverlayFailureLog {
    uint32_t codes[kOverlayFailureLogCodes] = {};
    int count = 0;
    int next = 0;
};

bool ShouldLogOverlayFailure(OverlayFailureLog& log, uint32_t code);

// False after DLL_PROCESS_DETACH: no callback or provider may run then.
bool ShouldInvokeCallback(bool detached);

// Registration state. A client DLL drives one instance through the table; hook_test drives its
// own instances to check the rules.
struct State {
    void (*raceFrameCallback)(void* ctx) = nullptr;
    void* raceFrameCtx = nullptr;
    void (*menuFrameCallback)(void* ctx) = nullptr;
    void* menuFrameCtx = nullptr;
    const char* (*itemTextProvider)(const void* item, void* ctx) = nullptr;
    void* itemTextCtx = nullptr;
    const RR95OverlayFrame* (*overlaySource)(void* ctx) = nullptr;
    void* overlayCtx = nullptr;
    RR95MenuRowDesc menuRow = {};
    bool menuRowRegistered = false;
    // Set when the general mod has built its graphics menu or will never build it. No row can be
    // added afterwards.
    bool menuClosed = false;
};

int32_t RegisterRaceFrameCallback(State& state, void (*callback)(void* ctx), void* ctx);
int32_t RegisterMenuFrameCallback(State& state, void (*callback)(void* ctx), void* ctx);
int32_t AddGraphicsMenuRow(State& state, const RR95MenuRowDesc* row);
int32_t RegisterItemTextProvider(State& state, const char* (*provider)(const void* item, void* ctx), void* ctx);
int32_t SetOverlayLayer(State& state, const RR95OverlayFrame* (*source)(void* ctx), void* ctx);

// The client row text through the provider; empty while no row is registered, the provider returned
// null (the item footer never takes a null string), or the DLL is detaching (detached is the
// process detach flag, so the client callback is gated like every other entry point).
const char* GraphicsMenuRowText(const State& state, bool detached);
// Same through Global() and the process detach flag.
const char* GraphicsMenuRowText();

// The process-wide state used by the table and by the detours.
State& Global();

// Called by hook_sites when the graphics menu was built, or when it never will be.
void CloseGraphicsMenuRowSlot(State& state);
void CloseGraphicsMenuRowSlot();

// Called by hook_sites / presenter on the game thread. All no-ops without a client.
void OnRaceFrame();
void OnMenuFrame();
const char* ItemTextFor(const void* item);
const RR95OverlayFrame* OverlayFrame();
bool OverlayRegistered();
bool GraphicsMenuRowRegistered();

// Hook state: set by dllmain once the shared hooks are installed, failed or skipped.
void SetHookState(int32_t hookState);
int32_t HookState();

// Graphics menu state (rr95-music-standalone S2): set by hook_sites when the deferred build is
// requested, finished or never requested. The item-text flag follows the detour on FUN_0045b930.
void SetGraphicsMenuState(int32_t menuState);
int32_t GraphicsMenuState();

// Graphics menu state transitions (rr95-music-standalone S3a, review R3-002). The install decides
// PENDING or NOT_USED, the deferred build closes PENDING as APPLIED or FAILED, and a removal always
// leaves NOT_USED. A build event is ignored unless the state is PENDING.
enum class GraphicsMenuEvent { InstallWithMenu, InstallWithoutMenu, BuildApplied, BuildFailed, Removed };
int32_t NextGraphicsMenuState(int32_t current, GraphicsMenuEvent event);

// Result of the deferred build: the state and the item-text detour flag are set together, so a
// failed relocation rolls the detour back before any client can read it.
struct GraphicsMenuBuildOutcome {
    int32_t menuState = RR95_MENU_STATE_FAILED;
    bool itemTextDetourInstalled = false;
};
GraphicsMenuBuildOutcome DecideGraphicsMenuBuildOutcome(bool applied);
void SetItemTextDetourInstalled(bool installed);
bool ItemTextDetourInstalled();

// DLL_PROCESS_DETACH. No callback runs after this.
void SetDetached();

}  // namespace rr95::modapi
