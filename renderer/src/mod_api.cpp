#include "mod_api.h"

namespace rr95::modapi {

namespace {

State g_state;

int32_t g_hookState = RR95_HOOK_STATE_NOT_INSTALLED;
// Unknown until InstallHooks requests or refuses the deferred build, so an early client waits
// instead of taking a site the general mod may still claim.
int32_t g_menuState = RR95_MENU_STATE_UNKNOWN;
bool g_itemTextDetourInstalled = false;

// Set by DLL_PROCESS_DETACH. Every entry point checks it before touching client code.
bool g_detached = false;

// One capability slot: a null value clears it, a second non-null registration is rejected.
template <typename T>
int32_t RegisterOne(T& slot, void*& ctx, T value, void* newCtx) {
    switch (DecideSlot(slot != nullptr, value == nullptr)) {
    case SlotAction::Reject:
        return RR95_STATUS_REJECTED;
    case SlotAction::Clear:
        slot = nullptr;
        ctx = nullptr;
        return RR95_STATUS_OK;
    case SlotAction::Register:
    default:
        slot = value;
        ctx = newCtx;
        return RR95_STATUS_OK;
    }
}

int32_t ApiRegisterRaceFrame(void (*callback)(void* ctx), void* ctx) {
    return RegisterRaceFrameCallback(Global(), callback, ctx);
}

int32_t ApiRegisterMenuFrame(void (*callback)(void* ctx), void* ctx) {
    return RegisterMenuFrameCallback(Global(), callback, ctx);
}

int32_t ApiAddGraphicsMenuRow(const RR95MenuRowDesc* row) {
    return AddGraphicsMenuRow(Global(), row);
}

int32_t ApiRegisterItemText(const char* (*provider)(const void* item, void* ctx), void* ctx) {
    return RegisterItemTextProvider(Global(), provider, ctx);
}

int32_t ApiSetOverlay(const RR95OverlayFrame* (*source)(void* ctx), void* ctx) {
    return SetOverlayLayer(Global(), source, ctx);
}

int32_t ApiGetMenuState() {
    return GraphicsMenuState();
}

int32_t ApiIsItemTextDetourInstalled() {
    return ItemTextDetourInstalled() ? 1 : 0;
}

}  // namespace

SlotAction DecideSlot(bool occupied, bool nullValue) {
    if (nullValue) {
        return SlotAction::Clear;
    }
    return occupied ? SlotAction::Reject : SlotAction::Register;
}

ItemTextSource DecideItemTextSource(bool ownedItem, bool clientRowSlot, bool menuApplied,
                                    bool clientProviderRegistered) {
    if (!menuApplied) {
        return ItemTextSource::Original;
    }
    if (ownedItem) {
        return ItemTextSource::Owned;
    }
    if (clientRowSlot) {
        return ItemTextSource::ClientRow;
    }
    return clientProviderRegistered ? ItemTextSource::Client : ItemTextSource::Original;
}

OverlayFrameProblem CheckOverlayFrame(const RR95OverlayFrame& frame) {
    if (frame.pixels == nullptr) {
        return OverlayFrameProblem::NoPixels;
    }
    if (frame.width < 1 || frame.width > kOverlayFrameMaxWidth) {
        return OverlayFrameProblem::Width;
    }
    if (frame.height < 1 || frame.height > kOverlayFrameMaxHeight) {
        return OverlayFrameProblem::Height;
    }
    return OverlayFrameProblem::None;
}

bool IsValidOverlayFrame(const RR95OverlayFrame& frame) {
    return CheckOverlayFrame(frame) == OverlayFrameProblem::None;
}

const char* OverlayFrameProblemToString(OverlayFrameProblem problem) {
    switch (problem) {
    case OverlayFrameProblem::NoPixels:
        return "pixels are null";
    case OverlayFrameProblem::Width:
        return "width is outside 1..640";
    case OverlayFrameProblem::Height:
        return "height is outside 1..480";
    case OverlayFrameProblem::None:
    default:
        return "valid";
    }
}

bool ShouldLogOverlayProblem(uint32_t& loggedMask, OverlayFrameProblem problem) {
    if (problem == OverlayFrameProblem::None) {
        return false;
    }
    const uint32_t bit = uint32_t{1} << static_cast<uint32_t>(problem);
    if ((loggedMask & bit) != 0) {
        return false;
    }
    loggedMask |= bit;
    return true;
}

bool OverlayNeedsUpload(const OverlayUploadState& state, uint32_t counter, int width, int height) {
    if (state.failed && state.counter == counter && state.width == width && state.height == height) {
        return false;  // this exact frame already failed; retry only on a new counter or size
    }
    return !state.uploaded || state.counter != counter || state.width != width || state.height != height;
}

void RecordOverlayUpload(OverlayUploadState& state, uint32_t counter, int width, int height) {
    state.counter = counter;
    state.width = width;
    state.height = height;
    state.uploaded = true;
    state.failed = false;
}

void RecordOverlayUploadFailure(OverlayUploadState& state, uint32_t counter, int width, int height) {
    state.counter = counter;
    state.width = width;
    state.height = height;
    state.uploaded = false;
    state.failed = true;
}

bool OverlayRetryDue(const OverlayRetryState& state, uint64_t nowMs) {
    return state.consecutiveFailures == 0 || nowMs >= state.nextAttemptMs;
}

void RecordOverlayRetryFailure(OverlayRetryState& state, uint64_t nowMs) {
    ++state.consecutiveFailures;
    state.nextAttemptMs = nowMs + kOverlayRetryBackoffMs;
}

void RecordOverlayRetrySuccess(OverlayRetryState& state) {
    state.consecutiveFailures = 0;
    state.nextAttemptMs = 0;
}

bool ShouldLogOverlayFailure(OverlayFailureLog& log, uint32_t code) {
    for (int i = 0; i < log.count; ++i) {
        if (log.codes[i] == code) {
            return false;
        }
    }
    if (log.count < kOverlayFailureLogCodes) {
        log.codes[log.count++] = code;
    } else {
        log.codes[log.next] = code;
        log.next = (log.next + 1) % kOverlayFailureLogCodes;
    }
    return true;
}

bool ShouldInvokeCallback(bool detached) {
    return !detached;
}

int32_t RegisterRaceFrameCallback(State& state, void (*callback)(void* ctx), void* ctx) {
    return RegisterOne(state.raceFrameCallback, state.raceFrameCtx, callback, ctx);
}

int32_t RegisterMenuFrameCallback(State& state, void (*callback)(void* ctx), void* ctx) {
    return RegisterOne(state.menuFrameCallback, state.menuFrameCtx, callback, ctx);
}

int32_t AddGraphicsMenuRow(State& state, const RR95MenuRowDesc* row) {
    if (state.menuClosed) {
        return RR95_STATUS_TOO_LATE;
    }
    if (row == nullptr) {
        state.menuRow = RR95MenuRowDesc{};
        state.menuRowRegistered = false;
        return RR95_STATUS_OK;
    }
    if (row->selectHandler == 0 || row->text == nullptr) {
        return RR95_STATUS_INVALID_ARGUMENT;
    }
    if (state.menuRowRegistered) {
        return RR95_STATUS_REJECTED;
    }
    state.menuRow = *row;
    state.menuRowRegistered = true;
    return RR95_STATUS_OK;
}

int32_t RegisterItemTextProvider(State& state, const char* (*provider)(const void* item, void* ctx), void* ctx) {
    return RegisterOne(state.itemTextProvider, state.itemTextCtx, provider, ctx);
}

int32_t SetOverlayLayer(State& state, const RR95OverlayFrame* (*source)(void* ctx), void* ctx) {
    return RegisterOne(state.overlaySource, state.overlayCtx, source, ctx);
}

const char* GraphicsMenuRowText(const State& state, bool detached) {
    if (!ShouldInvokeCallback(detached)) {
        return "";
    }
    if (!state.menuRowRegistered || state.menuRow.text == nullptr) {
        return "";
    }
    const char* text = state.menuRow.text(state.menuRow.ctx);
    return text != nullptr ? text : "";
}

const char* GraphicsMenuRowText() {
    return GraphicsMenuRowText(Global(), g_detached);
}

State& Global() {
    return g_state;
}

void CloseGraphicsMenuRowSlot(State& state) {
    state.menuClosed = true;
}

void CloseGraphicsMenuRowSlot() {
    CloseGraphicsMenuRowSlot(Global());
}

void OnRaceFrame() {
    if (!ShouldInvokeCallback(g_detached)) {
        return;
    }
    State& state = Global();
    if (state.raceFrameCallback != nullptr) {
        state.raceFrameCallback(state.raceFrameCtx);
    }
}

void OnMenuFrame() {
    if (!ShouldInvokeCallback(g_detached)) {
        return;
    }
    State& state = Global();
    if (state.menuFrameCallback != nullptr) {
        state.menuFrameCallback(state.menuFrameCtx);
    }
}

const char* ItemTextFor(const void* item) {
    if (!ShouldInvokeCallback(g_detached)) {
        return nullptr;
    }
    State& state = Global();
    if (state.itemTextProvider == nullptr) {
        return nullptr;
    }
    return state.itemTextProvider(item, state.itemTextCtx);
}

const RR95OverlayFrame* OverlayFrame() {
    if (!ShouldInvokeCallback(g_detached)) {
        return nullptr;
    }
    State& state = Global();
    return state.overlaySource != nullptr ? state.overlaySource(state.overlayCtx) : nullptr;
}

bool OverlayRegistered() {
    return !g_detached && Global().overlaySource != nullptr;
}

bool GraphicsMenuRowRegistered() {
    return !g_detached && Global().menuRowRegistered;
}

void SetHookState(int32_t hookState) {
    g_hookState = hookState;
}

int32_t HookState() {
    return g_hookState;
}

void SetGraphicsMenuState(int32_t menuState) {
    g_menuState = menuState;
}

int32_t GraphicsMenuState() {
    return g_menuState;
}

int32_t NextGraphicsMenuState(int32_t current, GraphicsMenuEvent event) {
    switch (event) {
    case GraphicsMenuEvent::InstallWithMenu:
        return RR95_MENU_STATE_PENDING;
    case GraphicsMenuEvent::InstallWithoutMenu:
    case GraphicsMenuEvent::Removed:
        return RR95_MENU_STATE_NOT_USED;
    case GraphicsMenuEvent::BuildApplied:
        return current == RR95_MENU_STATE_PENDING ? RR95_MENU_STATE_APPLIED : current;
    case GraphicsMenuEvent::BuildFailed:
        return current == RR95_MENU_STATE_PENDING ? RR95_MENU_STATE_FAILED : current;
    }
    return current;
}

GraphicsMenuBuildOutcome DecideGraphicsMenuBuildOutcome(bool applied) {
    GraphicsMenuBuildOutcome outcome;
    outcome.menuState = applied ? RR95_MENU_STATE_APPLIED : RR95_MENU_STATE_FAILED;
    outcome.itemTextDetourInstalled = applied;
    return outcome;
}

void SetItemTextDetourInstalled(bool installed) {
    g_itemTextDetourInstalled = installed;
}

bool ItemTextDetourInstalled() {
    return g_itemTextDetourInstalled;
}

void SetDetached() {
    g_detached = true;
}

}  // namespace rr95::modapi

extern "C" const RR95ModApi* RR95_GetModApi(uint32_t version) {
    if (version != RR95_MOD_API_VERSION) {
        return nullptr;
    }
    static const RR95ModApi table = {
        RR95_MOD_API_VERSION,
        static_cast<uint32_t>(sizeof(RR95ModApi)),
        rr95::modapi::ApiRegisterRaceFrame,
        rr95::modapi::ApiRegisterMenuFrame,
        rr95::modapi::ApiAddGraphicsMenuRow,
        rr95::modapi::ApiRegisterItemText,
        rr95::modapi::ApiSetOverlay,
        rr95::modapi::HookState,
        rr95::modapi::ApiGetMenuState,
        rr95::modapi::ApiIsItemTextDetourInstalled};
    return &table;
}
