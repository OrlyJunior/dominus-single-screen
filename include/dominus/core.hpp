#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace dominus {

// XInput-normalized values only. An adapter must validate its actual input path
// before supplying these values; M2ENGAGE button enums are not interchangeable.
inline constexpr std::uint16_t ButtonView = 0x0020;
inline constexpr std::uint16_t ButtonY = 0x8000;
inline constexpr std::uint16_t ShortcutButtons = ButtonView | ButtonY;
// LB/RB as the game.exe Steam pad builder 0x22A920 writes them for its only
// l1/r1 action readers (docs/STEAM-INPUT-VIEW.md); the collection turns them
// into DS L (0x200) and R (0x100).
inline constexpr std::uint16_t ButtonLeftShoulder = 0x0100;
inline constexpr std::uint16_t ButtonRightShoulder = 0x0200;
inline constexpr std::uint16_t PageButtons = ButtonLeftShoulder | ButtonRightShoulder;

enum class Title { Unknown, PortraitOfRuin, DawnOfSorrow, OrderOfEcclesia };
enum class Context { Unknown, Exploration, Menu, Dialogue, Cutscene, Touchscreen, Transition };
enum class ScaleMode { Fit, Integer };
enum class Presentation { Original, Gameplay, Map };

struct TitleProfile {
    Title title;
    const char* displayName;
    int nativeWidth;
    int nativeHeight;
    // Static signatures and unit tests cannot establish runtime validation.
    bool runtimeValidated;
};

const TitleProfile& title_profile(Title title) noexcept;

// A rectangle of the collection's back buffer, in normalized coordinates.
struct NormalizedRect { float x = 0.f, y = 0.f, width = 1.f, height = 1.f; };
// How the enlarged view finds the map among the two right-hand panels.
enum class MapPanelSource { Pixels, NativePage };
// How the mod adapts one verified title: pure data. Nothing here applies
// before the runtime has verified the title, its scene and its pause.
struct AdaptationProfile {
    Title title = Title::Unknown;
    // The collection layout: gameplay on the left, the two top-screen pages
    // stacked on the right (the upper one shows the page the game selected).
    NormalizedRect gameplay{}, upperPanel{}, lowerPanel{};
    MapPanelSource mapPanel = MapPanelSource::Pixels;
    // v0.1.1 Portrait: the main screen alone in every verified scene. A title
    // without it shows the main screen alone only where its scene verdict says
    // so, and the collection's original layout everywhere else.
    bool singleScreenEverywhere = false;
    // LB/RB switch between the map and the other top-screen page inside the
    // enlarged view, and every press made in the view (View aside) is kept
    // from the game until released (PageButtonFilter, ViewReservableButtons).
    bool pageNavigation = false;
    // Native marker replay and its position identity validated for this title.
    bool markerReplay = false;
    // The gameplay screen is the DS touch screen: mouse/touch positions are
    // moved from the enlarged image to the original panel, and hidden from
    // the game while the enlarged view is open.
    bool touchRemap = false;
};
// nullptr: no adaptation, the mod stays inert for that title.
const AdaptationProfile* adaptation_profile(Title title) noexcept;

struct Geometry {
    int left = 0;
    int top = 0;
    int width = 0;
    int height = 0;
    double scale = 0.0;
    bool valid = false;
    bool integerApplied = false;
};

// Uses the native DS 256x192 canvas. Fit keeps an exact 4:3 integer-pixel
// rectangle. Integer gracefully falls back to Fit if the window is too small.
Geometry fit_4_3(int viewportWidth, int viewportHeight, ScaleMode mode = ScaleMode::Fit) noexcept;

struct NativePoint {
    double x = 0.0;
    double y = 0.0;
    bool inside = false;
};

// Geometry utility, not a validated touchscreen hook. Outside-bar coordinates
// have inside=false and must not be delivered as touches.
NativePoint screen_to_native(const Geometry& geometry, double x, double y) noexcept;

// A pointer position in client pixels of the collection's window, as the user
// aimed it at the mod's presentation (source shown in `shown`, both in pixels
// of a backWidth x backHeight back buffer stretched over the client area),
// moved to the same spot of `source` in the original layout, which is what
// the collection maps to the DS touch screen. Outside the shown image (black
// bars) inside=false: no touch.
struct ClientPoint {
    int x = 0, y = 0;
    bool inside = false;
};
ClientPoint presented_to_original(const Geometry& shown, int backWidth, int backHeight,
                                  const NormalizedRect& source, int clientWidth, int clientHeight,
                                  int x, int y) noexcept;

// The pointer is hidden from the game while the enlarged view reserves input
// (the DS is paused under it) and, after that, until every mouse button is
// released, so a click made in the view never becomes a touch on resume.
class PointerReservation {
public:
    // True: report no pointer to the game now.
    bool update(bool reserving, bool buttonsHeld) noexcept;
    void reset() noexcept { held_ = false; }

private:
    bool held_ = false;
};

struct InputSnapshot {
    std::uint16_t buttons = 0;
    bool connected = true;
    bool focused = true;
    // Adapter increments this on controller replacement/reconnection, including
    // replacement at the same XInput slot. A slot number is not an identity.
    std::uint64_t connectionEpoch = 0;
};

