#include "dominus/core.hpp"

#include <algorithm>
#include <cmath>

namespace dominus {
namespace {

constexpr TitleProfile Profiles[] = {
    {Title::Unknown, "Unknown / unsupported", 256, 192, false},
    {Title::PortraitOfRuin, "Castlevania: Portrait of Ruin", 256, 192, false},
    {Title::DawnOfSorrow, "Castlevania: Dawn of Sorrow", 256, 192, false},
    {Title::OrderOfEcclesia, "Castlevania: Order of Ecclesia", 256, 192, false},
};

// The exact preset observed in real Portrait sessions at 1920x1080 and
// 2560x1440: gameplay on the left (1280x960 at 1080p) plus the two right-hand
// 640x480 panels. Their order follows native Select (upper = the selected
// page), so Portrait picks the map panel from pixels each time View opens it.
constexpr NormalizedRect CollectionGameplay{0.f, 1.f / 18.f, 2.f / 3.f, 8.f / 9.f};
constexpr NormalizedRect CollectionUpperPanel{2.f / 3.f, 1.f / 18.f, 1.f / 3.f, 4.f / 9.f};
constexpr NormalizedRect CollectionLowerPanel{2.f / 3.f, 0.5f, 1.f / 3.f, 4.f / 9.f};

constexpr AdaptationProfile Adaptations[] = {
    // v0.1.1, validated in gameplay.
    {Title::PortraitOfRuin, CollectionGameplay, CollectionUpperPanel, CollectionLowerPanel,
     MapPanelSource::Pixels, true, false, true, false},
    // Experimental: same collection layout assumed (not yet observed in Dawn),
    // map panel from the native page, LB/RB pages, marker frozen (no replay
    // until its cycle and position identity are established for dra01), and
    // pointer positions remapped: Dawn's gameplay screen is its touch screen
    // (dra01 0x18006fef0 adds the camera to touch coordinates).
    {Title::DawnOfSorrow, CollectionGameplay, CollectionUpperPanel, CollectionLowerPanel,
     MapPanelSource::NativePage, false, true, false, true},
};

bool same_runtime(const RuntimeSnapshot& a, const RuntimeSnapshot& b) noexcept {
    return a.context.title == b.context.title && a.context.scene == b.context.scene &&
           a.context.validated == b.context.validated && a.context.epoch == b.context.epoch &&
           a.capabilities.title == b.capabilities.title &&
           a.capabilities.gameplaySurfaceValidated == b.capabilities.gameplaySurfaceValidated &&
           a.capabilities.mapSurfaceValidated == b.capabilities.mapSurfaceValidated &&
           a.capabilities.realPauseValidated == b.capabilities.realPauseValidated &&
           a.capabilities.inputPathValidated == b.capabilities.inputPathValidated;
}

bool safe_context(const RuntimeSnapshot& runtime, bool focused) noexcept {
    return focused && runtime.context.validated &&
           title_profile(runtime.context.title).title != Title::Unknown &&
           runtime.context.scene == Context::Exploration &&
           runtime.capabilities.title == runtime.context.title;
}

} // namespace

const TitleProfile& title_profile(Title title) noexcept {
    for (const auto& profile : Profiles) {
        if (profile.title == title) return profile;
    }
    return Profiles[0];
}

const AdaptationProfile* adaptation_profile(Title title) noexcept {
    for (const auto& profile : Adaptations) {
        if (profile.title == title) return &profile;
    }
    return nullptr;
}

Geometry fit_4_3(int viewportWidth, int viewportHeight, ScaleMode mode) noexcept {
    Geometry result{};
    if (viewportWidth <= 0 || viewportHeight <= 0) return result;
    const int integerScale = std::min(viewportWidth / 256, viewportHeight / 192);
    if (mode == ScaleMode::Integer && integerScale >= 1) {
        result.width = 256 * integerScale;
        result.height = 192 * integerScale;
        result.integerApplied = true;
    } else {
        const int ratioUnits = std::min(viewportWidth / 4, viewportHeight / 3);
        if (ratioUnits <= 0) return result;
        result.width = 4 * ratioUnits;
        result.height = 3 * ratioUnits;
    }
    result.left = (viewportWidth - result.width) / 2;
    result.top = (viewportHeight - result.height) / 2;
    result.scale = static_cast<double>(result.width) / 256.0;
    result.valid = true;
    return result;
}

NativePoint screen_to_native(const Geometry& geometry, double x, double y) noexcept {
    NativePoint result{};
    if (!geometry.valid || geometry.width <= 0 || geometry.height <= 0 ||
        !std::isfinite(x) || !std::isfinite(y)) return result;
    const double localX = x - geometry.left;
    const double localY = y - geometry.top;
    if (localX < 0.0 || localY < 0.0 || localX >= geometry.width || localY >= geometry.height)
        return result;
    result.x = localX * 256.0 / geometry.width;
    result.y = localY * 192.0 / geometry.height;
    result.inside = true;
    return result;
}

ClientPoint presented_to_original(const Geometry& shown, int backWidth, int backHeight,
                                  const NormalizedRect& source, int clientWidth, int clientHeight,
                                  int x, int y) noexcept {
    ClientPoint result{};
    if (!shown.valid || shown.width <= 0 || shown.height <= 0 || backWidth <= 0 || backHeight <= 0 ||
        clientWidth <= 0 || clientHeight <= 0 || !(source.width > 0.f) || !(source.height > 0.f) ||
        x < 0 || y < 0 || x >= clientWidth || y >= clientHeight) return result;
    // The centre of the client pixel, in back-buffer pixels.
    const double bx = (x + 0.5) * backWidth / clientWidth;
    const double by = (y + 0.5) * backHeight / clientHeight;
    const double u = (bx - shown.left) / shown.width;
    const double v = (by - shown.top) / shown.height;
    if (!(u >= 0.0 && u < 1.0 && v >= 0.0 && v < 1.0)) return result;
    const double nx = source.x + u * source.width;
    const double ny = source.y + v * source.height;
    if (!(nx >= 0.0 && nx < 1.0 && ny >= 0.0 && ny < 1.0)) return result;
    result.x = std::min(clientWidth - 1, static_cast<int>(nx * clientWidth));
    result.y = std::min(clientHeight - 1, static_cast<int>(ny * clientHeight));
    result.inside = true;
    return result;
}

bool PointerReservation::update(bool reserving, bool buttonsHeld) noexcept {
    if (reserving) {
        held_ = held_ || buttonsHeld;
        return true;
    }
    if (held_ && buttonsHeld) return true;
    held_ = false;
    return false;
}

double lower_brown_fraction(const std::uint32_t* rgb, unsigned columns, unsigned rows) noexcept {
    if (!rgb || !columns || !rows) return 0.0;
    // Lower 5/12 of the panel: the Status enemy panel's frame and fields.
    const unsigned first = rows - rows * 5 / 12;
    unsigned brown = 0;
    for (unsigned row = first; row < rows; ++row) {
        for (unsigned column = 0; column < columns; ++column) {
            const auto c = rgb[row * columns + column];
            const int r = (c >> 16) & 0xff, g = (c >> 8) & 0xff, b = c & 0xff;
            if (r >= 0x70 && r <= 0xc8 && r > g && g > b && r - b >= 0x20 && r - b <= 0x70 && g - b >= 0x08 && g - b <= 0x38) ++brown;
        }
    }
    return static_cast<double>(brown) / ((rows - first) * columns);
}

MapPanel choose_map_panel(const std::uint32_t* first, const std::uint32_t* second,
                          unsigned columns, unsigned rows) noexcept {
    if (!first || !second) return MapPanel::Unknown;
    const double a = lower_brown_fraction(first, columns, rows), b = lower_brown_fraction(second, columns, rows);
    constexpr double Gap = 0.30, MapCeiling = 0.15;
    if (a <= MapCeiling && b - a >= Gap) return MapPanel::First;
    if (b <= MapCeiling && a - b >= Gap) return MapPanel::Second;
    return MapPanel::Unknown;
}

ImageDiff diff_images(const std::uint32_t* a, const std::uint32_t* b,
                      unsigned width, unsigned height) noexcept {
    ImageDiff d{};
    if (!a || !b) return d;
    d.left = width; d.top = height;
    for (unsigned y = 0; y < height; ++y) {
        for (unsigned x = 0; x < width; ++x) {
            if (a[y * width + x] == b[y * width + x]) continue;
            ++d.changed;
            if (x < d.left) d.left = x;
            if (y < d.top) d.top = y;
            if (x + 1 > d.right) d.right = x + 1;
            if (y + 1 > d.bottom) d.bottom = y + 1;
        }
    }
    if (!d.changed) d.left = d.top = 0;
    return d;
}

BlinkTracker::BlinkTracker(unsigned width, unsigned height) : width_(width), height_(height) {
    for (auto& slot : slots_) slot.image.assign(static_cast<std::size_t>(width) * height, 0);
}

void BlinkTracker::reset() noexcept {
    for (auto& slot : slots_) slot.runCount = 0;
    latest_ = toggles_ = 0;
    hasLatest_ = hasPrevious_ = false;
    since_ = 0;
}

void BlinkTracker::exclude_pause(std::uint64_t pausedSinceMs, std::uint64_t pausedMs) noexcept {
    if (hasLatest_ && since_ <= pausedSinceMs) since_ += pausedMs;
}

BlinkTracker::Event BlinkTracker::observe(const std::uint32_t* image, std::uint64_t timeMs) noexcept {
    const std::size_t count = slots_[0].image.size();
    const auto same = [&](unsigned slot) {
        return std::equal(image, image + count, slots_[slot].image.begin());
    };
    if (hasLatest_ && same(latest_)) return Event::Same;
    const auto record = [&](Slot& slot) {
        slot.runs[slot.runCount % slot.runs.size()] = timeMs - since_;
        ++slot.runCount;
    };
    if (hasPrevious_ && same(latest_ ^ 1)) {
        record(slots_[latest_]);
        latest_ ^= 1; ++toggles_; since_ = timeMs;
        return Event::Toggle;
    }
    // A third image: the old one becomes the previous; timings start over,
    // since a run that ended in something new is not a blink half-period.
    if (hasLatest_) { latest_ ^= 1; hasPrevious_ = true; }
    std::copy(image, image + count, slots_[latest_].image.begin());
    slots_[0].runCount = slots_[1].runCount = 0;
    hasLatest_ = true; toggles_ = 0; since_ = timeMs;
    return Event::NewState;
}

std::uint64_t BlinkTracker::typicalRunMs(unsigned slot) const noexcept {
    const auto& s = slots_[slot & 1];
    const unsigned n = s.runCount < s.runs.size() ? s.runCount : static_cast<unsigned>(s.runs.size());
    if (!n) return 0;
    std::array<std::uint64_t, 4> sorted{};
    std::copy(s.runs.begin(), s.runs.begin() + n, sorted.begin());
    std::sort(sorted.begin(), sorted.begin() + n);
    return sorted[n / 2];
}

ImageDiff BlinkTracker::difference() const noexcept {
    if (!hasPrevious_) return {};
    return diff_images(slots_[0].image.data(), slots_[1].image.data(), width_, height_);
}

BlinkPlan plan_blink(const BlinkTracker& tracker, std::uint64_t nowMs, const BlinkLimits& limits) noexcept {
    BlinkPlan plan{};
    plan.frozen = tracker.latest();
    plan.phaseStartMs = tracker.runStartMs();
    plan.frozenMs = tracker.typicalRunMs(plan.frozen);
    plan.otherMs = tracker.typicalRunMs(plan.frozen ^ 1);
    if (!tracker.hasPrevious()) { plan.reason = "no-other-image"; return plan; }
    if (tracker.toggles() < limits.minToggles) { plan.reason = "too-few-toggles"; return plan; }
    if (!plan.frozenMs || !plan.otherMs) { plan.reason = "no-run-timing"; return plan; }
    if (plan.frozenMs < limits.minRunMs || plan.otherMs < limits.minRunMs ||
        plan.frozenMs > limits.maxRunMs || plan.otherMs > limits.maxRunMs) { plan.reason = "run-out-of-range"; return plan; }
    // The alternation must have been going on until the pause froze it.
    if (nowMs < plan.phaseStartMs || nowMs - plan.phaseStartMs > 2 * (plan.frozenMs + plan.otherMs)) {
        plan.reason = "stale-alternation"; return plan;
    }
    plan.diff = tracker.difference();
    if (!plan.diff.changed || plan.diff.changed > limits.maxChanged ||
        plan.diff.right - plan.diff.left > limits.maxBox || plan.diff.bottom - plan.diff.top > limits.maxBox) {
        plan.reason = "difference-not-small"; return plan;
    }
    plan.valid = true;
    plan.reason = "ok";
    return plan;
}

bool blink_shows_other(const BlinkPlan& plan, std::uint64_t nowMs) noexcept {
    if (!plan.valid || nowMs < plan.phaseStartMs) return false;
    const std::uint64_t period = plan.frozenMs + plan.otherMs;
    return period && (nowMs - plan.phaseStartMs) % period >= plan.frozenMs;
}

namespace {
unsigned marker_run_ticks(std::uint64_t milliseconds) noexcept {
    // Avoid overflowing a multiplication of an absolute QPC-derived time.
    const auto seconds = milliseconds / 1000;
    if (seconds > 0xffffffffu / 60u) return 0xffffffffu;
    const auto ticks = seconds * 60 + ((milliseconds % 1000) * 60 + 500) / 1000;
    return static_cast<unsigned>(std::min<std::uint64_t>(ticks, 0xffffffffu));
}
}

MarkerCycleTracker::MarkerCycleTracker(unsigned width, unsigned height, const MarkerCycleLimits& limits)
    : width_(width), height_(height), limits_(limits),
      unionMask_(static_cast<std::size_t>(width) * height, 0) {
    for (auto& image : images_) image.resize(unionMask_.size());
}

void MarkerCycleTracker::reset() noexcept {
    ++generation_;
    latest_ = imageCount_ = runCount_ = cycleCount_ = expected_ = initialZeroTicks_ = 0;
    identityEpoch_ = since_ = lastSampleMs_ = 0;
    haveTransition_ = pauseInterrupted_ = false;
    unionDiff_ = {};
    std::fill(unionMask_.begin(), unionMask_.end(), 0);
}

void MarkerCycleTracker::start(const std::uint32_t* image, std::uint64_t timeMs,
                               std::uint64_t identityEpoch) noexcept {
    reset();
    if (!image || images_[0].empty()) return;
    std::copy(image, image + images_[0].size(), images_[0].begin());
    imageCount_ = 1;
    since_ = lastSampleMs_ = timeMs;
    identityEpoch_ = identityEpoch;
}

const std::uint32_t* MarkerCycleTracker::image(unsigned slot) const noexcept {
    return slot < imageCount_ ? images_[slot].data() : nullptr;
}

void MarkerCycleTracker::appendRun(unsigned slot, unsigned ticks) noexcept {
    if (runCount_ == HistorySize) {
        std::move(runs_.begin() + 1, runs_.end(), runs_.begin());
        --runCount_;
    }
    runs_[runCount_++] = {slot, ticks};
}

void MarkerCycleTracker::findCycle() noexcept {
    // Compare ordered occurrences and their measured duration, not a set of
    // image hashes. In particular, A-B-A-C must retain both visits to A.
    for (unsigned length = 2; length <= MaxMarkerCycleStates && length * 2 <= runCount_; ++length) {
        const unsigned first = runCount_ - length * 2, second = runCount_ - length;
        bool equal = true;
        for (unsigned i = 0; i < length; ++i) {
            if (runs_[first + i].slot != runs_[second + i].slot ||
                runs_[first + i].ticks != runs_[second + i].ticks) { equal = false; break; }
        }
        if (!equal || runs_[second].slot != latest_) continue;
        cycleCount_ = length;
        std::copy(runs_.begin() + second, runs_.begin() + runCount_, cycle_.begin());
        expected_ = 0;
        return;
    }
    // Single-lap closure for Portrait's 39-tick hardware marker cycle once
    // capture revisits the loop start after observing all distinct states.
    if (!limits_.nativePeriodTicks || imageCount_ < limits_.minSingleLapStates ||
        unionDiff_.changed < limits_.minSingleLapChanged ||
        unionDiff_.right - unionDiff_.left < limits_.minSingleLapUnionBox ||
        unionDiff_.bottom - unionDiff_.top < limits_.minSingleLapUnionBox) {
        return;
    }
    const unsigned maxLen = std::min({runCount_, imageCount_, MaxMarkerCycleStates});
    for (unsigned length = maxLen; length >= limits_.minSingleLapStates; --length) {
        const unsigned startIdx = runCount_ - length;
        if (runs_[startIdx].slot != latest_) continue;
        unsigned sumTicks = 0;
        std::uint32_t seenMask = 0;
        bool valid = true;
        for (unsigned i = 0; i < length; ++i) {
            const auto& r = runs_[startIdx + i];
            if (r.slot >= imageCount_ || (seenMask & (1u << r.slot)) != 0 ||
                r.ticks < limits_.minHoldTicks || r.ticks > limits_.maxSingleLapHoldTicks) {
                valid = false;
                break;
            }
            seenMask |= (1u << r.slot);
            sumTicks += r.ticks;
        }
        if (valid && sumTicks == limits_.nativePeriodTicks) {
            cycleCount_ = length;
            std::copy(runs_.begin() + startIdx, runs_.begin() + runCount_, cycle_.begin());
            expected_ = 0;
            return;
        }
    }
    if (latest_ == 0 && imageCount_ <= MaxMarkerCycleStates && runCount_ == imageCount_ - 1) {
        unsigned sumOther = 0;
        bool ordered = true;
        for (unsigned i = 0; i < runCount_; ++i) {
            const auto& r = runs_[i];
            if (r.slot != i + 1 || r.ticks < limits_.minHoldTicks ||
                r.ticks > limits_.maxSingleLapHoldTicks) {
                ordered = false;
                break;
            }
            sumOther += r.ticks;
        }
        if (ordered && sumOther < limits_.nativePeriodTicks) {
            const unsigned inferredZeroTicks = limits_.nativePeriodTicks - sumOther;
            if (inferredZeroTicks >= limits_.minHoldTicks &&
                inferredZeroTicks <= limits_.maxSingleLapHoldTicks &&
                initialZeroTicks_ <= inferredZeroTicks) {
                cycle_[0] = {0, inferredZeroTicks};
                std::copy(runs_.begin(), runs_.begin() + runCount_, cycle_.begin() + 1);
                cycleCount_ = imageCount_;
                expected_ = 0;
                return;
            }
        }
    }
}

MarkerCycleTracker::Event MarkerCycleTracker::observe(const std::uint32_t* image,
                                                      std::uint64_t timeMs,
                                                      std::uint64_t identityEpoch) noexcept {
    if (!image || images_[0].empty()) { reset(); return Event::Reset; }
    if (!imageCount_) { start(image, timeMs, identityEpoch); return Event::Captured; }
    if (identityEpoch != identityEpoch_ || timeMs < lastSampleMs_ ||
        timeMs - lastSampleMs_ > limits_.maxSampleGapMs) {
        start(image, timeMs, identityEpoch); return Event::Reset;
    }
    lastSampleMs_ = timeMs;
    const auto& previous = images_[latest_];
    if (std::equal(image, image + previous.size(), previous.begin())) {
        // A source that stopped animating must not retain a complete cycle.
        const unsigned maxHold = cycleCount_ ? (cycle_[expected_].ticks + (pauseInterrupted_ ? 2u : 0u)) : 0;
        if (cycleCount_ && timeMs >= since_ &&
            (timeMs - since_) * 60 > static_cast<std::uint64_t>(maxHold) * 1000 +
                                       limits_.maxSampleGapMs * 60) {
            start(image, timeMs, identityEpoch); return Event::Reset;
        }
        return Event::Same;
    }
    const auto diff = diff_images(previous.data(), image, width_, height_);
    unsigned left = diff.left, top = diff.top, right = diff.right, bottom = diff.bottom;
    if (unionDiff_.changed) {
        left = std::min(left, unionDiff_.left); top = std::min(top, unionDiff_.top);
        right = std::max(right, unionDiff_.right); bottom = std::max(bottom, unionDiff_.bottom);
    }
    const unsigned ticks = timeMs >= since_ ? marker_run_ticks(timeMs - since_) : 0;
    const bool interruptedCycleRun = cycleCount_ && pauseInterrupted_;
    if (!diff.changed || diff.changed > limits_.maxChanged ||
        diff.right - diff.left > limits_.maxTransitionBox || diff.bottom - diff.top > limits_.maxTransitionBox ||
        right - left > limits_.maxUnionBox || bottom - top > limits_.maxUnionBox ||
        (haveTransition_ && ((!interruptedCycleRun && ticks < limits_.minHoldTicks) || ticks > limits_.maxHoldTicks))) {
        start(image, timeMs, identityEpoch); return Event::Reset;
    }
    unsigned next = 0;
    for (; next < imageCount_; ++next)
        if (std::equal(image, image + previous.size(), images_[next].begin())) break;
    const unsigned previousExpected = expected_;
    if (cycleCount_) {
        const unsigned expectedNext = (expected_ + 1) % cycleCount_;
        const bool validTicks = pauseInterrupted_
                                    ? (ticks <= cycle_[expected_].ticks + 2)
                                    : (ticks == cycle_[expected_].ticks);
        if (!validTicks || next != cycle_[expectedNext].slot) {
            // A moved marker, changed background or changed sequence loses
            // confidence immediately. Old room images never feed its new plan.
            start(image, timeMs, identityEpoch); return Event::Reset;
        }
        expected_ = expectedNext;
    }
    const bool captured = next == imageCount_;
    if (captured && imageCount_ == MaxMarkerCycleStates) {
        start(image, timeMs, identityEpoch); return Event::Reset;
    }
    for (unsigned y = diff.top; y < diff.bottom; ++y) {
        for (unsigned x = diff.left; x < diff.right; ++x) {
            const auto i = static_cast<std::size_t>(y) * width_ + x;
            if (previous[i] != image[i] && !unionMask_[i]) { unionMask_[i] = 1; ++unionDiff_.changed; }
        }
    }
    unionDiff_.left = left; unionDiff_.top = top; unionDiff_.right = right; unionDiff_.bottom = bottom;
    if (haveTransition_) appendRun(latest_, interruptedCycleRun ? cycle_[previousExpected].ticks : ticks);
    else initialZeroTicks_ = ticks;
    pauseInterrupted_ = false;
    if (captured) {
        std::copy(image, image + previous.size(), images_[next].begin());
        ++imageCount_;
    }
    latest_ = next;
    since_ = timeMs;
    haveTransition_ = true;
    if (!cycleCount_) findCycle();
    return captured ? Event::Captured : Event::Revisited;
}

void MarkerCycleTracker::exclude_pause(std::uint64_t pausedSinceMs, std::uint64_t pausedMs) noexcept {
    if (!imageCount_ || !pausedMs) return;
    if (since_ <= pausedSinceMs) {
        since_ += pausedMs;
        pauseInterrupted_ = true;
    }
    if (lastSampleMs_ <= pausedSinceMs) lastSampleMs_ += pausedMs;
}

MarkerCyclePlan MarkerCycleTracker::plan(std::uint64_t frozenAtMs) const noexcept {
    MarkerCyclePlan plan{};
    plan.generation = generation_; plan.frozenSlot = latest_; plan.diff = unionDiff_;
    if (!imageCount_) return plan;
    if (frozenAtMs < since_) { plan.reason = "pause-before-current-state"; return plan; }
    if (frozenAtMs < lastSampleMs_) { plan.reason = "pause-before-last-sample"; return plan; }
    if (frozenAtMs - lastSampleMs_ > limits_.maxSampleGapMs) {
        plan.reason = "capture-gap-before-pause"; return plan;
    }
    if (!unionDiff_.changed || runCount_ < 2) { plan.reason = "insufficient-completed-runs"; return plan; }
    const auto elapsed = frozenAtMs - since_;
    const unsigned maxTicks = cycleCount_ ? (cycle_[expected_].ticks + (pauseInterrupted_ ? 2u : 0u)) : limits_.maxHoldTicks;
    if (elapsed / 1000 > static_cast<std::uint64_t>(maxTicks) / 60 + 1 ||
        elapsed * 60 > static_cast<std::uint64_t>(maxTicks) * 1000 + limits_.maxSampleGapMs * 60) {
        plan.reason = "stale-sequence"; return plan;
    }
    plan.count = cycleCount_ ? cycleCount_ : std::min({runCount_, imageCount_, MaxMarkerCycleStates});
    for (unsigned i = 0; i < plan.count; ++i) {
        const auto& run = cycleCount_ ? cycle_[(expected_ + i) % cycleCount_] : runs_[runCount_ - plan.count + i];
        plan.slots[i] = run.slot; plan.holdTicks[i] = run.ticks; plan.periodTicks += run.ticks;
    }
    plan.phaseStartMs = cycleCount_ ? since_ : frozenAtMs;
    plan.confidence = cycleCount_ ? MarkerCycleConfidence::Complete : MarkerCycleConfidence::Partial;
    plan.valid = plan.periodTicks != 0;
    plan.reason = cycleCount_ ? "complete-observed-cycle" : "partial-observed-sequence";
    return plan;
}

MarkerCyclePlan plan_marker_cycle(const MarkerCycleTracker& tracker, std::uint64_t frozenAtMs) noexcept {
    return tracker.plan(frozenAtMs);
}

MarkerCycleFrame marker_cycle_frame(const MarkerCyclePlan& plan, std::uint64_t nowMs) noexcept {
    MarkerCycleFrame frame{};
    frame.slot = plan.frozenSlot;
    if (!plan.valid || !plan.count || plan.count > MaxMarkerCycleStates || !plan.periodTicks ||
        nowMs < plan.phaseStartMs) return frame;
    // Work in millisecond*60 units. 39 ticks repeat at exactly 650 ms;
    // converting each two/three/five-frame hold to 33/50/83 ms would drift.
    const std::uint64_t periodUnits = static_cast<std::uint64_t>(plan.periodTicks) * 1000;
    const auto elapsed = nowMs - plan.phaseStartMs;
    auto units = ((elapsed % periodUnits) * 60) % periodUnits;
    for (unsigned i = 0; i < plan.count; ++i) {
        const auto holdUnits = static_cast<std::uint64_t>(plan.holdTicks[i]) * 1000;
        if (units < holdUnits) return {plan.slots[i], i, plan.holdTicks[i]};
        units -= holdUnits;
    }
    return frame;
}

void InputRecognizer::reset() noexcept {
    *this = InputRecognizer{};
}

InputResult InputRecognizer::update(const InputSnapshot& input, const InputPolicy& policy) noexcept {
    InputResult result{};
    const std::uint16_t raw = input.connected ? input.buttons : 0;
    result.forwardedButtons = raw;
    const auto reserved = static_cast<std::uint16_t>(raw & ShortcutButtons);
    const bool active = input.connected && input.focused && policy.shortcutsValidated;
    const bool changedIdentity = initialized_ && input.connectionEpoch != connectionEpoch_;
    const bool activated = !initialized_ || !wasActive_ || changedIdentity;
    const auto finish = [&](std::uint16_t suppressed) {
        result.consumedButtons = static_cast<std::uint16_t>(raw & suppressed);
        result.forwardedButtons = static_cast<std::uint16_t>(result.forwardedButtons & ~suppressed);
        previousButtons_ = raw;
        connectionEpoch_ = input.connectionEpoch;
        initialized_ = true;
        wasActive_ = active;
        return result;
    };

    if (!input.connected) {
        result.reset = initialized_;
        // Disconnection provides no evidence that physical buttons were
        // released. Keep an established reservation through reconnection.
        state_ = reservationOwned_ ? State::Quarantine : State::Idle;
        return finish(0);
    }

    if (!active) {
        result.reset = wasActive_;
        // Continue draining an already reserved gesture even if its chord
        // changed the mod mode or focus/capabilities changed mid-gesture.
        if (reservationOwned_) {
            state_ = State::Quarantine;
            if (reserved == 0) {
                state_ = State::Idle;
                reservationOwned_ = false;
            }
            return finish(ShortcutButtons);
        }
        state_ = State::Idle;
        return finish(0);
    }

    if (activated) {
        result.reset = true;
        // A held View cannot safely become a new gesture after reconnect/focus
        // changes. Y alone is ordinary gameplay input and always passes.
        const bool mustDrain = (raw & ButtonView) != 0 || (reservationOwned_ && reserved != 0);
        state_ = mustDrain ? State::Quarantine : State::Idle;
        reservationOwned_ = mustDrain;
        return finish(mustDrain ? ShortcutButtons : 0);
    }

    if (state_ == State::ChordDrain || state_ == State::Quarantine) {
        if (reserved == 0) {
            state_ = State::Idle;
            reservationOwned_ = false;
        }
        return finish(ShortcutButtons);
    }

    const bool viewDown = (raw & ButtonView) != 0;
    const bool yDown = (raw & ButtonY) != 0;
    if (state_ == State::ViewPending) {
        if (pendingGestureEpoch_ != policy.gestureEpoch || pendingEnabled_ != policy.modEnabled ||
            pendingMapAllowed_ != policy.mapAllowed) {
            result.reset = true;
            state_ = reserved ? State::Quarantine : State::Idle;
            reservationOwned_ = reserved != 0;
            return finish(ShortcutButtons);
        }
        if (!yDown) chordArmed_ = true;
        if (yDown && chordArmed_) {
            state_ = State::ChordDrain;
            result.action = InputAction::ToggleMod;
            return finish(ShortcutButtons);
        }
        if (viewDown) return finish(ButtonView);
        state_ = State::Idle;
        reservationOwned_ = false;
        if (pendingEnabled_ && pendingMapAllowed_) {
            result.action = InputAction::ToggleMap;
        } else {
            result.forwardedButtons = static_cast<std::uint16_t>(raw | ButtonView);
            result.delayedViewPulse = true;
        }
        return finish(0);
    }

    if (viewDown && (previousButtons_ & ButtonView) == 0) {
        reservationOwned_ = true;
        pendingEnabled_ = policy.modEnabled;
        pendingMapAllowed_ = policy.mapAllowed;
        pendingGestureEpoch_ = policy.gestureEpoch;
        chordArmed_ = (previousButtons_ & ButtonY) == 0;
        if (yDown && chordArmed_) {
            state_ = State::ChordDrain;
            result.action = InputAction::ToggleMod;
            return finish(ShortcutButtons);
        }
        state_ = State::ViewPending;
        return finish(ButtonView);
    }
    return finish(0);
}

PageInput PageButtonFilter::update(std::uint16_t buttons, bool connected, bool reserving,
                                   std::uint16_t reservable) noexcept {
    PageInput result{};
    if (!connected) {
        // No release evidence: keep reservations, and do not take buttons
        // already held on reconnection for new presses.
        initialized_ = false;
        return result;
    }
    if (!initialized_) {
        previous_ = buttons;
        initialized_ = true;
    }
    const auto edges = static_cast<std::uint16_t>(buttons & ~previous_);
    if (reserving) {
        const auto mask = static_cast<std::uint16_t>((reservable | PageButtons) & ~ButtonView);
        reserved_ = static_cast<std::uint16_t>(reserved_ | (edges & mask));
        result.step = ((edges & ButtonRightShoulder) ? 1 : 0) - ((edges & ButtonLeftShoulder) ? 1 : 0);
    }
    result.consumedButtons = static_cast<std::uint16_t>(buttons & reserved_);
    reserved_ = static_cast<std::uint16_t>(reserved_ & buttons);
    previous_ = buttons;
    return result;
}

unsigned step_page(unsigned page, int step, unsigned count) noexcept {
    if (!count) return page;
    const long long n = count;
    const long long next = (static_cast<long long>(page % count) + step % n + n) % n;
    return static_cast<unsigned>(next);
}

void PauseController::releaseOwned() noexcept {
    if (lease_.token == 0) {
        if (!active_) backend_ = nullptr;
        return;
    }
    if (!backend_) {
        issue_ = PauseIssue::ReleasePending;
        return;
    }
    const auto released = backend_->releasePause(lease_);
    if (released == PauseReleaseStatus::Released || released == PauseReleaseStatus::Superseded) {
        lease_ = {};
        issue_ = released == PauseReleaseStatus::Superseded ? PauseIssue::OwnershipLost : PauseIssue::None;
        if (!active_) backend_ = nullptr;
    } else {
        issue_ = PauseIssue::ReleasePending;
    }
}

bool PauseController::begin(IPauseBackend& backend) noexcept {
    if (active_) return backend_ == &backend && service();
    service();
    if (lease_.token != 0) return false;
    backend_ = &backend;
    auto state = backend.observe();
    if (!state.available) {
        issue_ = PauseIssue::Unavailable;
        backend_ = nullptr;
        return false;
    }
    if (state.paused) {
        active_ = true;
        issue_ = PauseIssue::None;
        return true;
    }
    const auto acquired = backend.acquirePause();
    if (acquired.status == PauseAcquireStatus::Acquired && acquired.lease.token != 0) {
        lease_ = acquired.lease;
        state = backend.observe();
        if (state.available && state.paused && state.revision == lease_.revision) {
            active_ = true;
            issue_ = PauseIssue::None;
            return true;
        }
        issue_ = PauseIssue::AcquireFailed;
        releaseOwned();
        return false;
    }
    if (acquired.status == PauseAcquireStatus::AlreadyPaused) {
        state = backend.observe();
        if (state.available && state.paused) {
            active_ = true;
            issue_ = PauseIssue::None;
            return true;
        }
    }
    issue_ = acquired.status == PauseAcquireStatus::Unavailable
                 ? PauseIssue::Unavailable : PauseIssue::AcquireFailed;
    backend_ = nullptr;
    return false;
}

void PauseController::end() noexcept {
    active_ = false;
    releaseOwned();
}

bool PauseController::service() noexcept {
    if (!active_) {
        if (lease_.token != 0) releaseOwned();
        return false;
    }
    if (!backend_) {
        active_ = false;
        issue_ = PauseIssue::Unavailable;
        return false;
    }
    const auto state = backend_->observe();
    if (!state.available || !state.paused) {
        end();
        if (issue_ != PauseIssue::ReleasePending)
            issue_ = state.available ? PauseIssue::OwnershipLost : PauseIssue::Unavailable;
        return false;
    }
    if (lease_.token != 0 && state.revision != lease_.revision) {
        // External pause commands supersede ownership even if paused stays true.
        // The backend is still responsible for the atomic comparison.
        releaseOwned();
        if (lease_.token != 0) {
            active_ = false;
            return false;
        }
    }
    return true;
}

void ModController::setEnabled(bool enabled) noexcept {
    if (enabled_ == enabled) return;
    enabled_ = enabled;
    ++gestureEpoch_;
    pause_.end();
}

void ModController::shutdown() noexcept {
    enabled_ = false;
    input_.reset();
    pause_.end();
}

FrameResult ModController::update(const InputSnapshot& input, const RuntimeSnapshot& runtime,
                                  IPauseBackend* pauseBackend) noexcept {
    FrameResult result{};
    const bool contextChanged = !haveRuntime_ || !same_runtime(lastRuntime_, runtime);
    if (contextChanged) {
        ++gestureEpoch_;
        if (haveRuntime_) pause_.end();
        lastRuntime_ = runtime;
        haveRuntime_ = true;
    }
    if (haveInput_ && connectionEpoch_ != input.connectionEpoch) pause_.end();
    connectionEpoch_ = input.connectionEpoch;
    haveInput_ = true;
    const bool safe = safe_context(runtime, input.focused);
    const bool gameplayReady = safe && runtime.capabilities.gameplaySurfaceValidated;
    const bool mapReady = gameplayReady && runtime.capabilities.mapSurfaceValidated &&
                          runtime.capabilities.realPauseValidated &&
                          runtime.capabilities.inputPathValidated && pauseBackend;
    if (!enabled_ || !mapReady || !input.connected ||
        (pause_.backend() && pause_.backend() != pauseBackend)) {
        pause_.end();
    }
    pause_.service();
    const bool inputValidated = title_profile(runtime.context.title).title != Title::Unknown &&
                                runtime.capabilities.title == runtime.context.title &&
                                runtime.capabilities.inputPathValidated;
    const InputPolicy policy{inputValidated, enabled_, mapReady, gestureEpoch_};
    result.input = input_.update(input, policy);
    if (result.input.action == InputAction::ToggleMod) {
        setEnabled(!enabled_);
    } else if (result.input.action == InputAction::ToggleMap) {
        if (pause_.active()) {
            pause_.end();
        } else if (!enabled_ || !mapReady || !pauseBackend || !pause_.begin(*pauseBackend)) {
            result.mapOperationRejected = true;
        }
    }
    result.modEnabled = enabled_;
    result.mapOpen = pause_.active();
    result.pauseOwned = pause_.ownsPause();
    result.pauseReleasePending = pause_.releasePending();
    result.pauseIssue = pause_.issue();
    if (enabled_ && gameplayReady && !result.pauseReleasePending) {
        result.presentation = result.mapOpen ? Presentation::Map : Presentation::Gameplay;
    }
    return result;
}

namespace {

bool same_fields(const EmuTaskTitleFields& a, const EmuTaskTitleFields& b) noexcept {
    return a.vtable == b.vtable && a.module == b.module && a.index == b.index && a.ready == b.ready;
}

bool same_image(const TitleModuleImage& a, const TitleModuleImage& b) noexcept {
    return a.handle == b.handle && a.imageSize == b.imageSize && a.timestamp == b.timestamp;
}

bool same_key(const TitleFileKey& a, const TitleFileKey& b) noexcept {
    return a.valid && b.valid && a.volume == b.volume && a.file == b.file && a.size == b.size &&
           a.written == b.written;
}

// Module table 0x14027def8 and the per-title save names (dra0N_data_*):
// a naming fact only. Of these, only Portrait has been validated in gameplay.
Title indexed_title(std::int32_t index) noexcept {
    switch (index) {
        case 0: return Title::DawnOfSorrow;
        case 1: return Title::PortraitOfRuin;
        case 2: return Title::OrderOfEcclesia;
        default: return Title::Unknown;
    }
}

// Only an enabled title is ever verified; any other index stays OtherTitle.
Title enabled_title(const TitleSet& enabled, std::int32_t index) noexcept {
    if (index == PortraitModuleIndex && enabled.portrait) return Title::PortraitOfRuin;
    if (index == DawnModuleIndex && enabled.dawn) return Title::DawnOfSorrow;
    return Title::Unknown;
}

} // namespace

const char* title_reason_name(TitleReason reason) noexcept {
    switch (reason) {
        case TitleReason::Verified: return "verified";
        case TitleReason::NoTask: return "no-task";
        case TitleReason::TaskMismatch: return "task-mismatch";
        case TitleReason::ReadFailed: return "read-failed";
        case TitleReason::NoModule: return "no-module";
        case TitleReason::OtherTitle: return "other-title";
        case TitleReason::StaticModule: return "static-module";
        case TitleReason::Loading: return "loading";
        case TitleReason::ModuleMismatch: return "module-mismatch";
        case TitleReason::ModuleImage: return "module-image";
        case TitleReason::ModuleFile: return "module-file";
        case TitleReason::Inconsistent: return "inconsistent";
    }
    return "unknown";
}

TitleObservation TitleDetector::update(ITitlePlatform& platform, std::uintptr_t observedTask,
                                       std::uint64_t taskEpoch, std::uint64_t nowMs) noexcept {
    TitleObservation next{};
    next.taskEpoch = taskEpoch;
    const auto finish = [&](TitleReason reason, Title title = Title::Unknown) {
        next.reason = reason;
        next.title = reason == TitleReason::Verified ? title : Title::Unknown;
        next.portrait = next.title == Title::PortraitOfRuin;
        if (next.title == Title::Unknown) fastValid_ = false;
        const bool changed = !haveLast_ || last_.title != next.title || last_.reason != next.reason ||
                             !same_fields(last_.fields, next.fields) || last_.taskEpoch != next.taskEpoch;
        next.generation = last_.generation + (changed ? 1 : 0);
        last_ = next;
        haveLast_ = true;
        return next;
    };

    std::uintptr_t task = 0;
    if (!platform.read_task(task) || !task) return finish(TitleReason::NoTask);
    // The task whose lifecycle epoch and pause the native observers track.
    if (task != observedTask) return finish(TitleReason::TaskMismatch);
    EmuTaskTitleFields first{};
    if (!platform.read_fields(task, first)) return finish(TitleReason::ReadFailed);
    next.fields = first;
    next.indexed = indexed_title(first.index);
    if (first.index == StaticModuleIndex) return finish(TitleReason::StaticModule);
    if (first.index < 0) return finish(TitleReason::NoModule);
    const Title wanted = enabled_title(enabled_, first.index);
    if (wanted == Title::Unknown) return finish(TitleReason::OtherTitle);
    if (!first.ready) return finish(TitleReason::Loading);
    const auto handle = platform.title_module(wanted);
    // Also rejects setEmuModuleNum(index) still pointing at the previous module.
    if (!handle || handle != first.module) return finish(TitleReason::ModuleMismatch);
    const bool unchanged = fastValid_ && fastTitle_ == wanted && fastTask_ == task && fastHandle_ == handle &&
                           fastEpoch_ == taskEpoch && same_fields(fastFields_, first);
    if (!unchanged) {
        const auto image = platform.module_image(wanted, handle);
        if (!image.valid || image.handle != handle) return finish(TitleReason::ModuleImage);
        if (!file_verified(platform, wanted, image, taskEpoch, nowMs)) return finish(TitleReason::ModuleFile);
    }
    // A load/unload racing these checks changes +0x184, +0x528 or the module.
    EmuTaskTitleFields second{};
    if (!platform.read_fields(task, second) || !same_fields(first, second) ||
        platform.title_module(wanted) != handle) {
        return finish(TitleReason::Inconsistent);
    }
    fastValid_ = true;
    fastTitle_ = wanted;
    fastTask_ = task;
    fastHandle_ = handle;
    fastEpoch_ = taskEpoch;
    fastFields_ = first;
    return finish(TitleReason::Verified, wanted);
}

bool TitleDetector::file_verified(ITitlePlatform& platform, Title title, const TitleModuleImage& image,
                                  std::uint64_t taskEpoch, std::uint64_t nowMs) noexcept {
    auto& f = files_[title == Title::DawnOfSorrow ? 1 : 0];
    const bool sameImage = f.haveImage && same_image(f.image, image);
    const bool sameEpoch = sameImage && f.keyEpoch == taskEpoch;
    if (sameEpoch && f.fileOk) return true;
    if (sameEpoch && (f.attempts >= MaxFileAttempts || nowMs < f.retryAtMs)) return false;
    if (!sameEpoch) f.attempts = 0;
    const auto key = platform.file_key(title, image);
    // A new task epoch re-checks only the cheap identity of an image already
    // verified; its content is hashed again only if that identity changed.
    if (sameImage && f.fileOk && same_key(key, f.key)) {
        f.keyEpoch = taskEpoch;
        return true;
    }
    f.image = image;
    f.haveImage = true;
    f.key = key;
    f.keyEpoch = taskEpoch;
    ++f.attempts;
    ++verifications_;
    f.fileOk = key.valid && platform.verify_file(title, image, key);
    f.retryAtMs = f.fileOk ? 0 : nowMs + FileRetryMs;
    return f.fileOk;
}

const char* game_scene_name(GameScene scene) noexcept {
    switch (scene) {
        case GameScene::Unknown: return "unknown";
        case GameScene::Boot: return "boot";
        case GameScene::Title: return "title";
        case GameScene::StartMenu: return "start-menu";
        case GameScene::Loading: return "loading";
        case GameScene::Gameplay: return "gameplay";
        case GameScene::InGameMenu: return "in-game-menu";
        case GameScene::Sequence: return "sequence";
        case GameScene::ExtraMode: return "extra-mode";
    }
    return "unknown";
}

SceneVerdict classify_portrait_scene(const PortraitStateFields& fields, bool available) noexcept {
    SceneVerdict verdict{};
    if (!available) return verdict;
    switch (fields.mode) {
        case PortraitGameplayMode:
            // Extra modes keep other sub-screens; never validated with the map.
            if (fields.gameType != 0) {
                verdict.scene = GameScene::ExtraMode;
                return verdict;
            }
            verdict.scene = GameScene::Gameplay;
            verdict.gameplay = true;
            // Fade-in, death and a sub-screen switch in progress are gameplay
            // without a map to show yet.
            verdict.mapReady = fields.gameState == PortraitRunningState &&
                               fields.page == fields.requestedPage && fields.page <= 1;
            return verdict;
        case 0x00: case 0x01: case 0x14: case 0x15:
            verdict.scene = GameScene::Loading;
            return verdict;
        case 0x05:
            verdict.scene = GameScene::Title;
            return verdict;
        case 0x0b: case 0x1c:
            verdict.scene = GameScene::Boot;
            return verdict;
        case 0x07: case 0x08: case 0x09:
            verdict.scene = GameScene::InGameMenu;
            return verdict;
        case 0x12: case 0x13: case 0x16: case 0x1a: case 0x1b:
            verdict.scene = GameScene::StartMenu;
            return verdict;
        case 0x0d: case 0x0e: case 0x0f: case 0x10: case 0x11: case 0x17:
            verdict.scene = GameScene::Sequence;
            return verdict;
        default:
            return verdict;
    }
}

bool same_dawn_fields(const DawnStateFields& a, const DawnStateFields& b) noexcept {
    return a.mode == b.mode && a.gameState == b.gameState && a.gameType == b.gameType && a.page == b.page &&
           a.requestedPage == b.requestedPage && a.pageOverlay == b.pageOverlay && a.frozen == b.frozen &&
           a.eventFlags == b.eventFlags && a.fadeFrames == b.fadeFrames && a.brightness == b.brightness;
}

SceneVerdict classify_dawn_scene(const DawnStateFields& fields, bool available) noexcept {
    SceneVerdict verdict{};
    if (!available) return verdict;
    switch (fields.mode) {
        case DawnGameplayMode: {
            // The other modes keep their own top screens; never validated.
            if (fields.gameType != 0) {
                verdict.scene = GameScene::ExtraMode;
                return verdict;
            }
            verdict.scene = GameScene::Gameplay;
            // The map/Status pair, both pages live. An event's own top-screen
            // page, the overlay, a scripted page lock or an unknown sub-state
            // is shown in the original layout instead.
            const bool pair = fields.page <= 1 && fields.requestedPage <= 1 && fields.pageOverlay == 0;
            const bool knownState = fields.gameState <= 3 || fields.gameState == 6;
            const bool single = pair && knownState && !(fields.eventFlags & DawnPageLockFlag);
            verdict.gameplay = single;
            verdict.singleScreen = single;
            // Entering, room changes, area loads, fades, a page switch in
            // progress or a moment the game would refuse its own pause menu:
            // no map yet.
            verdict.mapReady = single && fields.gameState == DawnRunningState &&
                               fields.page == fields.requestedPage && fields.frozen == 0 &&
                               fields.fadeFrames == 0 && fields.brightness == 0 &&
                               !(fields.eventFlags & DawnMenuBlockFlags);
            // The shown page is on the first bank, the upper panel: page 0
            // (map) puts the map there, page 1 (Status) in the lower one.
            if (verdict.mapReady) verdict.mapPanel = fields.page;
            return verdict;
        }
        case 0x00: case 0x01:
            verdict.scene = GameScene::Loading;
            return verdict;
        case 0x03:
            verdict.scene = GameScene::Title;
            return verdict;
        case 0x0a:
            verdict.scene = GameScene::InGameMenu;
            return verdict;
        case 0x05: case 0x16:
            verdict.scene = GameScene::Sequence;
            return verdict;
        default:
            return verdict;
    }
}

SceneVerdict classify_scene(const TitleSceneFields& fields, bool available) noexcept {
    switch (fields.title) {
        case Title::PortraitOfRuin: return classify_portrait_scene(fields.portrait, available);
        case Title::DawnOfSorrow: return classify_dawn_scene(fields.dawn, available);
        default: return {};
    }
}

MapCommand map_command(const MapCommandInput& input) noexcept {
    if (input.mapOpen && input.owned) return MapCommand::CloseVoluntary;
    if (input.owned || input.choosing) return MapCommand::Cancel;
    if (input.paused) return MapCommand::None;
    return input.mapReady ? MapCommand::Open : MapCommand::Refuse;
}

TitleGateState TitleGate::update(const TitleGateInput& input) noexcept {
    const Title title = input.title.title;
    if (title != Title::Unknown) {
        if (streak_ && (title_ != title || module_ != input.title.fields.module || epoch_ != input.title.taskEpoch))
            streak_ = 0;
        if (streak_ < Confirmations) ++streak_;
        title_ = title;
        module_ = input.title.fields.module;
        epoch_ = input.title.taskEpoch;
    } else {
        streak_ = 0;
    }
    TitleGateState state{};
    state.identity = streak_ >= Confirmations;
    state.input = state.identity && input.pauseKnown && (!input.paused || input.modPause);
    state.gameplay = state.identity && input.scene.gameplay;
    state.mapReady = state.gameplay && state.input && input.scene.mapReady;
    state.identityChanged = state.identity != identity_;
    state.inputChanged = state.input != input_;
    state.gameplayChanged = state.gameplay != gameplay_;
    state.title = state.identity ? title_ : Title::Unknown;
    identity_ = state.identity;
    input_ = state.input;
    gameplay_ = state.gameplay;
    return state;
}

} // namespace dominus