struct InputPolicy {
    bool shortcutsValidated = false;
    bool modEnabled = false;
    bool mapAllowed = false;
    // Change on task/context/profile changes. An unfinished gesture is canceled
    // and drained rather than being applied in a different context.
    std::uint64_t gestureEpoch = 0;
};

enum class InputAction { None, ToggleMod, ToggleMap };

struct InputResult {
    std::uint16_t forwardedButtons = 0;
    std::uint16_t consumedButtons = 0;
    InputAction action = InputAction::None;
    bool delayedViewPulse = false;
    bool reset = false;
};

// Stateful per chosen controller, not per XInputGetState invocation. Feed each
// observed state into this object in order; repeat polls never repeat actions.
// View alone is classified on release. In bypass/unsupported contexts it is
// replayed as one logical input-sample pulse after classification. A runtime
// adapter must latch that pulse until a game input update consumes it; a random
// connectivity poll must not eat the pulse.
//
// Y alone is never delayed. A Y already delivered before View cannot be undone;
// that order is NOT a chord. Release/repress Y while holding View to form a new
// chord. View-first and truly simultaneous snapshots are supported. A new Y on
// the same sample as a pending View release also completes the chord: sampling
// cannot establish their sub-frame order, and replaying View|Y would leak the
// reserved combination into the original game.
class InputRecognizer {
public:
    InputResult update(const InputSnapshot& input, const InputPolicy& policy) noexcept;
    // Next activation requires neutral reserved buttons; no deferred action.
    void reset() noexcept;

private:
    enum class State { Idle, ViewPending, ChordDrain, Quarantine };
    State state_ = State::Idle;
    bool initialized_ = false;
    bool wasActive_ = false;
    bool reservationOwned_ = false;
    bool pendingEnabled_ = false;
    bool pendingMapAllowed_ = false;
    bool chordArmed_ = false;
    std::uint16_t previousButtons_ = 0;
    std::uint64_t connectionEpoch_ = 0;
    std::uint64_t pendingGestureEpoch_ = 0;
};

// Buttons inside the enlarged view. While the view reserves input (see
// ViewInputOwner), a press that begins on a reservable button (LB/RB always)
// is consumed until it is released, even after the view closed: the DS
// computes trigger = (previous ^ current) & current (dra01 0x18001fb90), so
// an action button still held when emulation resumes would otherwise fire. A
// button already held when the view opened passes unchanged, so resuming sees
// no new edge for it. LB steps back and RB forward; both starting on one
// sample cancel out. Disconnection is no evidence of a release: reservations
// survive it.
struct PageInput {
    std::uint16_t consumedButtons = 0;
    int step = 0;
};
// The d-pad (XInput up/down/left/right at the Steam pad builder output).
inline constexpr std::uint16_t DirectionButtons = 0x000F;
// What the view keeps from the game: every press but View (the gesture
// recognizer's) and the directions. A direction is held movement, so one held
// across the close keeps moving, as after the game's own pause menu; holding
// it back made Soma ignore a direction pressed while closing (second session).
inline constexpr std::uint16_t ViewReservableButtons =
    static_cast<std::uint16_t>(~(ButtonView | DirectionButtons));

// Whether the enlarged view owns new presses: from the request that opens it
// (opening: the View release that asks for it, its panel choice or its pause
// request) while it is shown, until the request that closes it (the View
// release or View+Y recognized on this sample). Never while its pause is being
// released: a press made right after closing belongs to the game.
class ViewInputOwner {
public:
    bool update(bool shown, bool opening, InputAction action) noexcept;
    void reset() noexcept { closing_ = false; }

private:
    bool closing_ = false;
};
class PageButtonFilter {
public:
    PageInput update(std::uint16_t buttons, bool connected, bool reserving,
                     std::uint16_t reservable = PageButtons) noexcept;
    void reset() noexcept { *this = PageButtonFilter{}; }
    std::uint16_t reserved() const noexcept { return reserved_; }

private:
    std::uint16_t previous_ = 0, reserved_ = 0;
    bool initialized_ = false;
};
// The page after a step among count pages, wrapping; page if count is 0.
unsigned step_page(unsigned page, int step, unsigned count) noexcept;

// Portrait shows the two pages of the DS top screen (map, Status) in two panels
// whose order flips with native Select. Status always has the brown enemy panel
// in its lower part (Jonathan and Charlotte alike); the map has none. User
// screenshots, 23/10/2026: map 0.000, Status 0.662-0.675. Samples are
// 0x00RRGGBB on a PanelColumns x PanelRows grid of cell centres, row-major.
inline constexpr unsigned PanelColumns = 16;
inline constexpr unsigned PanelRows = 12;
double lower_brown_fraction(const std::uint32_t* rgb, unsigned columns, unsigned rows) noexcept;
enum class MapPanel { Unknown, First, Second };
// Relative: the map is the panel with clearly less brown than the other one.
MapPanel choose_map_panel(const std::uint32_t* first, const std::uint32_t* second,
                          unsigned columns, unsigned rows) noexcept;

// Panel images at DS screen resolution (one sample per DS pixel, 0x00RRGGBB).
inline constexpr unsigned NativeWidth = 256;
inline constexpr unsigned NativeHeight = 192;
// Changed samples and their bounding box, [left,right) x [top,bottom).
struct ImageDiff { unsigned changed = 0, left = 0, top = 0, right = 0, bottom = 0; };
ImageDiff diff_images(const std::uint32_t* a, const std::uint32_t* b,
                      unsigned width, unsigned height) noexcept;

// The collection's pause stops the whole DS (EmuTask frame loop 0x14002aef0
// skips emulation while +0x185 is set), so the DS-drawn map marker freezes in
// one blink phase. This follows what a panel shows while the game runs: the
// image now (slot latest()) and the one before it, and whether the two have
// been strictly alternating. It never guesses which pixels are the marker.
class BlinkTracker {
public:
    enum class Event { Same, Toggle, NewState };
    BlinkTracker(unsigned width = NativeWidth, unsigned height = NativeHeight);
    // `image` holds width*height samples taken at timeMs (non-decreasing).
    Event observe(const std::uint32_t* image, std::uint64_t timeMs) noexcept;
    void reset() noexcept;
    // The source was paused for pausedMs since a run that began at or before
    // pausedSinceMs: the DS did not advance, so that time counts neither
    // toward the run nor toward staleness. Later runs are unaffected.
    void exclude_pause(std::uint64_t pausedSinceMs, std::uint64_t pausedMs) noexcept;
    unsigned latest() const noexcept { return latest_; }
    // Back-and-forth changes between the same two images since the last new one.
    unsigned toggles() const noexcept { return toggles_; }
    bool hasPrevious() const noexcept { return hasPrevious_; }
    std::uint64_t runStartMs() const noexcept { return since_; }
    // Median of the last completed runs of a slot's image; 0 if none yet.
    std::uint64_t typicalRunMs(unsigned slot) const noexcept;
    const std::uint32_t* image(unsigned slot) const noexcept { return slots_[slot & 1].image.data(); }
    ImageDiff difference() const noexcept;

private:
    struct Slot {
        std::vector<std::uint32_t> image;
        std::array<std::uint64_t, 4> runs{};
        unsigned runCount = 0;
    };
    unsigned width_, height_;
    Slot slots_[2];
    unsigned latest_ = 0, toggles_ = 0;
    bool hasLatest_ = false, hasPrevious_ = false;
    std::uint64_t since_ = 0;
};

// Conservative limits for treating the tracked pair as a blink: strict
// alternation seen several times, plausible half-periods, and the two images
// differing only in a small area (DS pixels). Anything else is refused and
// the frozen frame is shown unchanged.
struct BlinkLimits {
    unsigned minToggles = 3;
    std::uint64_t minRunMs = 30, maxRunMs = 2000;
    // Room for a blinking room cell, not for a panel-wide change.
    unsigned maxBox = 128, maxChanged = 6000;
};
struct BlinkPlan {
    bool valid = false;
    const char* reason = "none";
    unsigned frozen = 0;                       // slot the paused game shows
    std::uint64_t frozenMs = 0, otherMs = 0;   // typical run of each image
    std::uint64_t phaseStartMs = 0;            // when the frozen image's run began
    ImageDiff diff{};
};
// Called once the panel stopped changing under the pause, at nowMs.
BlinkPlan plan_blink(const BlinkTracker& tracker, std::uint64_t nowMs,
                     const BlinkLimits& limits = {}) noexcept;
// Whether to show the other image at nowMs: the frozen image's run continues
// from where the game stopped, then both alternate with the measured runs.
bool blink_shows_other(const BlinkPlan& plan, std::uint64_t nowMs) noexcept;

// Multi-state replacement used by the marker compositor. Slots identify unique
// captured images; an ordered cycle can visit a slot more than once. Durations
// are inferred nominal 60 Hz ticks from dense capture timestamps, never a
// rounded millisecond duration per phase. This is not a native game counter;
// Complete means two observed ordered cycles agreed, not runtime validation.
inline constexpr unsigned MaxMarkerCycleStates = 16;
enum class MarkerCycleConfidence { Frozen, Partial, Complete };
struct MarkerCycleLimits {
    unsigned maxChanged = 192, maxTransitionBox = 16, maxUnionBox = 20;
    // More than one native frame between observations could hide a visual
    // state, so it invalidates both the complete cycle and the partial history.
    std::uint64_t maxSampleGapMs = 25;
    // Portrait's shortest visible phase is two native frames (~33 ms),
    // longer than the accepted sampling gap; a whole phase cannot be skipped.
    unsigned minHoldTicks = 2, maxHoldTicks = 120;
    // Authentic single-lap closure for Portrait's 39-tick (650 ms) hardware
    // marker cycle once capture revisits the loop start with full bounds.
    unsigned nativePeriodTicks = 39;
    unsigned minSingleLapStates = 12, maxSingleLapHoldTicks = 5;
    unsigned minSingleLapChanged = 48, minSingleLapUnionBox = 10;
};
struct MarkerCyclePlan {
    bool valid = false;
    const char* reason = "no-images";
    MarkerCycleConfidence confidence = MarkerCycleConfidence::Frozen;
    std::uint64_t generation = 0;
    unsigned frozenSlot = 0, count = 0;
    std::array<unsigned, MaxMarkerCycleStates> slots{};
    std::array<unsigned, MaxMarkerCycleStates> holdTicks{};
    unsigned periodTicks = 0;
    // For a complete cycle, the run containing the pause continues from its
    // original onset. A partial sequence starts at the pause and excludes its
    // unfinished run; it does not claim to reconstruct the missing cycle.
    std::uint64_t phaseStartMs = 0;
    ImageDiff diff{};
};
struct MarkerCycleFrame {
    unsigned slot = 0, sequenceIndex = 0, holdTicks = 0;
};
class MarkerCycleTracker {
public:
    enum class Event { Same, Captured, Revisited, Reset };
    MarkerCycleTracker(unsigned width = NativeWidth, unsigned height = NativeHeight,
                       const MarkerCycleLimits& limits = {});
    // Supply the capture timestamp, not GPU-collection time. identityEpoch
    // must change on task/title/panel/position boundaries known by the adapter.
    Event observe(const std::uint32_t* image, std::uint64_t timeMs,
                  std::uint64_t identityEpoch = 0) noexcept;
    void reset() noexcept;
    void exclude_pause(std::uint64_t pausedSinceMs, std::uint64_t pausedMs) noexcept;
    unsigned latest() const noexcept { return latest_; }
    unsigned imageCount() const noexcept { return imageCount_; }
    unsigned completedRuns() const noexcept { return runCount_; }
    std::uint64_t generation() const noexcept { return generation_; }
    std::uint64_t runStartMs() const noexcept { return since_; }
    const std::uint32_t* image(unsigned slot) const noexcept;
    ImageDiff difference() const noexcept { return unionDiff_; }
    // frozenAtMs is the pause's onset, even when called after GPU settling.
    MarkerCyclePlan plan(std::uint64_t frozenAtMs) const noexcept;

private:
    struct Run { unsigned slot = 0, ticks = 0; };
    static constexpr unsigned HistorySize = MaxMarkerCycleStates * 3;
    void start(const std::uint32_t* image, std::uint64_t timeMs,
               std::uint64_t identityEpoch) noexcept;
    void appendRun(unsigned slot, unsigned ticks) noexcept;
    void findCycle() noexcept;
    unsigned width_, height_;
    MarkerCycleLimits limits_;
    std::array<std::vector<std::uint32_t>, MaxMarkerCycleStates> images_{};
    std::vector<unsigned char> unionMask_;
    std::array<Run, HistorySize> runs_{};
    unsigned latest_ = 0, imageCount_ = 0, runCount_ = 0, cycleCount_ = 0;
    std::array<Run, MaxMarkerCycleStates> cycle_{};
    unsigned expected_ = 0, initialZeroTicks_ = 0;
    std::uint64_t generation_ = 0, identityEpoch_ = 0, since_ = 0, lastSampleMs_ = 0;
    bool haveTransition_ = false, pauseInterrupted_ = false;
    ImageDiff unionDiff_{};
};
MarkerCycleFrame marker_cycle_frame(const MarkerCyclePlan& plan, std::uint64_t nowMs) noexcept;
MarkerCyclePlan plan_marker_cycle(const MarkerCycleTracker& tracker, std::uint64_t frozenAtMs) noexcept;

struct PauseState {
    bool available = false;
    bool paused = false;
    std::uint64_t revision = 0;
};

struct PauseLease {
    // Nonzero token identifies both the task lifetime and the owner. revision
    // changes on every pause request, including same-value external requests.
    std::uint64_t token = 0;
    std::uint64_t revision = 0;
};

enum class PauseAcquireStatus { Acquired, AlreadyPaused, Unavailable, Failed };
struct PauseAcquisition {
    PauseAcquireStatus status = PauseAcquireStatus::Unavailable;
    PauseLease lease{};
};
enum class PauseReleaseStatus { Released, Superseded, Unavailable, Failed };

// Adapter contract: calls occur on the validated safe game thread. acquirePause
// atomically observes/requests the native simulation pause, returning a lease
// only for a pause it owns. Failed/Unavailable must not change pause state.
// releasePause performs a conditional compare-and-release: a changed task,
// revision, or owner yields Superseded and MUST NOT unpause anything. Every
// external pause request must invalidate prior leases, even if it writes true
// over true. Without these guarantees this backend must remain unavailable.
// No raw game addresses or writes are part of this portable core.
class IPauseBackend {
public:
    virtual ~IPauseBackend() = default;
    virtual PauseState observe() noexcept = 0;
    virtual PauseAcquisition acquirePause() noexcept = 0;
    virtual PauseReleaseStatus releasePause(PauseLease lease) noexcept = 0;
};

enum class PauseIssue { None, Unavailable, AcquireFailed, OwnershipLost, ReleasePending };

class PauseController {
public:
    PauseController() noexcept = default;
    PauseController(const PauseController&) = delete;
    PauseController& operator=(const PauseController&) = delete;
    PauseController(PauseController&&) = delete;
    PauseController& operator=(PauseController&&) = delete;
    // backend must outlive this controller's outstanding lease. No destructor
    // invokes the backend: shutdown/service must run explicitly on a safe thread.
    bool begin(IPauseBackend& backend) noexcept;
    void end() noexcept;
    // Services pending releases and verifies that active map pause is real.
    bool service() noexcept;
    bool active() const noexcept { return active_; }
    bool ownsPause() const noexcept { return active_ && lease_.token != 0; }
    bool releasePending() const noexcept { return !active_ && lease_.token != 0; }
    PauseIssue issue() const noexcept { return issue_; }
    IPauseBackend* backend() const noexcept { return backend_; }

private:
    void releaseOwned() noexcept;
    IPauseBackend* backend_ = nullptr;
    PauseLease lease_{};
    bool active_ = false;
    PauseIssue issue_ = PauseIssue::None;
};

struct ContextSnapshot {
    Title title = Title::Unknown;
    Context scene = Context::Unknown;
    bool validated = false;
    // Increment when changing task, title, save/session, or another identity
    // boundary, even if the scene label remains Exploration.
    std::uint64_t epoch = 0;
};

struct Capabilities {
    Title title = Title::Unknown;
    bool gameplaySurfaceValidated = false;
    bool mapSurfaceValidated = false;
    bool realPauseValidated = false;
    bool inputPathValidated = false;
};

struct RuntimeSnapshot {
    ContextSnapshot context{};
    Capabilities capabilities{};
};

struct FrameResult {
    InputResult input{};
    Presentation presentation = Presentation::Original;
    bool modEnabled = false;
    bool mapOpen = false;
    bool pauseOwned = false;
    bool pauseReleasePending = false;
    bool mapOperationRejected = false;
    PauseIssue pauseIssue = PauseIssue::None;
};

// Pure coordinator; it neither calls D3D nor modifies the native layout. A
// renderer must leave Original completely untouched. All validation defaults
// are false: static analysis is never enough to turn on native integration.
class ModController {
public:
    explicit ModController(bool initiallyEnabled = false) noexcept : enabled_(initiallyEnabled) {}
    ModController(const ModController&) = delete;
    ModController& operator=(const ModController&) = delete;
    ModController(ModController&&) = delete;
    ModController& operator=(ModController&&) = delete;
    FrameResult update(const InputSnapshot& input, const RuntimeSnapshot& runtime,
                       IPauseBackend* pauseBackend = nullptr) noexcept;
    void setEnabled(bool enabled) noexcept;
    void shutdown() noexcept;
    bool enabled() const noexcept { return enabled_; }
    bool mapOpen() const noexcept { return pause_.active(); }
    bool pauseReleasePending() const noexcept { return pause_.releasePending(); }

private:
    bool enabled_ = false;
    InputRecognizer input_{};
    PauseController pause_{};
    bool haveRuntime_ = false;
    RuntimeSnapshot lastRuntime_{};
    std::uint64_t gestureEpoch_ = 0;
    bool haveInput_ = false;
    std::uint64_t connectionEpoch_ = 0;
};

// Which title the collection's EmuTask has loaded (game.exe E6FA885F...6B64,
// REA/Ghidra 12.1.4; docs/TITLE-ISOLATION.md). EmuTask::loadEmuModuleNum
// (0x1400299d0) frees the module in +0x528, clears +0x184, stores
// LoadLibraryA(dra01/dra02/dra03/ac_dracula.dll[index]) there and, once the
// module's m2epi_moduledata export has initialized, writes the index to +0x53c
// and sets +0x184. The script setter setEmuModuleNum (0x140029bf0) writes +0x53c
// alone before the reload, so the index by itself never proves the title. The
// constructors start with module 0, index -1 and +0x184 clear; index 4 is the
// collection's static module, which loads no DLL.
struct EmuTaskTitleFields {
    std::uintptr_t vtable = 0;
    std::uintptr_t module = 0;  // +0x528
    std::int32_t index = -1;    // +0x53c
    std::uint8_t ready = 0;     // +0x184
};
inline constexpr std::int32_t DawnModuleIndex = 0;
inline constexpr std::int32_t PortraitModuleIndex = 1;
inline constexpr std::int32_t StaticModuleIndex = 4;

// Titles the detector may verify. Any other index is OtherTitle, as in
// v0.1.1. Dawn stays off unless the experimental profile is enabled.
struct TitleSet {
    bool portrait = true;
    bool dawn = false;
};

// A loaded module as the platform sees it. Valid only for a MEM_IMAGE x64 DLL
// mapped at handle whose path is the title's DLL (dra01.dll for Dawn,
// dra02.dll for Portrait) in the game.exe directory.
struct TitleModuleImage {
    std::uintptr_t handle = 0;
    std::uint32_t imageSize = 0;
    std::uint32_t timestamp = 0;
    bool valid = false;
};
// Identity of that file on disk (volume, file id, size, last write time).
struct TitleFileKey {
    std::uint64_t volume = 0, file = 0, size = 0, written = 0;
    bool valid = false;
};

// Read-only platform access. Implementations never call game code, load or pin
// a module, or write game memory; any failed read is reported, not guessed.
class ITitlePlatform {
public:
    virtual ~ITitlePlatform() = default;
    // The EmuTask published at game.exe+0x599FD0 with the derived vtable.
    virtual bool read_task(std::uintptr_t& task) noexcept = 0;
    virtual bool read_fields(std::uintptr_t task, EmuTaskTitleFields& fields) noexcept = 0;
    // The currently loaded DLL of title (dra02.dll, dra01.dll), 0 if none.
    virtual std::uintptr_t title_module(Title title) noexcept = 0;
    virtual TitleModuleImage module_image(Title title, std::uintptr_t handle) noexcept = 0;
    virtual TitleFileKey file_key(Title title, const TitleModuleImage& image) noexcept = 0;
    // Expensive: the whole file's SHA-256 and PE identity against image, with
    // the file identity unchanged (key) before and after reading it.
    virtual bool verify_file(Title title, const TitleModuleImage& image, const TitleFileKey& key) noexcept = 0;
};

enum class TitleReason {
    Verified, NoTask, TaskMismatch, ReadFailed, NoModule, OtherTitle, StaticModule,
    Loading, ModuleMismatch, ModuleImage, ModuleFile, Inconsistent
};
const char* title_reason_name(TitleReason reason) noexcept;

struct TitleObservation {
    // The verified title: Unknown unless reason is Verified. Only this enables
    // anything title-specific.
    Title title = Title::Unknown;
    // title == PortraitOfRuin (kept for the v0.1.1 diagnostics).
    bool portrait = false;
    TitleReason reason = TitleReason::NoTask;
    // What +0x53c names (0 dra01, 1 dra02, 2 dra03). Diagnostic, never proof.
    Title indexed = Title::Unknown;
    EmuTaskTitleFields fields{};
    std::uint64_t taskEpoch = 0;
    // Changes whenever the verdict, reason, fields or task epoch change.
    std::uint64_t generation = 0;
};

// Fail-closed identification, once per Present. A title in the enabled set
// requires the native observers' task, its index (1 Portrait, 0 Dawn) with
// +0x184 set, +0x528 equal to the loaded DLL of that title, that module's
// image/path, its file hash, and an identical second read of the fields
// bracketing those checks. While nothing changes, a frame costs two field
// reads and a module lookup: each title's file is hashed once per loaded
// image, its cheap file identity is re-checked on each new task epoch, and a
// failed verification is retried at most MaxFileAttempts times per epoch.
class TitleDetector {
public:
    static constexpr unsigned MaxFileAttempts = 3;
    static constexpr std::uint64_t FileRetryMs = 5000;
    explicit TitleDetector(TitleSet enabled = {}) noexcept : enabled_(enabled) {}
    // observedTask: the task the native observers validated (0 if none).
    TitleObservation update(ITitlePlatform& platform, std::uintptr_t observedTask,
                            std::uint64_t taskEpoch, std::uint64_t nowMs) noexcept;
    void reset() noexcept { *this = TitleDetector{enabled_}; }
    TitleSet enabled() const noexcept { return enabled_; }
    // File hashes attempted so far (diagnostic and tests).
    unsigned verifications() const noexcept { return verifications_; }

private:
    // Per title, so returning to a title already hashed costs no new hash.
    struct FileCheck {
        TitleModuleImage image{};
        TitleFileKey key{};
        bool haveImage = false, fileOk = false;
        std::uint64_t keyEpoch = 0, retryAtMs = 0;
        unsigned attempts = 0;
    };
    bool file_verified(ITitlePlatform& platform, Title title, const TitleModuleImage& image,
                       std::uint64_t taskEpoch, std::uint64_t nowMs) noexcept;
    TitleSet enabled_{};
    TitleObservation last_{};
    bool haveLast_ = false;
    bool fastValid_ = false;
    Title fastTitle_ = Title::Unknown;
    std::uintptr_t fastTask_ = 0, fastHandle_ = 0;
    std::uint64_t fastEpoch_ = 0;
    EmuTaskTitleFields fastFields_{};
    FileCheck files_[2]{};  // Portrait, Dawn
    unsigned verifications_ = 0;
};

// What a supported title is doing, separate from which title it is. Only a
// title-specific classifier fills it; a title without one stays Unknown, so
// nothing treats every screen of a supported title as gameplay.
enum class GameScene { Unknown, Boot, Title, StartMenu, Loading, Gameplay, InGameMenu, Sequence, ExtraMode };
const char* game_scene_name(GameScene scene) noexcept;
struct SceneVerdict {
    GameScene scene = GameScene::Unknown;
    // View belongs to the mod (enlarged map). Elsewhere it stays native Select.
    bool gameplay = false;
    // The sub-screen shows the real map now: the enlarged map may open.
    bool mapReady = false;
    // The main screen alone shows what the title needs now. Used by titles
    // without AdaptationProfile::singleScreenEverywhere; elsewhere the
    // collection's original layout is shown.
    bool singleScreen = false;
    // With mapReady, the panel that holds the map: 0 upper, 1 lower; -1 when
    // the title does not tell (Portrait uses pixels).
    int mapPanel = -1;
    // With mapReady and mapPanel: how many pages the view offers (2 for the
    // map/Status pair, LB/RB switching them).
    unsigned viewPages = 0;
    // A short native transition (a start/load mode): keep whichever of the
    // single screen or the original layout was shown, so no flash in between.
    bool holdLayout = false;
};

// Portrait's own state, read from dra02.dll FFEFA110...8744 (Ghidra 12.1.4;
// docs/TITLE-ISOLATION.md). The GameThread loop 0x1800149a0 calls
// modes[0x1803695a0][mode 0x18098c224] each frame: 2 is gameplay (0x180037d80,
// running while 0x1809b1e3c == 2; Start enters 7, the pause menu), 5 the title,
// 0xb the Konami logo, 0/1 game start/load, 7/8/9 in-game menus, 0x12/0x13/0x16
// start menus, 0xd-0x11/0x17 scripted sequences. The sub-screen page manager
// 0x180022ab0 shows page 0x1809bb6f8 and switches to the requested 0x1809bb6f9:
// 0 = map (Status drawn alongside), 1 = Status (map alongside, native Select
// toggles 0/1), 4 menus, 5/6 title images. Game type 0x1809b2661 is 0 for the
// normal game; 1-5 are extra modes whose second panel is another screen.
struct PortraitStateFields {
    std::uint8_t mode = 0xff;
    std::uint8_t gameState = 0xff;
    std::uint8_t gameType = 0xff;
    std::uint8_t page = 0xff;
    std::uint8_t requestedPage = 0xff;
};
inline constexpr std::uint8_t PortraitGameplayMode = 2, PortraitRunningState = 2;
// available=false (unreadable or inconsistent fields) is Unknown.
SceneVerdict classify_portrait_scene(const PortraitStateFields& fields, bool available) noexcept;

// Dawn of Sorrow's own state, read from dra01.dll 52E20914...CD6E (Ghidra
// 12.1.4; docs/DAWN-OF-SORROW.md). The GameThread loop 0x18000b700 calls
// modes[0x18026c600][mode 0x1808eb248] each frame: 2 is gameplay (0x18002b1b0,
// running while 0x180963ecc == 2; Start enters 0xa, the pause menu), 3 the
// title screen, 4 the title menu and 9 the file/mode select (all three with
// the decorative page 3 on the top screen; 4 reads the touch screen itself,
// 9 through 0x1801e21c0), 5 game over, 0x16 the closing sequence, 0/1 game
// start/load. The pause menu 0x180059390 runs its sub-menus (sub-state
// 0x1808eb249 1-8, several reading the touch screen) on the main screen and
// leaves the top-screen page alone; it asks for page 3 only when it exits to
// something other than gameplay. The top-screen manager
// 0x180022100 shows page 0x180965748 and switches to the requested
// 0x180965749. Pages 0 (map) and 1 (Status) are a pair: the shown page is
// drawn on the first sub-screen bank and, unless the overlay flag
// 0x18096574b is set, the other page on the second, so both stay live. Other
// pages replace the pair: 2 the warp rooms' own screen (0x18006ccb0: a wall
// with the localized picture of an area's name, 0x18006d440; native Select
// never switches pages in a warp room, 0x180022010), 3 a decorative
// "paper" screen (f_paper1/2) shown in the menus, the pause menu and, in the
// first validation session (28/10/2026), throughout the opening gameplay of a
// new game, where the overlay and the page lock also stayed set. Game type
// 0x180964346 is 0 for the normal game; 1-5 are other modes.
// Gameplay sub-states (0x180963ecc, then its step): 0/1 entering, 2 running,
// 3 room change, 4 step 1 the warp room's destination choice, 6 an area load
// with the game frozen (0x1808eb250 set, 0x1800191f0), 5 and others unknown.
// Brightness fades run while the int32 frame count 0x1808f5c2c is nonzero;
// 0x1808f5c34 is the level (0 normal).
// Event flags 0x180963ed4: 0x10000000 disables native Select page switching
// (0x180022010); 0x200001 blocks the game's own pause menu (Start).
struct DawnStateFields {
    std::uint8_t mode = 0xff;
    std::uint8_t gameState = 0xff;
    std::uint8_t subState = 0xff;  // the step of gameState (0x180963ecd)
    std::uint8_t gameType = 0xff;
    std::uint8_t page = 0xff;
    std::uint8_t requestedPage = 0xff;
    std::uint8_t pageOverlay = 0xff;
    std::uint8_t frozen = 0xff;
    std::uint32_t eventFlags = 0xffffffffu;
    std::int32_t fadeFrames = -1;
    std::int32_t brightness = -1;
};
inline constexpr std::uint8_t DawnGameplayMode = 2, DawnRunningState = 2;
// Only the warp room device 0x180077390 enters sub-state 4: Up at its centre
// stores the word 0x104 (sub-state 4, step 1), freezes the game and sets event
// flag 1. 0x180078500 then runs the choice on the main screen (the castle map
// with a marker per destination and a cursor; d-pad, A/B, a tap on a marker)
// while the top screen keeps page 2, now showing the highlighted destination's
// area picture; it ends with the word 1. Step 0 (0x18002ad80) has no producer.
inline constexpr std::uint8_t DawnWarpChoiceState = 4, DawnWarpChoiceStep = 1, DawnWarpRoomPage = 2;
inline constexpr std::uint32_t DawnPageLockFlag = 0x10000000u, DawnMenuBlockFlags = 0x00200001u;
bool same_dawn_fields(const DawnStateFields& a, const DawnStateFields& b) noexcept;
// Single screen (the main screen alone) in: normal-game mode 2 in a known
// sub-state, whatever the top screen shows, and in the warp room's destination
// choice (sub-state 4 step 1 with page 2 shown and requested, no map to
// enlarge); the title screen, title menu and file/mode select while the top
// screen shows only the decorative page 3; the pause menu of the normal game.
// Start/load modes hold the current layout.
// View is the mod's command only in gameplay with a live map: the top screen
// holds the map/Status pair (page 0 or 1 shown and requested, no overlay,
// native switching allowed); elsewhere, menus included, it stays native
// Select. The enlarged map additionally needs the running sub-state, no page
// switch in progress, no frozen game, no fade and the game's own pause menu
// allowed; no page authorizes it alone. It opens on the panel that holds the
// map (page 0: the shown page's panel; page 1: the other one), with LB/RB to
// Status. Everything else (boot screens, sequences, game over, other modes,
// unknown or unreadable states) keeps the original layout and native Select.
SceneVerdict classify_dawn_scene(const DawnStateFields& fields, bool available) noexcept;

// What the compositor shows for a title whose identity, focus, native and
// pause gates already passed: nothing while the mod is off; the enlarged view
// while it is open; the main screen alone where the profile or the scene
// allows it, or where the scene holds a layout that was the single screen
// (previous: the last presentation shown); the original layout elsewhere.
Presentation title_presentation(const AdaptationProfile& profile, const SceneVerdict& scene,
                                bool enabled, bool viewOpen,
                                Presentation previous = Presentation::Original) noexcept;

// The scene fields of whichever title was verified (the other part unused).
struct TitleSceneFields {
    Title title = Title::Unknown;
    PortraitStateFields portrait{};
    DawnStateFields dawn{};
};
SceneVerdict classify_scene(const TitleSceneFields& fields, bool available) noexcept;

// What a View release asks of the map, decided from the current state only.
enum class MapCommand { None, CloseVoluntary, Cancel, Open, Refuse };
struct MapCommandInput {
    bool mapOpen = false;   // the map is shown under the mod's own pause
    bool owned = false;     // the mod holds a pause lease
    bool choosing = false;  // panel selection before the pause request
    bool paused = false;    // the EmuTask is paused
    bool mapReady = false;
};
MapCommand map_command(const MapCommandInput& input) noexcept;

struct TitleGateInput {
    TitleObservation title{};
    bool pauseKnown = false;
    bool paused = false;
    // The mod owns the pause, has a pause request in flight, or left a stale
    // pause that only it may release (native::Status::stalePause).
    bool modPause = false;
    SceneVerdict scene{};
};
struct TitleGateState {
    bool identity = false;  // title-specific presentation, map, pause and marker may run
    bool input = false;     // the input filter may reserve, delay or chord View/Y
    bool gameplay = false;  // identity, and View is the map command (not native Select)
    bool mapReady = false;  // gameplay, input, and the enlarged map may open now
    bool identityChanged = false;
    bool inputChanged = false;
    bool gameplayChanged = false;
    Title title = Title::Unknown;  // the title identity stands for
};
// Identity needs Confirmations consecutive verified observations of the same
// title, module and task epoch; anything else revokes it at once. Input
// additionally needs a known pause state that is either clear or the mod's
// own, so the collection's menus (which pause the EmuTask) receive their
// original input. Gameplay and mapReady come from the scene verdict and need
// identity.
class TitleGate {
public:
    static constexpr unsigned Confirmations = 2;
    TitleGateState update(const TitleGateInput& input) noexcept;
    void reset() noexcept { *this = TitleGate{}; }
    bool identity() const noexcept { return identity_; }
    bool input() const noexcept { return input_; }
    bool gameplay() const noexcept { return gameplay_; }
    Title title() const noexcept { return identity_ ? title_ : Title::Unknown; }

private:
    unsigned streak_ = 0;
    Title title_ = Title::Unknown;
    std::uintptr_t module_ = 0;
    std::uint64_t epoch_ = 0;
    bool identity_ = false, input_ = false, gameplay_ = false;
};
// The v0.1.1 names, for Portrait-only callers and tests.
using PortraitGateInput = TitleGateInput;
using PortraitGateState = TitleGateState;
using PortraitGate = TitleGate;

} // namespace dominus
