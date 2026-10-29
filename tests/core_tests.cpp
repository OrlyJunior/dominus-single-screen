#include "dominus/core.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {
using namespace dominus;

constexpr std::uint16_t ButtonA = 0x1000;
constexpr std::uint16_t ButtonB = 0x2000;

static_assert(!std::is_copy_constructible<PauseController>::value, "Pause leases must have one owner");
static_assert(!std::is_move_constructible<PauseController>::value, "Default move must not duplicate pause leases");
static_assert(!std::is_copy_constructible<ModController>::value, "Coordinator owns pause state");
static_assert(!std::is_move_constructible<ModController>::value, "Coordinator owns pause state");

#define CHECK(condition) do { if (!(condition)) throw std::runtime_error( \
    std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": " #condition); } while (false)

bool near(double a, double b) { return std::abs(a - b) < 0.000001; }

struct InputFixture {
    InputRecognizer recognizer;
    InputSnapshot snapshot{};
    InputPolicy policy{true, true, true, 1};
    InputFixture() { recognizer.update(snapshot, policy); }
    InputResult poll(std::uint16_t buttons) {
        snapshot.buttons = buttons;
        return recognizer.update(snapshot, policy);
    }
};

struct FakePauseBackend final : IPauseBackend {
    bool available = true;
    bool paused = false;
    bool failAcquire = false;
    bool failRelease = false;
    bool raceToAlreadyPaused = false;
    bool supersedeDuringAcquire = false;
    bool unavailableAfterAcquire = false;
    bool reportFalseAfterAcquire = false;
    std::uint64_t revision = 1;
    std::uint64_t nextToken = 0;
    std::uint64_t activeToken = 0;
    int acquireCalls = 0;
    int releaseCalls = 0;
    int actualResumes = 0;

    PauseState observe() noexcept override { return {available, paused, revision}; }
    void externalPause(bool value) noexcept {
        paused = value;
        ++revision;
        activeToken = 0;
    }
    PauseAcquisition acquirePause() noexcept override {
        ++acquireCalls;
        if (!available) return {PauseAcquireStatus::Unavailable, {}};
        if (failAcquire) return {PauseAcquireStatus::Failed, {}};
        if (raceToAlreadyPaused) externalPause(true);
        if (paused) return {PauseAcquireStatus::AlreadyPaused, {}};
        paused = true;
        ++revision;
        activeToken = ++nextToken;
        const PauseLease lease{activeToken, revision};
        if (supersedeDuringAcquire) externalPause(true);
        if (unavailableAfterAcquire) available = false;
        if (reportFalseAfterAcquire) externalPause(false);
        return {PauseAcquireStatus::Acquired, lease};
    }
    PauseReleaseStatus releasePause(PauseLease lease) noexcept override {
        ++releaseCalls;
        if (!available) return PauseReleaseStatus::Unavailable;
        if (failRelease) return PauseReleaseStatus::Failed;
        if (!lease.token || lease.token != activeToken || lease.revision != revision)
            return PauseReleaseStatus::Superseded;
        paused = false;
        activeToken = 0;
        ++revision;
        ++actualResumes;
        return PauseReleaseStatus::Released;
    }
};

struct ModFixture {
    ModController controller;
    RuntimeSnapshot runtime{{Title::PortraitOfRuin, Context::Exploration, true, 1},
                            {Title::PortraitOfRuin, true, true, true, true}};
    FakePauseBackend pause;
    InputSnapshot input{};
    explicit ModFixture(bool enabled = true) : controller(enabled) { poll(0); }
    FrameResult poll(std::uint16_t buttons) {
        input.buttons = buttons;
        return controller.update(input, runtime, &pause);
    }
    FrameResult view() {
        CHECK(poll(ButtonView).input.action == InputAction::None);
        return poll(0);
    }
};

void geometry_and_touch() {
    const auto fullHD = fit_4_3(1920, 1080);
    CHECK(fullHD.valid && fullHD.left == 240 && fullHD.top == 0);
    CHECK(fullHD.width == 1440 && fullHD.height == 1080);
    CHECK(near(fullHD.scale, 5.625));
    CHECK(!fullHD.integerApplied);
    const auto integer = fit_4_3(1920, 1080, ScaleMode::Integer);
    CHECK(integer.left == 320 && integer.top == 60);
    CHECK(integer.width == 1280 && integer.height == 960);
    CHECK(integer.integerApplied && near(integer.scale, 5.0));
    const auto wide = fit_4_3(2560, 1080);
    CHECK(wide.left == 560 && wide.width == 1440 && wide.height == 1080);
    const auto odd = fit_4_3(1365, 767);
    CHECK(odd.left == 172 && odd.top == 1 && odd.width == 1020 && odd.height == 765);
    const auto small = fit_4_3(128, 96, ScaleMode::Integer);
    CHECK(small.valid && !small.integerApplied && near(small.scale, 0.5));
    CHECK(!fit_4_3(0, 1080).valid);
    CHECK(!fit_4_3(1920, -1).valid);
    CHECK(!fit_4_3(3, 2).valid);
    const auto large = fit_4_3(std::numeric_limits<int>::max(), std::numeric_limits<int>::max());
    CHECK(large.valid && large.width <= std::numeric_limits<int>::max());
    CHECK(static_cast<std::int64_t>(large.width) * 3 == static_cast<std::int64_t>(large.height) * 4);
    const auto center = screen_to_native(fullHD, 960.0, 540.0);
    CHECK(center.inside && near(center.x, 128.0) && near(center.y, 96.0));
    CHECK(screen_to_native(fullHD, 240.0, 0.0).inside);
    CHECK(!screen_to_native(fullHD, 239.9, 540).inside);
    CHECK(!screen_to_native(fullHD, 1680.0, 540).inside);
    CHECK(!screen_to_native(fullHD, 960, 1080).inside);
    CHECK(!screen_to_native(fullHD, std::numeric_limits<double>::infinity(), 0).inside);
    CHECK(!screen_to_native(fullHD, 0, std::numeric_limits<double>::quiet_NaN()).inside);
    CHECK(!screen_to_native({}, 0, 0).inside);
}

void title_profiles_require_runtime_validation() {
    for (const auto title : {Title::Unknown, Title::PortraitOfRuin, Title::DawnOfSorrow, Title::OrderOfEcclesia}) {
        const auto& profile = title_profile(title);
        CHECK(profile.title == title);
        CHECK(profile.nativeWidth == 256 && profile.nativeHeight == 192);
        CHECK(!profile.runtimeValidated);
    }
    CHECK(title_profile(static_cast<Title>(999)).title == Title::Unknown);
}

void view_release_and_repeated_polls() {
    InputFixture f;
    auto result = f.poll(ButtonView | ButtonA);
    CHECK(result.action == InputAction::None && result.forwardedButtons == ButtonA);
    CHECK(result.consumedButtons == ButtonView);
    for (int i = 0; i < 100; ++i) {
        result = f.poll(ButtonView | ButtonA);
        CHECK(result.action == InputAction::None && result.forwardedButtons == ButtonA);
    }
    result = f.poll(ButtonA);
    CHECK(result.action == InputAction::ToggleMap && !result.delayedViewPulse);
    CHECK(result.forwardedButtons == ButtonA);
    for (int i = 0; i < 10; ++i) CHECK(f.poll(ButtonA).action == InputAction::None);
    CHECK(f.poll(ButtonY | ButtonB).forwardedButtons == (ButtonY | ButtonB));
}

void disabled_and_unsupported_view_is_replayed_once() {
    for (int variant = 0; variant < 2; ++variant) {
        InputFixture f;
        if (variant == 0) f.policy.modEnabled = false;
        else f.policy.mapAllowed = false;
        CHECK(f.poll(ButtonView).forwardedButtons == 0);
        CHECK(f.poll(ButtonView).forwardedButtons == 0);
        auto result = f.poll(ButtonA);
        CHECK(result.delayedViewPulse && result.action == InputAction::None);
        CHECK(result.forwardedButtons == (ButtonView | ButtonA));
        result = f.poll(ButtonA);
        CHECK(!result.delayedViewPulse && result.forwardedButtons == ButtonA);
        CHECK(f.poll(ButtonY).forwardedButtons == ButtonY);
    }
}

void chords_toggle_once_and_drain_across_mode_changes() {
    for (bool simultaneous : {false, true}) {
        InputFixture f;
        if (!simultaneous) CHECK(f.poll(ButtonView).action == InputAction::None);
        auto result = f.poll(ButtonView | ButtonY | ButtonA);
        CHECK(result.action == InputAction::ToggleMod);
        CHECK(result.forwardedButtons == ButtonA && result.consumedButtons == ShortcutButtons);
        f.policy.modEnabled = false;
        for (int i = 0; i < 10; ++i) {
            result = f.poll(ButtonView | ButtonY | ButtonA);
            CHECK(result.action == InputAction::None && result.forwardedButtons == ButtonA);
        }
        CHECK(f.poll(ButtonY | ButtonB).forwardedButtons == ButtonB);
        CHECK(f.poll(ButtonView | ButtonY).forwardedButtons == 0);
        CHECK(f.poll(ButtonView).forwardedButtons == 0);
        CHECK(f.poll(0).action == InputAction::None);
        CHECK(f.poll(ButtonY).forwardedButtons == ButtonY);
    }
}

void y_before_view_is_not_retroactively_consumed() {
    InputFixture f;
    CHECK(f.poll(ButtonY).forwardedButtons == ButtonY);
    auto result = f.poll(ButtonView | ButtonY);
    CHECK(result.action == InputAction::None && result.forwardedButtons == ButtonY);
    CHECK(f.poll(ButtonView | ButtonY).action == InputAction::None);
    CHECK(f.poll(ButtonView).action == InputAction::None);
    result = f.poll(ButtonView | ButtonY);
    CHECK(result.action == InputAction::ToggleMod && result.forwardedButtons == 0);
    f.poll(0);
    CHECK(f.poll(ButtonY).forwardedButtons == ButtonY);
    CHECK(f.poll(ButtonView | ButtonY).action == InputAction::None);
    result = f.poll(ButtonY);
    CHECK(result.action == InputAction::ToggleMap && result.forwardedButtons == ButtonY);
}

void release_rollover_with_new_y_does_not_leak_native_combo() {
    for (bool enabled : {false, true}) {
        InputFixture f;
        f.policy.modEnabled = enabled;
        f.poll(ButtonView);
        const auto result = f.poll(ButtonY | ButtonA);
        CHECK(result.action == InputAction::ToggleMod);
        CHECK(!result.delayedViewPulse && result.forwardedButtons == ButtonA);
        f.policy.modEnabled = !enabled;
        CHECK(f.poll(ButtonY).forwardedButtons == 0);
        CHECK(f.poll(0).action == InputAction::None);
    }
}

void pending_gestures_cancel_at_context_and_mode_boundaries() {
    for (int variant = 0; variant < 3; ++variant) {
        InputFixture f;
        f.poll(ButtonView);
        if (variant == 0) ++f.policy.gestureEpoch;
        if (variant == 1) f.policy.mapAllowed = false;
        if (variant == 2) f.policy.modEnabled = false;
        const auto canceled = f.poll(ButtonView | ButtonY | ButtonA);
        CHECK(canceled.reset && canceled.action == InputAction::None);
        CHECK(canceled.forwardedButtons == ButtonA);
        const auto released = f.poll(0);
        CHECK(released.action == InputAction::None && !released.delayedViewPulse);
        CHECK(f.poll(ButtonY).forwardedButtons == ButtonY);
    }
}

void focus_disconnect_and_controller_identity() {
    InputFixture f;
    f.poll(ButtonView);
    f.snapshot.focused = false;
    CHECK(f.poll(ButtonView | ButtonY).action == InputAction::None);
    f.snapshot.focused = true;
    CHECK(f.poll(ButtonView | ButtonY).action == InputAction::None);
    CHECK(f.poll(0).action == InputAction::None);
    CHECK(f.poll(ButtonView | ButtonY).action == InputAction::ToggleMod);
    f.snapshot.connected = false;
    CHECK(f.poll(ShortcutButtons | ButtonA).forwardedButtons == 0);
    f.snapshot.connected = true;
    ++f.snapshot.connectionEpoch;
    auto result = f.poll(ShortcutButtons | ButtonA);
    CHECK(result.action == InputAction::None && result.forwardedButtons == ButtonA);
    CHECK(f.poll(0).action == InputAction::None);
    f.poll(ButtonView);
    ++f.snapshot.connectionEpoch;
    CHECK(f.poll(ShortcutButtons).action == InputAction::None);
    CHECK(f.poll(0).action == InputAction::None);
    CHECK(f.poll(ShortcutButtons).action == InputAction::ToggleMod);
    f.snapshot.connected = false;
    f.poll(0);
    f.snapshot.connected = true;
    CHECK(f.poll(ButtonY).forwardedButtons == 0);
    f.poll(0);
    CHECK(f.poll(ButtonY).forwardedButtons == ButtonY);

    InputRecognizer initiallyHeld;
    InputPolicy policy{true, true, true, 1};
    CHECK(initiallyHeld.update({ButtonView, true, true, 0}, policy).action == InputAction::None);
    CHECK(initiallyHeld.update({0, true, true, 0}, policy).action == InputAction::None);
    InputRecognizer initialY;
    CHECK(initialY.update({ButtonY, true, true, 0}, policy).forwardedButtons == ButtonY);
}

void unvalidated_input_is_passthrough() {
    InputRecognizer recognizer;
    InputPolicy policy{};
    for (const auto buttons : {ButtonA, ButtonY, ButtonView, ShortcutButtons, std::uint16_t(0)}) {
        const auto result = recognizer.update({buttons, true, true, 0}, policy);
        CHECK(result.forwardedButtons == buttons && result.consumedButtons == 0);
        CHECK(result.action == InputAction::None && !result.delayedViewPulse);
    }
    policy.shortcutsValidated = true;
    policy.modEnabled = true;
    policy.mapAllowed = true;
    CHECK(recognizer.update({ShortcutButtons, true, true, 0}, policy).action == InputAction::None);
    CHECK(recognizer.update({0, true, true, 0}, policy).action == InputAction::None);
    CHECK(recognizer.update({ShortcutButtons, true, true, 0}, policy).action == InputAction::ToggleMod);
}

void exhaustive_short_input_sequences_preserve_other_buttons() {
    // All 4^6 sequences of reserved-button states, with non-shortcut buttons
    // changing independently. This probes transitions rather than mirroring code.
    constexpr std::uint16_t states[] = {0, ButtonView, ButtonY, ShortcutButtons};
    for (unsigned sequence = 0; sequence < 4096; ++sequence) {
        InputFixture f;
        unsigned remaining = sequence;
        for (unsigned step = 0; step < 6; ++step) {
            const auto other = static_cast<std::uint16_t>((step & 1) ? ButtonA : ButtonB);
            const auto raw = static_cast<std::uint16_t>(states[remaining & 3] | other);
            remaining >>= 2;
            const auto result = f.poll(raw);
            CHECK((result.forwardedButtons & ~ShortcutButtons) == other);
            CHECK((result.consumedButtons & ~ShortcutButtons) == 0);
            if (result.action == InputAction::ToggleMod) {
                CHECK((raw & ButtonY) != 0);
                CHECK((result.forwardedButtons & ShortcutButtons) == 0);
                f.policy.modEnabled = !f.policy.modEnabled;
            }
            if (result.action == InputAction::ToggleMap) CHECK((raw & ButtonView) == 0);
            if (result.delayedViewPulse) {
                CHECK(result.action == InputAction::None);
                CHECK((result.forwardedButtons & ButtonView) != 0);
            }
        }
    }
}

void owned_pause_and_idempotent_close() {
    FakePauseBackend backend;
    PauseController pause;
    CHECK(pause.begin(backend) && pause.active() && pause.ownsPause());
    CHECK(backend.paused && backend.acquireCalls == 1);
    CHECK(pause.begin(backend) && backend.acquireCalls == 1);
    CHECK(pause.service());
    pause.end();
    CHECK(!pause.active() && !pause.ownsPause() && !pause.releasePending());
    CHECK(!backend.paused && backend.actualResumes == 1);
    const auto releases = backend.releaseCalls;
    pause.end();
    pause.service();
    CHECK(backend.releaseCalls == releases && backend.actualResumes == 1);
}

void preexisting_pause_is_never_owned_or_resumed() {
    FakePauseBackend backend;
    backend.externalPause(true);
    PauseController pause;
    CHECK(pause.begin(backend) && pause.active() && !pause.ownsPause());
    CHECK(backend.acquireCalls == 0);
    pause.end();
    CHECK(backend.paused && backend.releaseCalls == 0 && backend.actualResumes == 0);

    backend.externalPause(false);
    backend.raceToAlreadyPaused = true;
    CHECK(pause.begin(backend) && !pause.ownsPause());
    pause.end();
    CHECK(backend.paused && backend.actualResumes == 0);
}

void external_pause_requests_supersede_ownership() {
    for (bool externalValue : {false, true}) {
        FakePauseBackend backend;
        PauseController pause;
        CHECK(pause.begin(backend));
        backend.externalPause(externalValue);
        CHECK(pause.service() == externalValue);
        CHECK(!pause.ownsPause());
        pause.end();
        CHECK(backend.paused == externalValue && backend.actualResumes == 0);
        CHECK(!pause.releasePending());
    }
}

void failed_releases_are_retained_and_retried_safely() {
    FakePauseBackend backend;
    PauseController pause;
    CHECK(pause.begin(backend));
    backend.failRelease = true;
    pause.end();
    CHECK(!pause.active() && pause.releasePending() && backend.paused);
    CHECK(pause.issue() == PauseIssue::ReleasePending);
    CHECK(!pause.begin(backend));
    backend.available = false;
    CHECK(!pause.service() && pause.releasePending());
    backend.available = true;
    backend.failRelease = false;
    CHECK(!pause.service());
    CHECK(!pause.releasePending() && !backend.paused && backend.actualResumes == 1);

    CHECK(pause.begin(backend));
    backend.failRelease = true;
    pause.end();
    backend.externalPause(true);
    backend.failRelease = false;
    pause.service();
    CHECK(!pause.releasePending() && backend.paused && backend.actualResumes == 1);
}

void acquire_failures_cannot_produce_a_fake_pause() {
    {
        FakePauseBackend backend;
        PauseController pause;
        backend.available = false;
        CHECK(!pause.begin(backend) && !pause.active());
        CHECK(backend.acquireCalls == 0);
        backend.available = true;
        backend.failAcquire = true;
        CHECK(!pause.begin(backend) && !backend.paused && !pause.active());
    }
    {
        FakePauseBackend backend;
        PauseController pause;
        backend.supersedeDuringAcquire = true;
        CHECK(!pause.begin(backend) && !pause.active());
        CHECK(backend.paused && backend.actualResumes == 0 && !pause.releasePending());
    }
    {
        FakePauseBackend backend;
        PauseController pause;
        backend.unavailableAfterAcquire = true;
        CHECK(!pause.begin(backend) && !pause.active() && pause.releasePending());
        backend.available = true;
        pause.service();
        CHECK(!pause.releasePending() && !backend.paused && backend.actualResumes == 1);
    }
    {
        FakePauseBackend backend;
        PauseController pause;
        backend.reportFalseAfterAcquire = true;
        CHECK(!pause.begin(backend) && !pause.active() && !backend.paused);
        CHECK(backend.actualResumes == 0);
    }
}

void lost_pause_availability_closes_map_without_blind_resume() {
    FakePauseBackend backend;
    PauseController pause;
    CHECK(pause.begin(backend));
    backend.available = false;
    CHECK(!pause.service() && !pause.active() && pause.releasePending());
    CHECK(backend.paused && backend.actualResumes == 0);
    backend.externalPause(true);
    backend.available = true;
    pause.service();
    CHECK(!pause.releasePending() && backend.paused && backend.actualResumes == 0);
}

void coordinator_defaults_fail_open() {
    ModController controller(true);
    RuntimeSnapshot unknown{};
    auto result = controller.update({0, true, true, 0}, unknown);
    CHECK(result.presentation == Presentation::Original && !result.mapOpen);
    result = controller.update({ShortcutButtons, true, true, 0}, unknown);
    CHECK(result.input.forwardedButtons == ShortcutButtons);
    CHECK(result.input.action == InputAction::None);

    RuntimeSnapshot invalid{{static_cast<Title>(999), Context::Exploration, true, 1},
                            {static_cast<Title>(999), true, true, true, true}};
    FakePauseBackend unusedBackend;
    result = controller.update({0, true, true, 0}, invalid, &unusedBackend);
    CHECK(result.presentation == Presentation::Original && unusedBackend.acquireCalls == 0);

    ModFixture f;
    f.runtime.capabilities.realPauseValidated = false;
    CHECK(f.poll(0).presentation == Presentation::Gameplay);
    result = f.view();
    CHECK(!result.mapOpen && result.input.delayedViewPulse);
    CHECK(f.pause.acquireCalls == 0);
    f.runtime.capabilities.gameplaySurfaceValidated = false;
    CHECK(f.poll(0).presentation == Presentation::Original);
    f.runtime.capabilities.title = Title::DawnOfSorrow;
    CHECK(f.poll(ShortcutButtons).input.forwardedButtons == ShortcutButtons);
}

void coordinator_map_and_global_toggle() {
    ModFixture f(false);
    CHECK(f.poll(0).presentation == Presentation::Original);
    auto result = f.view();
    CHECK(result.input.delayedViewPulse && !result.mapOpen);
    result = f.poll(ShortcutButtons | ButtonA);
    CHECK(result.input.action == InputAction::ToggleMod && result.modEnabled);
    CHECK(result.presentation == Presentation::Gameplay && result.input.forwardedButtons == ButtonA);
    CHECK(f.poll(ShortcutButtons).input.forwardedButtons == 0);
    CHECK(f.poll(ButtonY).input.forwardedButtons == 0);
    f.poll(0);
    result = f.view();
    CHECK(result.mapOpen && result.pauseOwned && f.pause.paused);
    CHECK(result.presentation == Presentation::Map);
    result = f.poll(ShortcutButtons);
    CHECK(!result.modEnabled && !result.mapOpen && !f.pause.paused);
    CHECK(result.presentation == Presentation::Original && result.input.forwardedButtons == 0);
    CHECK(f.poll(ButtonY).input.forwardedButtons == 0);
    f.poll(0);
    CHECK(f.poll(ButtonY).input.forwardedButtons == ButtonY);
    CHECK(f.pause.actualResumes == 1);
}

void coordinator_falls_back_for_special_scenes_and_identity_changes() {
    for (const auto context : {Context::Unknown, Context::Menu, Context::Dialogue,
                               Context::Cutscene, Context::Touchscreen, Context::Transition}) {
        ModFixture f;
        CHECK(f.view().mapOpen);
        f.runtime.context.scene = context;
        const auto result = f.poll(0);
        CHECK(!result.mapOpen && result.presentation == Presentation::Original);
        CHECK(!f.pause.paused && f.pause.actualResumes == 1);
    }
    ModFixture f;
    CHECK(f.view().mapOpen);
    ++f.runtime.context.epoch;
    CHECK(!f.poll(0).mapOpen && !f.pause.paused);
    f.poll(ButtonView);
    f.runtime.context.scene = Context::Menu;
    const auto result = f.poll(0);
    CHECK(result.input.action == InputAction::None && !result.input.delayedViewPulse);

    ModFixture modeBoundary;
    modeBoundary.poll(ButtonView);
    modeBoundary.controller.setEnabled(false);
    modeBoundary.controller.setEnabled(true);
    const auto canceled = modeBoundary.poll(0);
    CHECK(canceled.input.action == InputAction::None && !canceled.mapOpen);
}

void coordinator_pause_failure_and_external_changes() {
    ModFixture failed;
    failed.pause.failAcquire = true;
    auto result = failed.view();
    CHECK(result.mapOperationRejected && !result.mapOpen && !failed.pause.paused);
    CHECK(result.presentation == Presentation::Gameplay);

    ModFixture f;
    CHECK(f.view().mapOpen);
    f.pause.externalPause(false);
    result = f.poll(0);
    CHECK(!result.mapOpen && !result.pauseOwned && f.pause.actualResumes == 0);
    CHECK(result.presentation == Presentation::Gameplay);
    CHECK(f.view().mapOpen);
    f.pause.externalPause(true);
    result = f.poll(0);
    CHECK(result.mapOpen && !result.pauseOwned && f.pause.paused);
    result = f.view();
    CHECK(!result.mapOpen && f.pause.paused && f.pause.actualResumes == 0);
}

void coordinator_focus_reconnect_release_pending_and_shutdown() {
    ModFixture f;
    CHECK(f.view().mapOpen);
    f.input.focused = false;
    auto result = f.poll(0);
    CHECK(!result.mapOpen && result.presentation == Presentation::Original && !f.pause.paused);
    f.input.focused = true;
    f.poll(0);
    CHECK(f.view().mapOpen);
    ++f.input.connectionEpoch;
    CHECK(!f.poll(0).mapOpen && !f.pause.paused);
    CHECK(f.view().mapOpen);
    f.input.connected = false;
    CHECK(!f.poll(0).mapOpen && !f.pause.paused);
    f.input.connected = true;
    ++f.input.connectionEpoch;
    f.poll(0);
    CHECK(f.view().mapOpen);
    f.pause.failRelease = true;
    result = f.poll(ShortcutButtons);
    CHECK(!result.modEnabled && result.pauseReleasePending);
    CHECK(result.presentation == Presentation::Original && f.pause.paused);
    f.pause.failRelease = false;
    result = f.poll(0);
    CHECK(!result.pauseReleasePending && !f.pause.paused);
    f.controller.setEnabled(true);
    f.poll(0);
    CHECK(f.view().mapOpen);
    f.controller.shutdown();
    CHECK(!f.controller.enabled() && !f.controller.mapOpen() && !f.pause.paused);
}

void coordinator_shared_profiles_never_imply_support() {
    for (const auto title : {Title::PortraitOfRuin, Title::DawnOfSorrow, Title::OrderOfEcclesia}) {
        ModFixture f;
        f.runtime.context.title = title;
        f.runtime.capabilities.title = title;
        f.poll(0);
        CHECK(f.poll(0).presentation == Presentation::Gameplay);
        CHECK(f.view().mapOpen);
        f.controller.shutdown();
        CHECK(!f.pause.paused);
        f.runtime.context.validated = false;
        f.controller.setEnabled(true);
        CHECK(f.poll(0).presentation == Presentation::Original);
    }
}

} // namespace

void map_panel_is_the_one_without_the_brown_enemy_panel() {
    constexpr unsigned n = PanelColumns * PanelRows;
    std::uint32_t status[n], charlotte[n], map[n], blank[n];
    for (unsigned i = 0; i < n; ++i) {
        const unsigned row = i / PanelColumns, column = i % PanelColumns;
        const bool lower = row >= PanelRows - PanelRows * 5 / 12;
        // Colours sampled from the user's screenshots: enemy panel frame/fields,
        // black portrait box, teal drop bars; Jonathan pink / Charlotte blue top.
        std::uint32_t c = 0xa21869;
        if (lower) c = column < 4 ? 0x000000 : (row == PanelRows - 4 ? 0x528495 : (row % 2 ? 0x95735a : 0x835140));
        status[i] = c;
        charlotte[i] = lower ? c : 0x10508f;
        map[i] = (row + column) % 5 ? 0x2a6a7a : 0xa04090;  // teal background, magenta rooms
        blank[i] = 0x000000;
    }
    CHECK(lower_brown_fraction(status, PanelColumns, PanelRows) > 0.5);
    CHECK(lower_brown_fraction(charlotte, PanelColumns, PanelRows) > 0.5);
    CHECK(lower_brown_fraction(map, PanelColumns, PanelRows) == 0.0);
    CHECK(choose_map_panel(map, status, PanelColumns, PanelRows) == MapPanel::First);
    CHECK(choose_map_panel(charlotte, map, PanelColumns, PanelRows) == MapPanel::Second);
    // No clear difference (both Status, both blank, missing data): undecided.
    CHECK(choose_map_panel(status, charlotte, PanelColumns, PanelRows) == MapPanel::Unknown);
    CHECK(choose_map_panel(blank, map, PanelColumns, PanelRows) == MapPanel::Unknown);
    CHECK(choose_map_panel(nullptr, map, PanelColumns, PanelRows) == MapPanel::Unknown);
    CHECK(lower_brown_fraction(status, 0, 0) == 0.0);
}

// Tiny 8x6 "panels": background 1, marker pixels 9, a revealed room 5.
struct Frames {
    static constexpr unsigned W = 8, H = 6;
    std::uint32_t on[W * H], off[W * H], moved[W * H], movedOff[W * H], big[W * H];
    Frames() {
        for (unsigned i = 0; i < W * H; ++i) on[i] = off[i] = moved[i] = movedOff[i] = big[i] = 1;
        on[2 * W + 3] = on[2 * W + 4] = 9;           // marker in the first room
        moved[4 * W + 6] = 9; moved[4 * W + 5] = 5;   // marker in a new room
        movedOff[4 * W + 5] = 5;
        for (unsigned i = 0; i < W * H; ++i) big[i] = i % 3 ? 7 : 1;  // most of the panel
    }
};

void image_difference_counts_and_bounds() {
    Frames f;
    const auto d = diff_images(f.on, f.off, Frames::W, Frames::H);
    CHECK(d.changed == 2 && d.left == 3 && d.right == 5 && d.top == 2 && d.bottom == 3);
    const auto none = diff_images(f.on, f.on, Frames::W, Frames::H);
    CHECK(none.changed == 0 && none.left == 0 && none.right == 0 && none.top == 0 && none.bottom == 0);
    CHECK(diff_images(nullptr, f.on, Frames::W, Frames::H).changed == 0);
}

void blink_tracker_follows_strict_alternation() {
    Frames f;
    BlinkTracker t(Frames::W, Frames::H);
    using E = BlinkTracker::Event;
    CHECK(t.observe(f.on, 0) == E::NewState && !t.hasPrevious());
    CHECK(t.observe(f.on, 16) == E::Same);
    CHECK(t.observe(f.off, 100) == E::NewState && t.hasPrevious() && t.toggles() == 0);
    const unsigned offSlot = t.latest();
    CHECK(t.observe(f.on, 300) == E::Toggle && t.toggles() == 1 && t.latest() != offSlot);
    CHECK(t.observe(f.off, 400) == E::Toggle && t.latest() == offSlot);
    CHECK(t.observe(f.on, 600) == E::Toggle && t.toggles() == 3 && t.runStartMs() == 600);
    // Runs: "off" lasted 200 (100-300) and 200 (400-600), "on" 100 (300-400).
    CHECK(t.typicalRunMs(offSlot) == 200 && t.typicalRunMs(offSlot ^ 1) == 100);
    CHECK(t.difference().changed == 2);
    // Moving to a new room: a third image ends the pair and its timings.
    CHECK(t.observe(f.moved, 650) == E::NewState && t.toggles() == 0);
    CHECK(t.typicalRunMs(0) == 0 && t.typicalRunMs(1) == 0);
    // The previous image is now "on" in the old room, not the new pair.
    CHECK(std::equal(f.on, f.on + Frames::W * Frames::H, t.image(t.latest() ^ 1)));
    t.reset();
    CHECK(!t.hasPrevious() && t.toggles() == 0 && t.observe(f.on, 700) == E::NewState);
}

void blink_plan_accepts_only_a_live_small_alternation() {
    Frames f;
    using E = BlinkTracker::Event;
    auto alternating = [&](BlinkTracker& t, const std::uint32_t* a, const std::uint32_t* b, unsigned changes) {
        std::uint64_t time = 0;
        for (unsigned i = 0; i <= changes; ++i, time += 250) t.observe(i % 2 ? b : a, time);
        return time - 250;  // start of the last run
    };
    BlinkTracker t(Frames::W, Frames::H);
    const auto last = alternating(t, f.on, f.off, 5);
    // Frozen while showing "off" (last run started at `last`).
    auto plan = plan_blink(t, last + 100);
    CHECK(plan.valid && std::string(plan.reason) == "ok");
    CHECK(plan.frozenMs == 250 && plan.otherMs == 250 && plan.phaseStartMs == last);
    CHECK(std::equal(f.off, f.off + Frames::W * Frames::H, t.image(plan.frozen)));
    // Schedule: frozen until last+250, other until last+500, frozen again.
    CHECK(!blink_shows_other(plan, last + 100) && !blink_shows_other(plan, last + 249));
    CHECK(blink_shows_other(plan, last + 250) && blink_shows_other(plan, last + 499));
    CHECK(!blink_shows_other(plan, last + 500) && blink_shows_other(plan, last + 760));
    CHECK(!blink_shows_other(plan, last - 1));
    // Long after the last change the pair is stale (blinking had stopped).
    CHECK(!plan_blink(t, last + 5000).valid && std::string(plan_blink(t, last + 5000).reason) == "stale-alternation");

    BlinkTracker fresh(Frames::W, Frames::H);
    fresh.observe(f.on, 0);
    CHECK(std::string(plan_blink(fresh, 10).reason) == "no-other-image");
    fresh.observe(f.off, 250);
    fresh.observe(f.on, 500);
    CHECK(fresh.toggles() == 1 && std::string(plan_blink(fresh, 510).reason) == "too-few-toggles");

    BlinkTracker large(Frames::W, Frames::H);
    const auto bigLast = alternating(large, f.on, f.big, 5);
    BlinkLimits limits{};
    limits.maxBox = 4; limits.maxChanged = 8;
    CHECK(std::string(plan_blink(large, bigLast + 10, limits).reason) == "difference-not-small");
    CHECK(plan_blink(t, last + 10, limits).valid);  // the 2-pixel marker still fits

    BlinkTracker fast(Frames::W, Frames::H);
    for (unsigned i = 0; i < 6; ++i) fast.observe(i % 2 ? f.off : f.on, i * 10);
    CHECK(std::string(plan_blink(fast, 55).reason) == "run-out-of-range");
    CHECK(!blink_shows_other(plan_blink(fast, 55), 70));

    // A 10 s pause in the middle of an "off" run: excluded, the run resumes
    // and the next toggle records 250, so the pair stays live and timed.
    BlinkTracker paused(Frames::W, Frames::H);
    const auto pausedLast = alternating(paused, f.on, f.off, 5);  // "off" since pausedLast
    paused.observe(f.off, pausedLast + 100);
    paused.exclude_pause(pausedLast + 100, 10000);
    CHECK(paused.runStartMs() == pausedLast + 10000);
    CHECK(plan_blink(paused, pausedLast + 10100).valid);
    CHECK(paused.observe(f.on, pausedLast + 10250) == E::Toggle);
    CHECK(paused.typicalRunMs(paused.latest() ^ 1) == 250);
    // A run that started after the pause began is not shifted.
    paused.exclude_pause(pausedLast, 5000);
    CHECK(paused.runStartMs() == pausedLast + 10250);

    // Marker moved into a revealed room: the pair is (moved, movedOff), not
    // the old room, once it has alternated again.
    BlinkTracker moving(Frames::W, Frames::H);
    alternating(moving, f.on, f.off, 4);
    moving.observe(f.moved, 2000);
    CHECK(moving.observe(f.movedOff, 2250) == E::NewState);
    for (unsigned i = 0; i < 4; ++i) moving.observe(i % 2 ? f.movedOff : f.moved, 2500 + i * 250);
    const auto movedPlan = plan_blink(moving, 3260);
    CHECK(movedPlan.valid && movedPlan.diff.left == 6 && movedPlan.diff.right == 7);
    CHECK(std::equal(f.movedOff, f.movedOff + Frames::W * Frames::H, moving.image(movedPlan.frozen)));
}

namespace {
struct NativeMarkerCycle {
    static constexpr unsigned W = 32, H = 24;
    std::array<std::array<std::uint32_t, W * H>, 16> images{};
    std::array<unsigned, 16> holds{{2, 2, 5, 2, 3, 2, 3, 2, 3, 5, 2, 3, 5, 2, 2, 2}};
    unsigned count = 13;
    NativeMarkerCycle() {
        for (unsigned state = 0; state < images.size(); ++state) {
            images[state].fill(0x005577);
            for (unsigned y = 4; y < 15; ++y)
                for (unsigned x = 10; x < 22; ++x)
                    images[state][y * W + x] = 0x800000 + state * 0x100 + x + y;
        }
    }
    static std::uint64_t ms(unsigned tick) { return (static_cast<std::uint64_t>(tick) * 1000 + 30) / 60; }
    unsigned period() const {
        unsigned result = 0;
        for (unsigned i = 0; i < count; ++i) result += holds[i];
        return result;
    }
    unsigned at(unsigned tick) const {
        tick %= period();
        for (unsigned state = 0; state < count; ++state) {
            if (tick < holds[state]) return state;
            tick -= holds[state];
        }
        return 0;
    }
    void feed(MarkerCycleTracker& tracker, unsigned first, unsigned last, std::uint64_t epoch = 0) const {
        for (unsigned tick = first; tick <= last; ++tick) tracker.observe(images[at(tick)].data(), ms(tick), epoch);
    }
    unsigned state(const MarkerCycleTracker& tracker, unsigned slot) const {
        const auto* image = tracker.image(slot);
        CHECK(image != nullptr);
        for (unsigned i = 0; i < count; ++i)
            if (std::equal(images[i].begin(), images[i].end(), image)) return i;
        throw std::runtime_error("uncaptured marker image");
    }
};

void marker_cycle_preserves_thirteen_states_and_thirty_nine_native_ticks() {
    NativeMarkerCycle f;
    MarkerCycleTracker t(f.W, f.H);
    f.feed(t, 0, 81);  // two complete observed sequences, paused in state 1
    const auto p = plan_marker_cycle(t, f.ms(81));
    CHECK(p.valid && p.confidence == MarkerCycleConfidence::Complete);
    CHECK(p.count == 13 && p.periodTicks == 39 && t.imageCount() == 13);
    CHECK(p.diff.changed == 132 && p.diff.right - p.diff.left == 12 && p.diff.bottom - p.diff.top == 11);
    CHECK(f.state(t, p.frozenSlot) == 1);
    unsigned sum = 0;
    for (unsigned i = 0; i < p.count; ++i) {
        const auto state = f.state(t, p.slots[i]);
        CHECK(state == (i + 1) % 13 && p.holdTicks[i] == f.holds[state]);
        sum += p.holdTicks[i];
    }
    CHECK(sum == 39);
    CHECK(marker_cycle_frame(p, p.phaseStartMs + 32).slot == p.frozenSlot);
    CHECK(f.state(t, marker_cycle_frame(p, p.phaseStartMs + 34).slot) == 2);
    for (const auto cycles : {1ull, 17ull, 1000000ull}) {
        const auto frame = marker_cycle_frame(p, p.phaseStartMs + cycles * 650);
        CHECK(frame.slot == p.frozenSlot && frame.holdTicks == 2 && frame.sequenceIndex == 0);
    }
    CHECK(marker_cycle_frame(p, p.phaseStartMs - 1).slot == p.frozenSlot);
}

void marker_cycle_repeated_images_keep_order_and_individual_durations() {
    NativeMarkerCycle f;
    const unsigned sequence[] = {0, 1, 0, 2}, holds[] = {2, 3, 4, 5};
    MarkerCycleTracker t(f.W, f.H);
    unsigned tick = 0;
    for (unsigned occurrence = 0; occurrence < 10; ++occurrence) {
        const unsigned phase = occurrence % 4;
        for (unsigned j = 0; j < holds[phase]; ++j, ++tick)
            t.observe(f.images[sequence[phase]].data(), f.ms(tick));
        if (occurrence == 2) {
            // A appeared twice, but that does not close A-B-A-C.
            CHECK(t.plan(f.ms(tick - 1)).confidence != MarkerCycleConfidence::Complete);
        }
    }
    const auto p = t.plan(f.ms(tick - 1));
    CHECK(p.valid && p.confidence == MarkerCycleConfidence::Complete && p.count == 4);
    CHECK(p.periodTicks == 14 && t.imageCount() == 3);
    CHECK(f.state(t, p.slots[0]) == 1 && p.holdTicks[0] == 3);
    CHECK(f.state(t, p.slots[1]) == 0 && p.holdTicks[1] == 4);
    CHECK(f.state(t, p.slots[2]) == 2 && p.holdTicks[2] == 5);
    CHECK(f.state(t, p.slots[3]) == 0 && p.holdTicks[3] == 2);
    CHECK(p.slots[1] == p.slots[3]);
}

void marker_cycle_partial_contains_only_completed_observed_runs() {
    NativeMarkerCycle f;
    MarkerCycleTracker t(f.W, f.H);
    f.feed(t, 0, 3);
    CHECK(!t.plan(f.ms(3)).valid);
    f.feed(t, 4, 12);
    const auto p = t.plan(f.ms(12));
    CHECK(p.valid && p.confidence == MarkerCycleConfidence::Partial && p.count == 3);
    CHECK(f.state(t, p.frozenSlot) == 4);
    CHECK(p.phaseStartMs == f.ms(12));
    for (unsigned i = 0; i < p.count; ++i) {
        CHECK(f.state(t, p.slots[i]) == i + 1 && p.holdTicks[i] == f.holds[i + 1]);
        CHECK(p.slots[i] != p.frozenSlot);  // unfinished run is never assigned a fabricated hold
    }
    // There is no native-cycle claim for the truncated sequence.
    CHECK(p.periodTicks == 9 && p.periodTicks != 39);
    CHECK(t.image(16) == nullptr);
}

void marker_cycle_invalidates_movement_identity_background_and_stopped_source() {
    NativeMarkerCycle f;
    using E = MarkerCycleTracker::Event;
    for (unsigned mode = 0; mode < 4; ++mode) {
        MarkerCycleTracker t(f.W, f.H);
        f.feed(t, 0, 81);
        CHECK(t.plan(f.ms(81)).confidence == MarkerCycleConfidence::Complete);
        const auto generation = t.generation();
        auto image = f.images[1];
        if (mode == 0) {  // 1 px positional change is novel even inside the old union box
            for (unsigned y = 4; y < 15; ++y) {
                image[y * f.W + 10] = 0x005577;
                image[y * f.W + 22] = image[y * f.W + 21];
            }
        } else if (mode == 1) image.fill(0x112233);
        if (mode < 3) {
            const auto event = t.observe(image.data(), f.ms(82), mode == 2 ? 7 : 0);
            CHECK(event == E::Reset && t.generation() != generation && t.imageCount() == 1);
        } else {
            // The same image continuing past the expected run loses confidence.
            for (unsigned tick = 82; tick <= 85; ++tick) t.observe(image.data(), f.ms(tick));
            CHECK(t.generation() != generation);
        }
        CHECK(t.plan(f.ms(85)).confidence == MarkerCycleConfidence::Frozen);
    }
    // Sub-room movement may leave the collapsed marker's image unchanged.
    // The independent position epoch must invalidate before the equality fast
    // path; equality itself supplies no evidence of the player's position.
    f.images[12].fill(0x005577);
    f.images[12][5 * f.W + 11] = 0xff0000;
    MarkerCycleTracker collapsed(f.W, f.H);
    f.feed(collapsed, 0, 115);
    CHECK(collapsed.plan(f.ms(115)).confidence == MarkerCycleConfidence::Complete);
    CHECK(collapsed.observe(f.images[12].data(), f.ms(116), 1) == E::Reset);
    CHECK(!collapsed.plan(f.ms(116)).valid && collapsed.imageCount() == 1);
}

void marker_cycle_sampling_gaps_clock_reversal_reset_and_recover() {
    NativeMarkerCycle f;
    using E = MarkerCycleTracker::Event;
    MarkerCycleTracker t(f.W, f.H);
    f.feed(t, 0, 81);
    const auto generation = t.generation();
    CHECK(t.observe(f.images[f.at(84)].data(), f.ms(84)) == E::Reset);  // two missing captures
    CHECK(t.generation() != generation && t.imageCount() == 1 && !t.plan(f.ms(84)).valid);
    f.feed(t, 85, 166);
    CHECK(t.plan(f.ms(166)).confidence == MarkerCycleConfidence::Complete);
    CHECK(t.observe(f.images[f.at(166)].data(), f.ms(165)) == E::Reset);
    CHECK(!t.plan(f.ms(165)).valid);
    CHECK(t.observe(nullptr, f.ms(167)) == E::Reset && t.imageCount() == 0);
    CHECK(!t.plan(f.ms(167)).valid);
}

void marker_cycle_supports_sixteen_states_and_refuses_seventeenth() {
    NativeMarkerCycle f;
    f.count = 16; f.holds.fill(2);
    MarkerCycleTracker t(f.W, f.H);
    f.feed(t, 0, 67);
    const auto p = t.plan(f.ms(67));
    CHECK(p.valid && p.confidence == MarkerCycleConfidence::Complete && p.count == 16 && p.periodTicks == 32);
    MarkerCycleTracker growing(f.W, f.H);
    unsigned tick = 0;
    for (unsigned state = 0; state < 16; ++state) {
        for (unsigned j = 0; j < 2; ++j, ++tick) growing.observe(f.images[state].data(), f.ms(tick));
    }
    auto seventeenth = f.images[0]; seventeenth[4 * f.W + 10] = 0xaabbcc;
    CHECK(growing.observe(seventeenth.data(), f.ms(tick)) == MarkerCycleTracker::Event::Reset);
    CHECK(growing.imageCount() == 1 && !growing.plan(f.ms(tick)).valid);
}

// Dawn's marker (dra01 0x18004d590): the counter +0x5e wraps at 39, the
// affine scale steps 16..1 with holds 2,2,3,2,3,... (16 states, 39 ticks),
// a 16x16 sprite. Here every scale is distinct and opaque, so its return from
// the smallest to the full size changes 255 pixels: above Portrait's limit,
// within Dawn's. Every other limit is Portrait's.
void dawn_marker_cycle_fits_its_sprite_and_keeps_portrait_limits() {
    constexpr unsigned W = 32, H = 24;
    constexpr std::array<unsigned, 16> holds{{2, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2}};
    std::array<std::array<std::uint32_t, W * H>, 16> images{};
    for (unsigned state = 0; state < images.size(); ++state) {
        images[state].fill(0x102030);
        const unsigned side = 16 - state, origin = 8 + state / 2;
        for (unsigned y = 0; y < side; ++y)
            for (unsigned x = 0; x < side; ++x) images[state][(4 + state / 2 + y) * W + origin + x] = 0xc0c0ff;
    }
    const auto at = [&](unsigned tick) {
        tick %= 39;
        for (unsigned state = 0; state < holds.size(); ++state) {
            if (tick < holds[state]) return state;
            tick -= holds[state];
        }
        return 0u;
    };
    const auto ms = [](unsigned tick) { return (static_cast<std::uint64_t>(tick) * 1000 + 30) / 60; };
    const auto diff = diff_images(images[15].data(), images[0].data(), W, H);
    CHECK(diff.changed == 255 && diff.right - diff.left == 16 && diff.bottom - diff.top == 16);
    // Dawn: the first lap back at the full size closes on the 39-tick period.
    MarkerCycleTracker dawn(W, H, marker_cycle_limits(Title::DawnOfSorrow));
    for (unsigned tick = 0; tick <= 39; ++tick) dawn.observe(images[at(tick)].data(), ms(tick));
    auto plan = dawn.plan(ms(39));
    CHECK(plan.valid && plan.confidence == MarkerCycleConfidence::Complete);
    CHECK(plan.count == 16 && plan.periodTicks == 39 && dawn.imageCount() == 16);
    // Still complete two laps later, frozen at any phase.
    for (unsigned tick = 40; tick <= 120; ++tick) dawn.observe(images[at(tick)].data(), ms(tick));
    plan = dawn.plan(ms(120));
    CHECK(plan.valid && plan.confidence == MarkerCycleConfidence::Complete && plan.periodTicks == 39);
    // Portrait's limits refuse that return and never complete the cycle.
    MarkerCycleTracker portrait(W, H, marker_cycle_limits(Title::PortraitOfRuin));
    for (unsigned tick = 0; tick <= 160; ++tick) portrait.observe(images[at(tick)].data(), ms(tick));
    CHECK(portrait.plan(ms(160)).confidence != MarkerCycleConfidence::Complete);
    // A change wider than the sprite is still no marker, whatever the title.
    auto wide = images[1];
    for (unsigned x = 0; x < W; ++x) wide[22 * W + x] = 0xffffff;
    MarkerCycleTracker refused(W, H, marker_cycle_limits(Title::DawnOfSorrow));
    refused.observe(images[0].data(), ms(0));
    refused.observe(images[0].data(), ms(1));
    CHECK(refused.observe(wide.data(), ms(2)) == MarkerCycleTracker::Event::Reset);
    // The limits themselves: Portrait unchanged, Dawn differs only there.
    const MarkerCycleLimits defaults{};
    const auto same = [](const MarkerCycleLimits& a, const MarkerCycleLimits& b, bool changed) {
        return (a.maxChanged == b.maxChanged) != changed && a.maxTransitionBox == b.maxTransitionBox &&
               a.maxUnionBox == b.maxUnionBox && a.maxSampleGapMs == b.maxSampleGapMs &&
               a.minHoldTicks == b.minHoldTicks && a.maxHoldTicks == b.maxHoldTicks &&
               a.nativePeriodTicks == b.nativePeriodTicks && a.minSingleLapStates == b.minSingleLapStates &&
               a.maxSingleLapHoldTicks == b.maxSingleLapHoldTicks &&
               a.minSingleLapChanged == b.minSingleLapChanged && a.minSingleLapUnionBox == b.minSingleLapUnionBox;
    };
    CHECK(same(marker_cycle_limits(Title::PortraitOfRuin), defaults, false));
    CHECK(same(marker_cycle_limits(Title::DawnOfSorrow), defaults, true));
    CHECK(marker_cycle_limits(Title::DawnOfSorrow).maxChanged == 256 && defaults.maxChanged == 192);
    CHECK(defaults.nativePeriodTicks == 39 && defaults.maxTransitionBox == 16);
    // Switching a tracker to another title's limits starts over.
    const auto generation = dawn.generation();
    dawn.set_limits(marker_cycle_limits(Title::PortraitOfRuin));
    CHECK(dawn.imageCount() == 0 && dawn.generation() != generation && !dawn.plan(ms(121)).valid);
}

void marker_cycle_excludes_pause_without_changing_measured_holds() {
    NativeMarkerCycle f;
    MarkerCycleTracker t(f.W, f.H);
    f.feed(t, 0, 81);
    const auto before = t.plan(f.ms(81));
    CHECK(before.confidence == MarkerCycleConfidence::Complete);
    t.exclude_pause(f.ms(81), 10000);
    const auto after = t.plan(f.ms(81) + 10000);
    CHECK(after.valid && after.phaseStartMs == before.phaseStartMs + 10000);
    CHECK(after.holdTicks == before.holdTicks && after.periodTicks == before.periodTicks);
    CHECK(t.observe(f.images[f.at(82)].data(), f.ms(82) + 10000) == MarkerCycleTracker::Event::Revisited);
    CHECK(t.plan(f.ms(82) + 10000).confidence == MarkerCycleConfidence::Complete);
}

void marker_cycle_retains_across_ten_static_reopenings_and_repeated_images() {
    NativeMarkerCycle f;
    MarkerCycleTracker t(f.W, f.H);
    f.feed(t, 0, 81, 42);
    const auto initialGen = t.generation();
    const unsigned gapTicks[10] = {5, 7, 4, 9, 6, 8, 3, 11, 5, 14};
    const unsigned pauseMs[10] = {820, 410, 1250, 600, 950, 330, 1500, 480, 710, 1100};
    const unsigned boundarySkewTicks[10] = {1, 0, 2, 1, 0, 1, 2, 0, 1, 0};
    unsigned gameTick = 81;
    std::uint64_t wallOffsetMs = 0;

    for (unsigned openIdx = 0; openIdx < 10; ++openIdx) {
        const auto openAtMs = f.ms(gameTick) + wallOffsetMs;
        const auto plan = t.plan(openAtMs);
        CHECK(plan.valid && plan.confidence == MarkerCycleConfidence::Complete);
        CHECK(plan.count == 13 && plan.periodTicks == 39 && t.generation() == initialGen);
        CHECK(f.state(t, plan.frozenSlot) == f.at(gameTick));

        t.exclude_pause(openAtMs, pauseMs[openIdx]);
        wallOffsetMs += pauseMs[openIdx];

        // Simulate async pause/release boundary skew (+0..+2 ticks) on the
        // single interrupted run without altering subsequent unpaused runs.
        const unsigned interruptedState = f.at(gameTick);
        for (unsigned s = 0; s < boundarySkewTicks[openIdx]; ++s) {
            wallOffsetMs += 16;
            CHECK(t.observe(f.images[interruptedState].data(), f.ms(gameTick) + wallOffsetMs, 42) ==
                  MarkerCycleTracker::Event::Same);
        }
        // Advance fewer than 15 ticks (< 250 ms, well under the 1.3 s 2-lap
        // requirement) before reopening the map at the same position.
        for (unsigned step = 0; step < gapTicks[openIdx]; ++step) {
            ++gameTick;
            const auto ev = t.observe(f.images[f.at(gameTick)].data(), f.ms(gameTick) + wallOffsetMs, 42);
            CHECK(ev == MarkerCycleTracker::Event::Same || ev == MarkerCycleTracker::Event::Revisited);
            CHECK(t.generation() == initialGen);
        }
    }
    const auto finalPlan = t.plan(f.ms(gameTick) + wallOffsetMs);
    CHECK(finalPlan.valid && finalPlan.confidence == MarkerCycleConfidence::Complete);
    unsigned sum = 0;
    for (unsigned i = 0; i < finalPlan.count; ++i) {
        const auto st = f.state(t, finalPlan.slots[i]);
        CHECK(finalPlan.holdTicks[i] == f.holds[st]);
        sum += finalPlan.holdTicks[i];
    }
    CHECK(sum == 39);

    // Once the single pause-interrupted run has transitioned, the very next
    // run requires exact tick equality again (+1 tick must reset).
    while (f.at(gameTick + 1) == f.at(gameTick)) {
        ++gameTick;
        CHECK(t.observe(f.images[f.at(gameTick)].data(), f.ms(gameTick) + wallOffsetMs, 42) ==
              MarkerCycleTracker::Event::Same);
    }
    ++gameTick; // transition out of any interrupted state
    CHECK(t.observe(f.images[f.at(gameTick)].data(), f.ms(gameTick) + wallOffsetMs, 42) ==
          MarkerCycleTracker::Event::Revisited);
    const unsigned cleanState = f.at(gameTick);
    while (f.at(gameTick + 1) == cleanState) {
        ++gameTick;
        CHECK(t.observe(f.images[cleanState].data(), f.ms(gameTick) + wallOffsetMs, 42) ==
              MarkerCycleTracker::Event::Same);
    }
    // Hold cleanState for 1 extra tick before transitioning: must reset!
    wallOffsetMs += 17;
    CHECK(t.observe(f.images[cleanState].data(), f.ms(gameTick) + wallOffsetMs, 42) ==
          MarkerCycleTracker::Event::Same);
    ++gameTick;
    CHECK(t.observe(f.images[f.at(gameTick)].data(), f.ms(gameTick) + wallOffsetMs, 42) ==
          MarkerCycleTracker::Event::Reset);
    CHECK(t.generation() != initialGen);

    // Repeated images (A-B-A-C) across exclude_pause must preserve exact
    // occurrence order and never remap by image equality alone.
    const unsigned seq[] = {0, 1, 0, 2}, holds[] = {2, 3, 4, 5};
    for (int variant = 0; variant < 4; ++variant) {
        MarkerCycleTracker abac(f.W, f.H);
        unsigned tick = 0;
        // occ 1..8 complete two full 4-state laps once occ 8 transitions to occ 9;
        // occ 12 (12 % 4 == 0) is the first A (expectedNext = B, slot 1),
        // occ 10 (10 % 4 == 2) is the second A (expectedNext = C, slot 2).
        const unsigned stopOccurrence = (variant < 2) ? 12u : 10u;
        for (unsigned occ = 0; occ <= stopOccurrence; ++occ) {
            const unsigned phase = occ % 4;
            const unsigned countTicks = (occ == stopOccurrence) ? 1u : holds[phase];
            for (unsigned j = 0; j < countTicks; ++j, ++tick)
                abac.observe(f.images[seq[phase]].data(), f.ms(tick), 9);
        }
        const auto gen = abac.generation();
        const auto pauseAt = f.ms(tick - 1);
        CHECK(abac.plan(pauseAt).confidence == MarkerCycleConfidence::Complete);
        abac.exclude_pause(pauseAt, 500);
        // Transition immediately on the first post-pause tick (pre-pause hold was only 1 tick).
        const unsigned rightNext = (variant < 2) ? 1u : 2u;
        const unsigned wrongNext = (variant < 2) ? 2u : 1u;
        const unsigned chosen = (variant % 2 == 0) ? rightNext : wrongNext;
        const auto ev = abac.observe(f.images[chosen].data(), f.ms(tick) + 500, 9);
        if (variant % 2 == 0) {
            CHECK(ev == MarkerCycleTracker::Event::Revisited && abac.generation() == gen);
            CHECK(abac.plan(f.ms(tick) + 500).confidence == MarkerCycleConfidence::Complete);
        } else {
            CHECK(ev == MarkerCycleTracker::Event::Reset && abac.generation() != gen);
            CHECK(!abac.plan(f.ms(tick) + 500).valid);
        }
    }
}

void marker_cycle_frame_boundaries_are_rational_and_pause_requires_fresh_capture() {
    NativeMarkerCycle f;
    MarkerCycleTracker t(f.W, f.H);
    f.feed(t, 0, 85);
    const auto p = t.plan(f.ms(85));
    CHECK(p.valid && p.confidence == MarkerCycleConfidence::Complete);
    unsigned cumulative = 0;
    for (unsigned i = 0; i < p.count; ++i) {
        const auto boundaryMs = (static_cast<std::uint64_t>(cumulative) * 1000 + 59) / 60;
        for (unsigned cycle = 0; cycle < 10; ++cycle) {
            const auto now = p.phaseStartMs + boundaryMs + cycle * 650;
            CHECK(marker_cycle_frame(p, now).sequenceIndex == i);
            if (i || cycle) CHECK(marker_cycle_frame(p, now - 1).sequenceIndex == (i + p.count - 1) % p.count);
        }
        cumulative += p.holdTicks[i];
    }
    CHECK(cumulative == 39 && marker_cycle_frame(p, p.phaseStartMs + 650).sequenceIndex == 0);
    // A five-frame hold cannot excuse missing the last pre-pause captures.
    CHECK(t.plan(f.ms(85) + 25).valid);
    const auto missed = t.plan(f.ms(85) + 26);
    CHECK(!missed.valid && std::string(missed.reason) == "capture-gap-before-pause");
    CHECK(!t.plan(f.ms(84)).valid);  // a plan cannot rewind observations
}

void marker_cycle_long_holds_obey_limits_and_stale_source_loses_confidence() {
    NativeMarkerCycle f;
    f.count = 2; f.holds.fill(120);
    MarkerCycleTracker t(f.W, f.H);
    f.feed(t, 0, 601);
    const auto p = t.plan(f.ms(601));
    CHECK(p.valid && p.confidence == MarkerCycleConfidence::Complete && p.periodTicks == 240);
    CHECK(p.holdTicks[0] == 120 && p.holdTicks[1] == 120);
    CHECK(marker_cycle_frame(p, p.phaseStartMs + 1999).sequenceIndex == 0);
    CHECK(marker_cycle_frame(p, p.phaseStartMs + 2000).sequenceIndex == 1);
    CHECK(marker_cycle_frame(p, p.phaseStartMs + 4000).sequenceIndex == 0);
    const auto generation = t.generation();
    // Dense samples make this a stopped animation, not a sampling-gap reset.
    for (unsigned tick = 602; tick <= 721; ++tick) {
        CHECK(t.observe(f.images[1].data(), f.ms(tick)) == MarkerCycleTracker::Event::Same);
    }
    CHECK(t.generation() == generation && t.plan(f.ms(721)).valid);
    CHECK(t.observe(f.images[1].data(), f.ms(722)) == MarkerCycleTracker::Event::Reset);
    CHECK(t.generation() != generation && !t.plan(f.ms(722)).valid);

    MarkerCycleLimits limits{}; limits.maxHoldTicks = 2;
    MarkerCycleTracker limited(f.W, f.H, limits);
    limited.observe(f.images[0].data(), 0);
    limited.observe(f.images[1].data(), f.ms(1));
    for (unsigned tick = 2; tick <= 4; ++tick) limited.observe(f.images[1].data(), f.ms(tick));
    CHECK(limited.observe(f.images[0].data(), f.ms(5)) == MarkerCycleTracker::Event::Reset);
    CHECK(!limited.plan(f.ms(5)).valid);
}

void marker_cycle_single_lap_thirty_nine_tick_revisit_closure() {
    NativeMarkerCycle f;
    MarkerCycleTracker t(f.W, f.H);
    // Before revisiting state 0 (ticks 0..38, 13 states captured, 11 completed runs),
    // the sequence is still Partial and must not claim a closed cycle.
    f.feed(t, 0, 38);
    const auto beforeRevisit = t.plan(f.ms(38));
    CHECK(beforeRevisit.valid && beforeRevisit.confidence == MarkerCycleConfidence::Partial);
    CHECK(t.imageCount() == 13 && t.completedRuns() == 11);

    // At tick 39 (~650 ms), state 0 is revisited (`runs == 12 == images - 1`).
    // Single-lap closure immediately recognizes the complete 13-state, 39-tick cycle.
    f.feed(t, 39, 39);
    const auto atRevisit = t.plan(f.ms(39));
    const auto gen = t.generation();
    CHECK(atRevisit.valid && atRevisit.confidence == MarkerCycleConfidence::Complete);
    CHECK(atRevisit.count == 13 && atRevisit.periodTicks == 39);
    CHECK(f.state(t, atRevisit.frozenSlot) == 0 && atRevisit.holdTicks[0] == f.holds[0]);

    // Continuing through Lap 2 (ticks 40..81) keeps the exact same generation and timings.
    f.feed(t, 40, 81);
    const auto afterLap2 = t.plan(f.ms(81));
    CHECK(afterLap2.valid && afterLap2.confidence == MarkerCycleConfidence::Complete);
    CHECK(t.generation() == gen && afterLap2.count == 13 && afterLap2.periodTicks == 39);

    // Transient slot 0 at movement stop (splits a 3-tick state into 1 tick slot 0 + 2 ticks slot 1),
    // followed by a 13-state cycle in slots 1..13 closing on slot 1 -> slot 2 with sum == 39 ticks
    // (mirrors runtime-19632.log gen=29).
    NativeMarkerCycle g;
    g.holds[0] = 3; g.holds[1] = 2; g.holds[2] = 4; // still 39 ticks total across states 0..12
    MarkerCycleTracker transientZero(g.W, g.H);
    auto transientImg = g.images[0];
    transientImg[4 * g.W + 10] ^= 0x010101;
    transientZero.observe(transientImg.data(), g.ms(0)); // 1 tick of transient slot 0
    g.feed(transientZero, 1, 39); // ticks 1..2: 2 ticks of slot 1 (state 0); at tick 39 revisits slot 1 (sum=38 != 39)
    CHECK(transientZero.plan(g.ms(39)).confidence == MarkerCycleConfidence::Partial);
    g.feed(transientZero, 40, 42); // ticks 39..41: 3 ticks of slot 1; at tick 42 transitions slot 1 -> slot 2 (sum=39)
    const auto closedOnTwo = transientZero.plan(g.ms(42));
    CHECK(closedOnTwo.valid && closedOnTwo.confidence == MarkerCycleConfidence::Complete);
    CHECK(closedOnTwo.count == 13 && closedOnTwo.periodTicks == 39 && transientZero.imageCount() == 14);
}

// Synthetic EmuTask/loader states only; no game memory or module is read.
constexpr std::uintptr_t FakeTask = 0x10000, FakeVtable = 0x140512850;
constexpr std::uintptr_t Dra01 = 0x7ff800000000, Dra02 = 0x7ff810000000, Dra02Elsewhere = 0x7ff820000000;

struct FakeTitlePlatform final : ITitlePlatform {
    bool taskAvailable = true, fieldsReadable = true, fileGood = true;
    std::uintptr_t task = FakeTask;
    EmuTaskTitleFields fields{FakeVtable, Dra02, PortraitModuleIndex, 1};
    std::uintptr_t loaded = Dra02, loadedDawn = 0;
    TitleModuleImage image{Dra02, 0x9c4000, 0x5f3a0000, true};
    TitleModuleImage dawnImage{Dra01, 0xd48000, 0x68e4b063, true};
    bool dawnFileGood = true;
    std::vector<Title> verified;
    TitleFileKey key{0x9a7c465a, 0x60000000069e4f, 5056512, 0x01d9000000000000, true};
    // Changes applied right after the N-th field read / module query (0: never).
    unsigned changeFieldsAfterRead = 0, unloadAfterQuery = 0;
    EmuTaskTitleFields changedFields{};
    unsigned fieldReads = 0, moduleQueries = 0, imageQueries = 0, keyQueries = 0, verifyCalls = 0;

    bool read_task(std::uintptr_t& out) noexcept override {
        if (!taskAvailable) return false;
        out = task;
        return true;
    }
    bool read_fields(std::uintptr_t requested, EmuTaskTitleFields& out) noexcept override {
        if (!fieldsReadable || requested != task) return false;
        out = fields;
        if (++fieldReads == changeFieldsAfterRead) fields = changedFields;
        return true;
    }
    std::uintptr_t title_module(Title title) noexcept override {
        const auto result = title == Title::PortraitOfRuin ? loaded : title == Title::DawnOfSorrow ? loadedDawn : 0;
        if (++moduleQueries == unloadAfterQuery) loaded = loadedDawn = 0;
        return result;
    }
    TitleModuleImage module_image(Title title, std::uintptr_t handle) noexcept override {
        ++imageQueries;
        const auto& base = title == Title::DawnOfSorrow ? dawnImage : image;
        auto result = base;
        result.handle = handle;
        result.valid = base.valid && handle == base.handle && title != Title::Unknown;
        return result;
    }
    TitleFileKey file_key(Title, const TitleModuleImage&) noexcept override { ++keyQueries; return key; }
    bool verify_file(Title title, const TitleModuleImage& checked, const TitleFileKey& checkedKey) noexcept override {
        ++verifyCalls;
        try { verified.push_back(title); } catch (...) {}
        const bool good = title == Title::DawnOfSorrow ? dawnFileGood : fileGood;
        return good && checked.valid && checkedKey.valid;
    }
    // EmuTask::loadEmuModuleNum's end state for one index.
    void load(std::int32_t index, std::uintptr_t module) {
        fields.index = index;
        fields.module = module;
        fields.ready = 1;
        loaded = module == Dra02 || module == Dra02Elsewhere ? module : 0;
        loadedDawn = module == Dra01 ? module : 0;
        image.handle = loaded ? loaded : image.handle;
        dawnImage.handle = loadedDawn ? loadedDawn : dawnImage.handle;
    }
};

struct TitleFixture {
    FakeTitlePlatform platform;
    TitleDetector detector;
    std::uint64_t epoch = 5, now = 1000;
    TitleObservation update() { return detector.update(platform, FakeTask, epoch, now); }
};

void title_detector_never_verifies_boot_menus_or_other_titles() {
    TitleFixture f;
    f.platform.taskAvailable = false;
    CHECK(!f.update().portrait && f.update().reason == TitleReason::NoTask);
    f.platform.taskAvailable = true;
    CHECK(f.detector.update(f.platform, 0, f.epoch, f.now).reason == TitleReason::TaskMismatch);
    CHECK(f.detector.update(f.platform, FakeTask + 8, f.epoch, f.now).reason == TitleReason::TaskMismatch);
    f.platform.fieldsReadable = false;
    CHECK(f.update().reason == TitleReason::ReadFailed);
    f.platform.fieldsReadable = true;
    // Constructor state: no module and index -1, +0x184 clear.
    f.platform.fields = {FakeVtable, 0, -1, 0};
    f.platform.loaded = 0;
    CHECK(f.update().reason == TitleReason::NoModule);
    // Boot reinitialization clamps -1 to 0 and loads dra01 behind the menu.
    f.platform.load(0, Dra01);
    auto observed = f.update();
    CHECK(!observed.portrait && observed.reason == TitleReason::OtherTitle && observed.indexed == Title::DawnOfSorrow);
    f.platform.load(2, Dra01 + 0x1000000);
    observed = f.update();
    CHECK(!observed.portrait && observed.reason == TitleReason::OtherTitle && observed.indexed == Title::OrderOfEcclesia);
    f.platform.load(3, Dra01 + 0x2000000);
    observed = f.update();
    CHECK(!observed.portrait && observed.reason == TitleReason::OtherTitle && observed.indexed == Title::Unknown);
    f.platform.fields = {FakeVtable, Dra01, StaticModuleIndex, 1};
    CHECK(f.update().reason == TitleReason::StaticModule);
    f.platform.fields.index = 9;
    CHECK(f.update().reason == TitleReason::OtherTitle);
    // Even with dra02.dll still loaded, another index is never Portrait.
    f.platform.fields = {FakeVtable, Dra02, 0, 1};
    f.platform.loaded = Dra02;
    CHECK(!f.update().portrait);
    CHECK(f.platform.verifyCalls == 0);
}

void title_detector_requires_the_loaded_verified_dra02() {
    TitleFixture f;
    f.platform.fields.ready = 0;
    CHECK(f.update().reason == TitleReason::Loading);
    // setEmuModuleNum(1) before the reload: index 1, previous module, ready.
    f.platform.fields = {FakeVtable, Dra01, PortraitModuleIndex, 1};
    f.platform.loaded = 0;
    CHECK(f.update().reason == TitleReason::ModuleMismatch);
    f.platform.loaded = Dra02;
    CHECK(f.update().reason == TitleReason::ModuleMismatch);
    f.platform.fields.module = Dra02Elsewhere;
    CHECK(f.update().reason == TitleReason::ModuleMismatch);
    f.platform.fields.module = Dra02;
    f.platform.image.valid = false;  // wrong path, not MEM_IMAGE or not a DLL
    CHECK(f.update().reason == TitleReason::ModuleImage);
    f.platform.image.valid = true;
    f.platform.fileGood = false;
    CHECK(f.update().reason == TitleReason::ModuleFile);
    TitleFixture good;
    const auto observed = good.update();
    CHECK(observed.portrait && observed.reason == TitleReason::Verified);
    CHECK(observed.indexed == Title::PortraitOfRuin && observed.fields.module == Dra02 && observed.taskEpoch == 5);
}

void title_detector_caches_file_checks_per_image_and_epoch() {
    TitleFixture f;
    CHECK(f.update().portrait);
    CHECK(f.platform.verifyCalls == 1 && f.platform.keyQueries == 1 && f.platform.imageQueries == 1);
    for (int i = 0; i < 300; ++i) CHECK(f.update().portrait);
    // Unchanged frames: two field reads and two module lookups, no file I/O.
    CHECK(f.platform.verifyCalls == 1 && f.platform.keyQueries == 1 && f.platform.imageQueries == 1);
    CHECK(f.platform.fieldReads == 2 * 301 && f.platform.moduleQueries == 2 * 301);
    // A new task epoch re-checks the image and the cheap file identity only.
    ++f.epoch;
    CHECK(f.update().portrait);
    CHECK(f.platform.verifyCalls == 1 && f.platform.keyQueries == 2 && f.platform.imageQueries == 2);
    // The file changed on disk: hashed again and refused.
    ++f.epoch;
    f.platform.key.written += 1;
    f.platform.fileGood = false;
    CHECK(f.update().reason == TitleReason::ModuleFile && f.platform.verifyCalls == 2);
    // Failed checks retry at most MaxFileAttempts times per epoch, spaced out.
    for (int i = 0; i < 10; ++i) CHECK(!f.update().portrait);
    CHECK(f.platform.verifyCalls == 2);
    f.now += TitleDetector::FileRetryMs;
    CHECK(!f.update().portrait && f.platform.verifyCalls == 3);
    f.now += TitleDetector::FileRetryMs;
    CHECK(!f.update().portrait && f.platform.verifyCalls == 4);
    f.now += 10 * TitleDetector::FileRetryMs;
    CHECK(!f.update().portrait && f.platform.verifyCalls == 4);
    // A transient failure recovers on a later attempt.
    ++f.epoch;
    f.platform.fileGood = true;
    CHECK(f.update().portrait && f.platform.verifyCalls == 5);
    // A reload at another base is a new image: hashed once more.
    f.platform.load(PortraitModuleIndex, Dra02Elsewhere);
    CHECK(f.update().portrait && f.platform.verifyCalls == 6);
    CHECK(f.detector.verifications() == 6);
}

void title_detector_rejects_races_and_unloads() {
    TitleFixture f;
    CHECK(f.update().portrait);
    // +0x184 cleared by a load starting between the two reads.
    f.platform.changedFields = f.platform.fields;
    f.platform.changedFields.ready = 0;
    f.platform.changeFieldsAfterRead = f.platform.fieldReads + 1;
    CHECK(f.update().reason == TitleReason::Inconsistent);
    CHECK(f.update().reason == TitleReason::Loading);
    f.platform.fields.ready = 1;
    CHECK(f.update().portrait);
    // +0x528 and vtable changes between the reads.
    f.platform.changedFields = f.platform.fields;
    f.platform.changedFields.module = Dra01;
    f.platform.changeFieldsAfterRead = f.platform.fieldReads + 1;
    CHECK(f.update().reason == TitleReason::Inconsistent);
    f.platform.fields.module = Dra02;
    f.platform.changedFields = f.platform.fields;
    f.platform.changedFields.vtable = FakeVtable + 0x10;
    f.platform.changeFieldsAfterRead = f.platform.fieldReads + 1;
    CHECK(f.update().reason == TitleReason::Inconsistent);
    f.platform.fields.vtable = FakeVtable;
    CHECK(f.update().portrait);
    // dra02.dll unloaded between the module checks, then gone.
    f.platform.unloadAfterQuery = f.platform.moduleQueries + 1;
    CHECK(f.update().reason == TitleReason::Inconsistent);
    CHECK(f.update().reason == TitleReason::ModuleMismatch);
    // The fast path never skips the module identity of a later frame.
    f.platform.loaded = Dra02;
    CHECK(f.update().portrait);
    f.platform.loaded = 0;
    CHECK(f.update().reason == TitleReason::ModuleMismatch);
}

void title_detector_follows_portrait_dawn_portrait_transitions() {
    TitleFixture f;
    std::vector<std::uint64_t> generations;
    const auto expect = [&](bool portrait, TitleReason reason) {
        const auto observed = f.update();
        CHECK(observed.portrait == portrait && observed.reason == reason);
        CHECK(f.update().generation == observed.generation);  // stable while unchanged
        CHECK(generations.empty() || observed.generation > generations.back());
        generations.push_back(observed.generation);
    };
    expect(true, TitleReason::Verified);
    f.platform.fields.index = 0;                     // menu: setEmuModuleNum(0)
    expect(false, TitleReason::OtherTitle);
    ++f.epoch;                                       // reinitialize
    f.platform.fields.ready = 0;                     // loadEmuModuleNum starts
    expect(false, TitleReason::OtherTitle);
    f.platform.load(0, Dra01);                       // Dawn of Sorrow running
    expect(false, TitleReason::OtherTitle);
    f.platform.fields.index = PortraitModuleIndex;   // menu: setEmuModuleNum(1)
    expect(false, TitleReason::ModuleMismatch);
    ++f.epoch;
    f.platform.fields.ready = 0;
    expect(false, TitleReason::Loading);
    f.platform.load(PortraitModuleIndex, Dra02);     // Portrait again, same image
    expect(true, TitleReason::Verified);
    ++f.epoch;                                       // task reinitialized in place
    expect(true, TitleReason::Verified);
    CHECK(f.platform.verifyCalls == 1);
}

void portrait_gate_confirms_identity_and_revokes_at_once() {
    PortraitGate gate;
    TitleObservation portrait{};
    portrait.portrait = true;
    portrait.title = Title::PortraitOfRuin;
    portrait.reason = TitleReason::Verified;
    portrait.fields = {FakeVtable, Dra02, PortraitModuleIndex, 1};
    portrait.taskEpoch = 7;
    PortraitGateInput input{portrait, true, false, false};
    TitleObservation other = portrait;
    other.portrait = false;
    other.title = Title::Unknown;
    other.reason = TitleReason::OtherTitle;
    auto state = gate.update({other, true, false, false});
    CHECK(!state.identity && !state.input && !state.identityChanged);
    state = gate.update(input);
    CHECK(!state.identity && !state.input);
    state = gate.update(input);
    CHECK(state.identity && state.input && state.identityChanged && state.inputChanged);
    state = gate.update(input);
    CHECK(state.identity && !state.identityChanged && !state.inputChanged);
    // Any unverified observation revokes immediately.
    state = gate.update({other, true, false, false});
    CHECK(!state.identity && !state.input && state.identityChanged && state.inputChanged);
    // A new module or task epoch needs fresh confirmations.
    gate.update(input);
    CHECK(gate.update(input).identity);
    input.title.fields.module = Dra02Elsewhere;
    CHECK(!gate.update(input).identity);
    CHECK(gate.update(input).identity);
    input.title.taskEpoch = 8;
    CHECK(!gate.update(input).identity);
    CHECK(gate.update(input).identity);
    gate.reset();
    CHECK(!gate.identity() && !gate.input());
}

void portrait_gate_keeps_input_out_of_foreign_pauses() {
    PortraitGate gate;
    TitleObservation portrait{};
    portrait.portrait = true;
    portrait.title = Title::PortraitOfRuin;
    portrait.reason = TitleReason::Verified;
    portrait.fields = {FakeVtable, Dra02, PortraitModuleIndex, 1};
    gate.update({portrait, true, false, false});
    CHECK(gate.update({portrait, true, false, false}).input);
    // The collection's menus pause the EmuTask: original input, Portrait kept.
    auto state = gate.update({portrait, true, true, false});
    CHECK(state.identity && !state.input && state.inputChanged && !state.identityChanged);
    // The mod's own map pause, request in flight or stale pause keep it.
    CHECK(gate.update({portrait, true, true, true}).input);
    CHECK(!gate.update({portrait, false, false, true}).input);  // pause unknown
    CHECK(gate.update({portrait, true, false, false}).input);
    TitleObservation lost = portrait;
    lost.portrait = false;
    lost.title = Title::Unknown;
    lost.reason = TitleReason::Loading;
    CHECK(!gate.update({lost, true, false, true}).input);
}

PortraitStateFields scene_fields(std::uint8_t mode, std::uint8_t state = 2, std::uint8_t type = 0,
                                 std::uint8_t page = 0, std::uint8_t requested = 0) {
    PortraitStateFields fields{};
    fields.mode = mode;
    fields.gameState = state;
    fields.gameType = type;
    fields.page = page;
    fields.requestedPage = requested;
    return fields;
}

void portrait_scene_separates_title_menus_and_gameplay() {
    // Unreadable or inconsistent fields are unknown, whatever they hold.
    auto verdict = classify_portrait_scene(scene_fields(2), false);
    CHECK(verdict.scene == GameScene::Unknown && !verdict.gameplay && !verdict.mapReady);
    // Before the Konami screen asks for page 6 the page is still 0: the mode decides.
    verdict = classify_portrait_scene(scene_fields(0x1c, 0, 0, 0, 0), true);
    CHECK(verdict.scene == GameScene::Boot && !verdict.gameplay && !verdict.mapReady);
    verdict = classify_portrait_scene(scene_fields(0x0b, 0, 0, 6, 5), true);
    CHECK(verdict.scene == GameScene::Boot && !verdict.gameplay);
    // Title screen (the reported case: page 6, the graveyard image).
    verdict = classify_portrait_scene(scene_fields(5, 0, 0, 6, 6), true);
    CHECK(verdict.scene == GameScene::Title && !verdict.gameplay && !verdict.mapReady);
    // Even a stale map page outside gameplay is not a map.
    verdict = classify_portrait_scene(scene_fields(5, 2, 0, 0, 0), true);
    CHECK(verdict.scene == GameScene::Title && !verdict.mapReady);
    for (const std::uint8_t mode : {0x12, 0x13, 0x16, 0x1a, 0x1b})
        CHECK(classify_portrait_scene(scene_fields(mode, 2, 0, 4, 4), true).scene == GameScene::StartMenu);
    for (const std::uint8_t mode : {0x00, 0x01, 0x14, 0x15})
        CHECK(classify_portrait_scene(scene_fields(mode), true).scene == GameScene::Loading);
    for (const std::uint8_t mode : {0x07, 0x08, 0x09}) {
        verdict = classify_portrait_scene(scene_fields(mode, 2, 0, 0, 0), true);
        CHECK(verdict.scene == GameScene::InGameMenu && !verdict.gameplay && !verdict.mapReady);
    }
    for (const std::uint8_t mode : {0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x17})
        CHECK(classify_portrait_scene(scene_fields(mode, 2, 0, 0, 0), true).scene == GameScene::Sequence);
    for (unsigned mode = 0x1d; mode <= 0xff; ++mode) {
        verdict = classify_portrait_scene(scene_fields(static_cast<std::uint8_t>(mode), 2, 0, 0, 0), true);
        CHECK(verdict.scene == GameScene::Unknown && !verdict.gameplay && !verdict.mapReady);
    }
    // Gameplay with the map or Status page shown: map ready.
    verdict = classify_portrait_scene(scene_fields(2, 2, 0, 0, 0), true);
    CHECK(verdict.scene == GameScene::Gameplay && verdict.gameplay && verdict.mapReady);
    CHECK(classify_portrait_scene(scene_fields(2, 2, 0, 1, 1), true).mapReady);
    // Gameplay without a map yet: fade-in, death, switching or another page.
    for (const std::uint8_t state : {0, 1, 3}) {
        verdict = classify_portrait_scene(scene_fields(2, state, 0, 0, 0), true);
        CHECK(verdict.gameplay && !verdict.mapReady);
    }
    CHECK(!classify_portrait_scene(scene_fields(2, 2, 0, 0, 1), true).mapReady);
    CHECK(!classify_portrait_scene(scene_fields(2, 2, 0, 4, 4), true).mapReady);
    // Extra modes (Sisters, Richter, ...) were never validated with the map.
    for (const std::uint8_t type : {1, 2, 3, 4, 5}) {
        verdict = classify_portrait_scene(scene_fields(2, 2, type, 0, 0), true);
        CHECK(verdict.scene == GameScene::ExtraMode && !verdict.gameplay && !verdict.mapReady);
    }
    CHECK(std::string(game_scene_name(GameScene::Title)) == "title");
}

void map_command_opens_only_ready_maps_and_always_closes() {
    // Opening needs an unpaused, ready map; anything else changes nothing.
    CHECK(map_command({false, false, false, false, true}) == MapCommand::Open);
    CHECK(map_command({false, false, false, false, false}) == MapCommand::Refuse);
    CHECK(map_command({false, false, false, true, true}) == MapCommand::None);
    // Closing and cancelling never depend on the scene.
    CHECK(map_command({true, true, false, false, false}) == MapCommand::CloseVoluntary);
    CHECK(map_command({true, true, false, true, true}) == MapCommand::CloseVoluntary);
    CHECK(map_command({false, true, false, true, false}) == MapCommand::Cancel);
    CHECK(map_command({false, false, true, false, false}) == MapCommand::Cancel);
}

void portrait_gate_separates_identity_gameplay_and_map() {
    PortraitGate gate;
    TitleObservation portrait{};
    portrait.portrait = true;
    portrait.title = Title::PortraitOfRuin;
    portrait.reason = TitleReason::Verified;
    portrait.fields = {FakeVtable, Dra02, PortraitModuleIndex, 1};
    const auto title = classify_portrait_scene(scene_fields(5, 0, 0, 6, 6), true);
    const auto ready = classify_portrait_scene(scene_fields(2, 2, 0, 0, 0), true);
    const auto fading = classify_portrait_scene(scene_fields(2, 1, 0, 0, 0), true);
    gate.update({portrait, true, false, false, ready});
    // Title screen: the mod may be toggled (identity, input) but View is native.
    auto state = gate.update({portrait, true, false, false, title});
    CHECK(state.identity && state.input && !state.gameplay && !state.mapReady);
    state = gate.update({portrait, true, false, false, ready});
    CHECK(state.gameplay && state.mapReady && state.gameplayChanged);
    state = gate.update({portrait, true, false, false, fading});
    CHECK(state.gameplay && !state.mapReady && !state.gameplayChanged);
    // The mod's own pause keeps a frozen ready map ready; a foreign pause does not.
    CHECK(gate.update({portrait, true, true, true, ready}).mapReady);
    CHECK(!gate.update({portrait, true, true, false, ready}).mapReady);
    // No identity: nothing, whatever the scene says.
    TitleObservation other = portrait;
    other.portrait = false;
    other.title = Title::Unknown;
    other.reason = TitleReason::OtherTitle;
    state = gate.update({other, true, false, false, ready});
    CHECK(!state.identity && !state.gameplay && !state.mapReady && state.gameplayChanged);
}

// Mirrors live_controller's View handling: mapAllowed = gameplay (plus the
// usual mod/focus/pause terms), and a ToggleMap is executed by map_command.
void view_follows_scene_without_duplicate_or_stray_commands() {
    PortraitGate gate;
    InputRecognizer recognizer;
    TitleObservation portrait{};
    portrait.portrait = true;
    portrait.title = Title::PortraitOfRuin;
    portrait.reason = TitleReason::Verified;
    portrait.fields = {FakeVtable, Dra02, PortraitModuleIndex, 1};
    InputSnapshot sample{};
    PortraitGateState state{};
    const auto step = [&](std::uint16_t buttons, const SceneVerdict& scene) {
        state = gate.update({portrait, true, false, false, scene});
        if (state.inputChanged) recognizer.reset();
        if (!state.input) return InputResult{buttons, 0, InputAction::None, false, false};
        sample.buttons = buttons;
        const InputPolicy policy{true, true, state.gameplay, 1};
        return recognizer.update(sample, policy);
    };
    const auto title = classify_portrait_scene(scene_fields(5, 0, 0, 6, 6), true);
    const auto ready = classify_portrait_scene(scene_fields(2, 2, 0, 0, 0), true);
    const auto switching = classify_portrait_scene(scene_fields(2, 2, 0, 0, 1), true);
    const auto menu = classify_portrait_scene(scene_fields(7, 2, 0, 0, 0), true);
    step(0, title);
    step(0, title);
    // Title: View is native Select (replayed on release), no map command.
    CHECK(step(ButtonView, title).forwardedButtons == 0);
    auto result = step(0, title);
    CHECK(result.action == InputAction::None && result.delayedViewPulse && result.forwardedButtons == ButtonView);
    // View+Y still toggles the mod on the title screen.
    step(ButtonView, title);
    CHECK(step(ButtonView | ButtonY, title).action == InputAction::ToggleMod);
    step(0, title);
    // Gameplay: View is the map command, decided at release by map_command.
    CHECK(step(ButtonView, ready).forwardedButtons == 0);
    result = step(0, ready);
    CHECK(result.action == InputAction::ToggleMap && !result.delayedViewPulse);
    CHECK(map_command({false, false, false, false, state.mapReady}) == MapCommand::Open);
    // A sub-screen switch in progress: consumed, refused, no Select page flip.
    CHECK(step(ButtonView, switching).forwardedButtons == 0);
    result = step(0, switching);
    CHECK(result.action == InputAction::ToggleMap && !result.delayedViewPulse && result.forwardedButtons == 0);
    CHECK(map_command({false, false, false, false, state.mapReady}) == MapCommand::Refuse);
    // View held from gameplay into the pause menu: swallowed and drained, no
    // map, no Select, no second command on release.
    CHECK(step(ButtonView, ready).forwardedButtons == 0);
    result = step(ButtonView, menu);
    CHECK(result.action == InputAction::None && result.forwardedButtons == 0);
    result = step(0, menu);
    CHECK(result.action == InputAction::None && !result.delayedViewPulse && result.forwardedButtons == 0);
    // Then, in the menu, View is native Select again.
    step(ButtonView, menu);
    result = step(0, menu);
    CHECK(result.action == InputAction::None && result.delayedViewPulse);
    // And held from the title into gameplay: drained, never a map command.
    step(0, title);
    step(ButtonView, title);
    result = step(ButtonView, ready);
    CHECK(result.action == InputAction::None && result.forwardedButtons == 0);
    result = step(0, ready);
    CHECK(result.action == InputAction::None && !result.delayedViewPulse);
}

// Mirrors live_controller: a gate change resets the recognizer, the inactive
// filter forwards the raw word, and a View held across reactivation drains.
void gate_changes_never_carry_a_gesture_across_contexts() {
    PortraitGate gate;
    InputRecognizer recognizer;
    TitleObservation portrait{};
    portrait.portrait = true;
    portrait.title = Title::PortraitOfRuin;
    portrait.reason = TitleReason::Verified;
    portrait.fields = {FakeVtable, Dra02, PortraitModuleIndex, 1};
    const InputPolicy policy{true, true, true, 1};
    InputSnapshot sample{};
    const auto filter = [&](std::uint16_t buttons, bool paused) {
        const auto state = gate.update({portrait, true, paused, false});
        if (state.inputChanged) recognizer.reset();
        sample.buttons = buttons;
        if (!state.input) return InputResult{buttons, 0, InputAction::None, false, false};
        return recognizer.update(sample, policy);
    };
    filter(0, false);
    CHECK(filter(0, false).forwardedButtons == 0);
    CHECK(filter(ButtonView, false).forwardedButtons == 0);          // pending in Portrait
    // The collection menu pauses mid-gesture: the raw word passes unchanged.
    CHECK(filter(ButtonView | ButtonY, true).forwardedButtons == (ButtonView | ButtonY));
    CHECK(filter(ButtonView, true).forwardedButtons == ButtonView);
    // Back in gameplay with View still held: drained, no map or mod toggle.
    auto result = filter(ButtonView, false);
    CHECK(result.forwardedButtons == 0 && result.action == InputAction::None);
    result = filter(0, false);
    CHECK(result.action == InputAction::None && result.forwardedButtons == 0);
    CHECK(filter(ButtonView, false).forwardedButtons == 0);
    CHECK(filter(0, false).action == InputAction::ToggleMap);
    // Title lost: every combination is forwarded exactly.
    portrait.portrait = false;
    portrait.title = Title::Unknown;
    portrait.reason = TitleReason::OtherTitle;
    for (std::uint32_t buttons = 0; buttons <= 0xffff; ++buttons) {
        const auto raw = static_cast<std::uint16_t>(buttons);
        result = filter(raw, buttons % 3 == 0);
        CHECK(result.forwardedButtons == raw && result.action == InputAction::None && !result.delayedViewPulse);
    }
}

bool same_rect(const NormalizedRect& a, float x, float y, float width, float height) {
    return a.x == x && a.y == y && a.width == width && a.height == height;
}

void adaptation_profiles_keep_portrait_v011_and_isolate_dawn() {
    const auto* portrait = adaptation_profile(Title::PortraitOfRuin);
    CHECK(portrait && portrait->title == Title::PortraitOfRuin);
    // The exact v0.1.1 preset and behavior.
    CHECK(same_rect(portrait->gameplay, 0.f, 1.f / 18.f, 2.f / 3.f, 8.f / 9.f));
    CHECK(same_rect(portrait->upperPanel, 2.f / 3.f, 1.f / 18.f, 1.f / 3.f, 4.f / 9.f));
    CHECK(same_rect(portrait->lowerPanel, 2.f / 3.f, 0.5f, 1.f / 3.f, 4.f / 9.f));
    CHECK(portrait->mapPanel == MapPanelSource::Pixels && portrait->singleScreenEverywhere);
    CHECK(!portrait->pageNavigation && portrait->markerReplay && !portrait->touchRemap);
    const auto* dawn = adaptation_profile(Title::DawnOfSorrow);
    CHECK(dawn && dawn->title == Title::DawnOfSorrow);
    CHECK(dawn->mapPanel == MapPanelSource::NativePage && !dawn->singleScreenEverywhere);
    CHECK(dawn->pageNavigation && dawn->markerReplay && dawn->touchRemap);
    // No adaptation: the mod stays inert there.
    CHECK(!adaptation_profile(Title::OrderOfEcclesia) && !adaptation_profile(Title::Unknown));
    // Static data never claims runtime validation.
    CHECK(!title_profile(Title::DawnOfSorrow).runtimeValidated);
}

void title_detector_keeps_dawn_inert_unless_enabled() {
    // Default set (v0.1.1): a loaded, perfect dra01 is still OtherTitle.
    TitleFixture off;
    off.platform.load(DawnModuleIndex, Dra01);
    auto observed = off.update();
    CHECK(!observed.portrait && observed.title == Title::Unknown && observed.reason == TitleReason::OtherTitle);
    CHECK(observed.indexed == Title::DawnOfSorrow && off.platform.verifyCalls == 0 && off.platform.imageQueries == 0);
    // Experimental set: the same checks as Portrait, against dra01.
    TitleFixture on;
    on.detector = TitleDetector{TitleSet{true, true}};
    on.platform.load(DawnModuleIndex, Dra01);
    on.platform.fields.ready = 0;
    CHECK(on.update().reason == TitleReason::Loading);
    on.platform.fields.ready = 1;
    // setEmuModuleNum(0) before the reload still names the previous module.
    on.platform.fields.module = Dra02;
    CHECK(on.update().reason == TitleReason::ModuleMismatch);
    on.platform.fields.module = Dra01;
    on.platform.dawnImage.valid = false;
    CHECK(on.update().reason == TitleReason::ModuleImage);
    on.platform.dawnImage.valid = true;
    on.platform.dawnFileGood = false;
    CHECK(on.update().reason == TitleReason::ModuleFile);
    on.platform.dawnFileGood = true;
    ++on.epoch;
    observed = on.update();
    CHECK(observed.title == Title::DawnOfSorrow && !observed.portrait && observed.reason == TitleReason::Verified);
    CHECK(!on.platform.verified.empty() && on.platform.verified.back() == Title::DawnOfSorrow);
    // A race between the reads is Inconsistent, never Dawn.
    on.platform.changedFields = on.platform.fields;
    on.platform.changedFields.ready = 0;
    on.platform.changeFieldsAfterRead = on.platform.fieldReads + 1;
    CHECK(on.update().reason == TitleReason::Inconsistent);
    // Portrait detection is unchanged with Dawn enabled; Ecclesia stays other.
    on.platform.load(2, Dra01 + 0x1000000);
    CHECK(on.update().reason == TitleReason::OtherTitle);
    on.platform.load(PortraitModuleIndex, Dra02);
    observed = on.update();
    CHECK(observed.portrait && observed.title == Title::PortraitOfRuin);
    // reset() keeps the configured set.
    on.detector.reset();
    CHECK(on.detector.enabled().dawn && on.detector.verifications() == 0);
}

void title_detector_hashes_each_title_once_across_switches() {
    TitleFixture f;
    f.detector = TitleDetector{TitleSet{true, true}};
    CHECK(f.update().title == Title::PortraitOfRuin);
    f.platform.fields.index = DawnModuleIndex;         // menu: setEmuModuleNum(0)
    CHECK(f.update().reason == TitleReason::ModuleMismatch);
    ++f.epoch;
    f.platform.load(DawnModuleIndex, Dra01);
    CHECK(f.update().title == Title::DawnOfSorrow);
    for (int i = 0; i < 100; ++i) CHECK(f.update().title == Title::DawnOfSorrow);
    ++f.epoch;
    f.platform.load(PortraitModuleIndex, Dra02);       // back to Portrait, same image
    CHECK(f.update().title == Title::PortraitOfRuin);
    ++f.epoch;
    f.platform.load(DawnModuleIndex, Dra01);           // and to Dawn again
    CHECK(f.update().title == Title::DawnOfSorrow);
    CHECK(f.platform.verifyCalls == 2 && f.detector.verifications() == 2);
    CHECK(f.platform.verified.size() == 2 && f.platform.verified[0] == Title::PortraitOfRuin &&
          f.platform.verified[1] == Title::DawnOfSorrow);
}

void title_gate_needs_fresh_confirmations_for_each_title() {
    TitleGate gate;
    TitleObservation portrait{};
    portrait.title = Title::PortraitOfRuin;
    portrait.portrait = true;
    portrait.reason = TitleReason::Verified;
    portrait.fields = {FakeVtable, Dra02, PortraitModuleIndex, 1};
    TitleObservation dawn = portrait;
    dawn.title = Title::DawnOfSorrow;
    dawn.portrait = false;
    dawn.fields = {FakeVtable, Dra01, DawnModuleIndex, 1};
    gate.update({portrait, true, false, false});
    auto state = gate.update({portrait, true, false, false});
    CHECK(state.identity && state.title == Title::PortraitOfRuin && gate.title() == Title::PortraitOfRuin);
    // Even a verified observation of another title starts from zero.
    state = gate.update({dawn, true, false, false});
    CHECK(!state.identity && state.identityChanged && state.title == Title::Unknown);
    state = gate.update({dawn, true, false, false});
    CHECK(state.identity && state.title == Title::DawnOfSorrow);
    // An observation without a verified title revokes it at once.
    TitleObservation none = dawn;
    none.title = Title::Unknown;
    none.reason = TitleReason::Loading;
    CHECK(!gate.update({none, true, false, false}).identity && gate.title() == Title::Unknown);
}

DawnStateFields dawn_fields(std::uint8_t mode, std::uint8_t state = 2, std::uint8_t type = 0,
                            std::uint8_t page = 0, std::uint8_t requested = 0) {
    DawnStateFields fields{};
    fields.mode = mode;
    fields.gameState = state;
    fields.gameType = type;
    fields.page = page;
    fields.requestedPage = requested;
    fields.subState = 0;
    fields.pageOverlay = 0;
    fields.frozen = 0;
    fields.eventFlags = 0;
    fields.fadeFrames = 0;
    fields.brightness = 0;
    return fields;
}

void dawn_scene_follows_gameplay_and_never_trusts_the_page_alone() {
    // Unreadable fields, or fields never read, are unknown.
    auto verdict = classify_dawn_scene(dawn_fields(2), false);
    CHECK(verdict.scene == GameScene::Unknown && !verdict.gameplay && !verdict.singleScreen && !verdict.mapReady);
    verdict = classify_dawn_scene(DawnStateFields{}, true);
    CHECK(verdict.scene == GameScene::Unknown && !verdict.singleScreen && verdict.mapPanel == -1 && !verdict.holdLayout);
    // Normal-game gameplay with the map or Status page: single screen, the
    // view opens on the panel that holds the map, Status as its second page.
    verdict = classify_dawn_scene(dawn_fields(2, 2, 0, 0, 0), true);
    CHECK(verdict.scene == GameScene::Gameplay && verdict.gameplay && verdict.singleScreen && verdict.mapReady);
    CHECK(verdict.mapPanel == 0 && verdict.viewPages == 2);
    verdict = classify_dawn_scene(dawn_fields(2, 2, 0, 1, 1), true);
    CHECK(verdict.mapReady && verdict.mapPanel == 1 && verdict.viewPages == 2);
    // Outside gameplay View is never the mod's and no map opens; the single
    // screen only where the main screen holds the scene (title screen, title
    // menu and file select with the decorative top page; the pause menu), and
    // start/load modes hold the layout.
    for (unsigned mode = 0; mode <= 0xff; ++mode) {
        if (mode == DawnGameplayMode) continue;
        for (const std::uint8_t page : {0, 1, 3}) {
            verdict = classify_dawn_scene(dawn_fields(static_cast<std::uint8_t>(mode), 2, 0, page, page), true);
            CHECK(!verdict.gameplay && !verdict.mapReady && verdict.mapPanel == -1 && verdict.viewPages == 0);
            const bool menu = (mode == 0x03 || mode == 0x04 || mode == 0x09) && page == 3;
            CHECK(verdict.singleScreen == (menu || mode == 0x0a));
            CHECK(verdict.holdLayout == (mode == 0x00 || mode == 0x01));
        }
    }
    CHECK(classify_dawn_scene(dawn_fields(0x0a), true).scene == GameScene::InGameMenu);
    CHECK(classify_dawn_scene(dawn_fields(0x03), true).scene == GameScene::Title);
    CHECK(classify_dawn_scene(dawn_fields(0x04), true).scene == GameScene::StartMenu);
    CHECK(classify_dawn_scene(dawn_fields(0x09), true).scene == GameScene::StartMenu);
    CHECK(classify_dawn_scene(dawn_fields(0x05), true).scene == GameScene::Sequence);
    CHECK(classify_dawn_scene(dawn_fields(0x16), true).scene == GameScene::Sequence);
    CHECK(classify_dawn_scene(dawn_fields(0x00), true).scene == GameScene::Loading);
    CHECK(classify_dawn_scene(dawn_fields(0x01), true).scene == GameScene::Loading);
    // The title menus with any other top screen, or switching it: original.
    CHECK(!classify_dawn_scene(dawn_fields(0x04, 0, 0, 3, 5), true).singleScreen);
    CHECK(!classify_dawn_scene(dawn_fields(0x09, 0, 0, 6, 6), true).singleScreen);
    // Other game types keep their own screens, their pause menu included.
    for (const std::uint8_t type : {1, 2, 3, 4, 5, 0xff}) {
        verdict = classify_dawn_scene(dawn_fields(2, 2, type, 0, 0), true);
        CHECK(verdict.scene == GameScene::ExtraMode && !verdict.singleScreen && !verdict.mapReady);
        CHECK(!classify_dawn_scene(dawn_fields(0x0a, 2, type, 0, 0), true).singleScreen);
    }
    // Any other top screen in gameplay keeps the single screen, but without a
    // live map View stays native Select and nothing opens.
    for (unsigned page = 2; page <= 0xff; ++page) {
        verdict = classify_dawn_scene(dawn_fields(2, 2, 0, static_cast<std::uint8_t>(page), static_cast<std::uint8_t>(page)), true);
        CHECK(verdict.scene == GameScene::Gameplay && verdict.singleScreen && !verdict.gameplay && !verdict.mapReady);
        CHECK(verdict.mapPanel == -1 && verdict.viewPages == 0);
    }
    auto fields = dawn_fields(2, 2, 0, 1, 1);
    fields.pageOverlay = 1;
    verdict = classify_dawn_scene(fields, true);
    CHECK(verdict.singleScreen && !verdict.gameplay && !verdict.mapReady);
    fields = dawn_fields(2, 2, 0, 0, 0);
    fields.eventFlags = DawnPageLockFlag;
    verdict = classify_dawn_scene(fields, true);
    CHECK(verdict.singleScreen && !verdict.gameplay && !verdict.mapReady);
    // A switch inside the pair keeps View the mod's (consumed, nothing opens);
    // a switch towards another page leaves it native.
    for (const auto& pages : {std::pair<int, int>{0, 1}, {1, 0}}) {
        verdict = classify_dawn_scene(dawn_fields(2, 2, 0, static_cast<std::uint8_t>(pages.first), static_cast<std::uint8_t>(pages.second)), true);
        CHECK(verdict.singleScreen && verdict.gameplay && !verdict.mapReady && verdict.mapPanel == -1);
    }
    for (const auto& pages : {std::pair<int, int>{0, 2}, {2, 0}, {3, 0}, {11, 10}}) {
        verdict = classify_dawn_scene(dawn_fields(2, 2, 0, static_cast<std::uint8_t>(pages.first), static_cast<std::uint8_t>(pages.second)), true);
        CHECK(verdict.singleScreen && !verdict.gameplay && !verdict.mapReady && verdict.viewPages == 0);
    }
    // Sub-states: entering, room change and area load keep single screen
    // without a map; unknown states and sub-state 4 outside the warp room's
    // choice (here with the map page, step 0) do not.
    for (unsigned state = 0; state <= 0xff; ++state) {
        const bool known = state <= 3 || state == 6;
        verdict = classify_dawn_scene(dawn_fields(2, static_cast<std::uint8_t>(state), 0, 0, 0), true);
        CHECK(verdict.singleScreen == known && verdict.gameplay == known);
        CHECK(verdict.mapReady == (state == DawnRunningState));
        verdict = classify_dawn_scene(dawn_fields(2, static_cast<std::uint8_t>(state), 0, 3, 3), true);
        CHECK(verdict.singleScreen == known && !verdict.gameplay && !verdict.mapReady);
    }
    // Running with the map, but the frozen game, a fade or a moment the game
    // refuses its own pause menu: single screen, View consumed, nothing opens.
    fields = dawn_fields(2);
    fields.frozen = 1;
    verdict = classify_dawn_scene(fields, true);
    CHECK(verdict.singleScreen && verdict.gameplay && !verdict.mapReady);
    fields = dawn_fields(2);
    fields.fadeFrames = 3;
    CHECK(classify_dawn_scene(fields, true).gameplay && !classify_dawn_scene(fields, true).mapReady);
    fields = dawn_fields(2);
    fields.brightness = -0x10000;
    CHECK(classify_dawn_scene(fields, true).gameplay && !classify_dawn_scene(fields, true).mapReady);
    for (const std::uint32_t flag : {0x1u, 0x200000u}) {
        fields = dawn_fields(2);
        fields.eventFlags = flag;
        CHECK(classify_dawn_scene(fields, true).gameplay && !classify_dawn_scene(fields, true).mapReady);
    }
    // Unrelated event bits change nothing.
    fields = dawn_fields(2);
    fields.eventFlags = 0x80 | 0x200 | 0x100000;
    CHECK(classify_dawn_scene(fields, true).mapReady && classify_dawn_scene(fields, true).viewPages == 2);
    CHECK(same_dawn_fields(dawn_fields(2), dawn_fields(2)) && !same_dawn_fields(dawn_fields(2), fields));
}

DawnStateFields logged_dawn(std::uint8_t mode, std::uint8_t state, std::uint8_t page, std::uint8_t requested,
                            std::uint8_t overlay, std::uint8_t frozen, std::uint32_t flags, std::int32_t fade,
                            std::int32_t level) {
    auto fields = dawn_fields(mode, state, 0, page, requested);
    fields.pageOverlay = overlay;
    fields.frozen = frozen;
    fields.eventFlags = flags;
    fields.fadeFrames = fade;
    fields.brightness = level;
    return fields;
}

// Scene fields logged in the first in-game session (28/10/2026, opening of a
// new game). Build c341626 kept the original layout throughout because the top
// screen never showed the map/Status pair; gameplay alone now decides.
void dawn_first_session_fields_reach_single_screen_in_gameplay() {
    const auto& dawn = *adaptation_profile(Title::DawnOfSorrow);
    const auto shown = [&](const DawnStateFields& fields, bool enabled) {
        return title_presentation(dawn, classify_dawn_scene(fields, true), enabled, false);
    };
    // Boot logos and start/load modes (coming from an original layout):
    // original layout even with the mod on.
    const DawnStateFields boot[] = {
        logged_dawn(0x14, 0, 11, 10, 0, 0, 0, 0, 0), logged_dawn(0x15, 0, 11, 10, 0, 0, 0, 0, 0),
        logged_dawn(0x00, 0, 3, 3, 0, 0, 0, 0, 0), logged_dawn(0x01, 0, 3, 3, 0, 0, 0x51140081, 0, 0)};
    for (const auto& fields : boot) {
        const auto verdict = classify_dawn_scene(fields, true);
        CHECK(!verdict.singleScreen && !verdict.gameplay && !verdict.mapReady);
        CHECK(shown(fields, true) == Presentation::Original && shown(fields, false) == Presentation::Original);
    }
    // Title screen, title menu and file select with the decorative top page:
    // the main screen alone with the mod on, View native.
    const DawnStateFields menus[] = {
        logged_dawn(0x03, 0, 3, 3, 0, 0, 0, 0, -65536), logged_dawn(0x04, 0, 3, 3, 0, 0, 0, 30, -65536),
        logged_dawn(0x09, 0, 3, 3, 0, 0, 0x8000000, 7, -57344)};
    for (const auto& fields : menus) {
        const auto verdict = classify_dawn_scene(fields, true);
        CHECK(verdict.singleScreen && !verdict.gameplay && !verdict.mapReady);
        CHECK(shown(fields, true) == Presentation::Gameplay && shown(fields, false) == Presentation::Original);
    }
    // Gameplay before the game gives the map (decorative page 3, then Status
    // with its overlay, switching locked): single screen, View native.
    const DawnStateFields opening[] = {
        logged_dawn(2, 0, 3, 3, 0, 0, 0x51140081, 0, 0), logged_dawn(2, 1, 3, 3, 0, 0, 0x50040081, 0, 0),
        logged_dawn(2, 2, 3, 3, 0, 0, 0x50040081, 0, 0), logged_dawn(2, 2, 3, 3, 0, 0, 0x50040000, 0, 0),
        logged_dawn(2, 2, 1, 1, 1, 0, 0x50040000, 0, 0), logged_dawn(2, 2, 1, 1, 1, 1, 0x50040000, 0, 0)};
    for (const auto& fields : opening) {
        const auto verdict = classify_dawn_scene(fields, true);
        CHECK(verdict.singleScreen && !verdict.gameplay && !verdict.mapReady && verdict.viewPages == 0);
        CHECK(shown(fields, true) == Presentation::Gameplay && shown(fields, false) == Presentation::Original);
    }
    // The pause menu opened from there: the main screen alone, View native.
    const auto menu = logged_dawn(0x0a, 2, 1, 1, 1, 1, 0x58040000, 0, 0);
    const auto verdict = classify_dawn_scene(menu, true);
    CHECK(verdict.scene == GameScene::InGameMenu && verdict.singleScreen && !verdict.gameplay);
    CHECK(shown(menu, true) == Presentation::Gameplay);
}

// Scene fields logged in the second in-game session (28/10/2026, build
// 414645a): the title menus and the pause menu still showed both screens; the
// map arrived on page 0 after an area load, with the switching lock cleared.
void dawn_second_session_menus_and_map_fields() {
    const auto& dawn = *adaptation_profile(Title::DawnOfSorrow);
    // Title flow: logos original, title/menu/file select single, loading holds.
    CHECK(!classify_dawn_scene(logged_dawn(0x14, 0, 11, 10, 0, 0, 0, 0, 0), true).singleScreen);
    CHECK(classify_dawn_scene(logged_dawn(0x03, 0, 3, 3, 0, 0, 0, 0, -65536), true).singleScreen);
    CHECK(classify_dawn_scene(logged_dawn(0x04, 0, 3, 3, 0, 0, 0, 15, 61440), true).singleScreen);
    CHECK(classify_dawn_scene(logged_dawn(0x09, 0, 3, 3, 0, 0, 0x8000000, 0, 0), true).singleScreen);
    const auto load = classify_dawn_scene(logged_dawn(0x01, 0, 3, 3, 0, 0, 0x51140081, 0, 0), true);
    CHECK(load.holdLayout && !load.singleScreen);
    CHECK(title_presentation(dawn, load, true, false, Presentation::Gameplay) == Presentation::Gameplay);
    CHECK(title_presentation(dawn, load, true, false, Presentation::Map) == Presentation::Gameplay);
    CHECK(title_presentation(dawn, load, true, false, Presentation::Original) == Presentation::Original);
    CHECK(title_presentation(dawn, load, false, false, Presentation::Gameplay) == Presentation::Original);
    // The area load that brings the map: single, View the mod's, not ready.
    auto verdict = classify_dawn_scene(logged_dawn(2, 6, 0, 0, 0, 1, 0x40040280, 0, 0), true);
    CHECK(verdict.singleScreen && verdict.gameplay && !verdict.mapReady);
    verdict = classify_dawn_scene(logged_dawn(2, 2, 0, 0, 0, 0, 0x40040081, 58, -63352), true);
    CHECK(verdict.gameplay && !verdict.mapReady);
    // Then running with the map on page 0: the map opens from the upper panel.
    verdict = classify_dawn_scene(logged_dawn(2, 2, 0, 0, 0, 0, 0x40000000, 0, 0), true);
    CHECK(verdict.gameplay && verdict.mapReady && verdict.mapPanel == 0 && verdict.viewPages == 2);
    // The pause menu over it: the main screen alone, View native, no map.
    const auto pause = logged_dawn(0x0a, 2, 0, 0, 0, 1, 0x48000000, 0, 0);
    verdict = classify_dawn_scene(pause, true);
    CHECK(verdict.scene == GameScene::InGameMenu && verdict.singleScreen && !verdict.gameplay && !verdict.mapReady);
    CHECK(title_presentation(dawn, verdict, true, false, Presentation::Gameplay) == Presentation::Gameplay);
    // A level left at +-8/+-16 after a fade is a normal screen (applied
    // brightness level / 4096); two refusals of that session were these.
    for (const int level : {8, -8, 16, -16, 4095, -4095}) {
        verdict = classify_dawn_scene(logged_dawn(2, 2, 0, 0, 0, 0, 0x30, 0, level), true);
        CHECK(verdict.mapReady && verdict.mapPanel == 0);
    }
    for (const int level : {4096, -4096, 65536, -65536}) {
        CHECK(!classify_dawn_scene(logged_dawn(2, 2, 0, 0, 0, 0, 0x30, 0, level), true).mapReady);
    }
    // Bit 0x1 still refuses (the game's own menu is blocked there).
    CHECK(!classify_dawn_scene(logged_dawn(2, 2, 0, 0, 0, 0, 0x31, 0, 0), true).mapReady);
    // Native Select moved to Status (mod off): the map is then the lower panel.
    verdict = classify_dawn_scene(logged_dawn(2, 2, 1, 1, 0, 0, 0x40000000, 0, 0), true);
    CHECK(verdict.mapReady && verdict.mapPanel == 1 && verdict.viewPages == 2);
}

// The warp room (third round, 29/10/2026): fields logged in the second session
// around four destination choices, all shown in the original layout by builds
// up to 0f29e7f. The step byte was not logged yet; the only writer of
// sub-state 4 stores it with step 1 (0x180077390, the word 0x104).
void dawn_warp_choice_stays_on_the_main_screen() {
    const auto& dawn = *adaptation_profile(Title::DawnOfSorrow);
    const auto warp = [](std::uint8_t state, std::uint8_t page, std::uint8_t requested, std::uint8_t frozen,
                         std::uint32_t flags, std::int32_t fade, std::int32_t level, std::uint8_t step = 0) {
        auto fields = logged_dawn(2, state, page, requested, 0, frozen, flags, fade, level);
        fields.subState = step;
        return fields;
    };
    const auto choice = warp(4, 2, 2, 1, 0x1000031, 0, 0, DawnWarpChoiceStep);
    // Entering the warp room and standing in it: its own top page, single
    // screen, no map to enlarge (View stays native Select, which the game
    // ignores in warp rooms).
    const DawnStateFields room[] = {
        warp(3, 0, 2, 0, 0x100030, 0, 0), warp(0, 2, 2, 0, 0x100030, 0, 0, 1),
        warp(1, 2, 2, 0, 0x30, 0, 0, 1), warp(2, 2, 2, 0, 0x30, 0, 0, 1),
    };
    for (const auto& fields : room) {
        const auto verdict = classify_dawn_scene(fields, true);
        CHECK(verdict.singleScreen && !verdict.gameplay && !verdict.mapReady && verdict.mapPanel == -1);
    }
    // The choice: the main screen alone too, whatever the layout before.
    auto verdict = classify_dawn_scene(choice, true);
    CHECK(verdict.scene == GameScene::Gameplay && verdict.singleScreen && !verdict.holdLayout);
    CHECK(!verdict.gameplay && !verdict.mapReady && verdict.mapPanel == -1 && verdict.viewPages == 0);
    for (const auto previous : {Presentation::Original, Presentation::Gameplay})
        CHECK(title_presentation(dawn, verdict, true, false, previous) == Presentation::Gameplay);
    CHECK(title_presentation(dawn, verdict, false, false, Presentation::Gameplay) == Presentation::Original);
    // The event flag is set after the map is drawn and the freeze only in some
    // pause states: neither is required, so no frame flashes the original.
    for (const std::uint32_t flags : {0x0u, 0x1u, 0x1000000u, 0x1000031u}) {
        for (const std::uint8_t frozen : {0, 1}) {
            auto fields = choice;
            fields.eventFlags = flags;
            fields.frozen = frozen;
            CHECK(classify_dawn_scene(fields, true).singleScreen);
        }
    }
    // Leaving it: back in the room, or the warp itself (fades, the area load,
    // arrival in the other warp room) until the next room change.
    const DawnStateFields after[] = {
        warp(1, 2, 2, 0, 0x1000031, 0, 0), warp(2, 2, 2, 0, 0x31, 0, 0), warp(2, 2, 2, 0, 0x30, 0, 0),
        warp(2, 2, 2, 0, 0x31, 5, 54614), warp(2, 2, 2, 0, 0x31, 29, 2184), warp(2, 2, 2, 0, 0x31, 0, 65520),
        warp(6, 2, 2, 1, 0x231, 0, 65536), warp(6, 2, 2, 1, 0x100230, 0, 65536), warp(0, 2, 2, 1, 0x100230, 0, 65536),
        warp(1, 2, 2, 0, 0x30, 7, 57344), warp(2, 2, 2, 0, 0x30, 6, 49152), warp(2, 2, 2, 0, 0x30, 0, 0),
        warp(3, 2, 2, 0, 0x100030, 0, 0),
    };
    for (const auto& fields : after) {
        verdict = classify_dawn_scene(fields, true);
        CHECK(verdict.singleScreen && !verdict.mapReady);
        CHECK(title_presentation(dawn, verdict, true, false, Presentation::Gameplay) == Presentation::Gameplay);
    }
    // Anything else in sub-state 4 keeps the original layout: the step without
    // a producer (0x18002ad80), any other step, another top page or a switch.
    for (unsigned step = 0; step <= 0xff; ++step) {
        auto fields = choice;
        fields.subState = static_cast<std::uint8_t>(step);
        CHECK(classify_dawn_scene(fields, true).singleScreen == (step == DawnWarpChoiceStep));
    }
    for (const auto& pages : {std::pair<int, int>{0, 0}, {1, 1}, {3, 3}, {0, 2}, {2, 0}, {2, 3}}) {
        auto fields = choice;
        fields.page = static_cast<std::uint8_t>(pages.first);
        fields.requestedPage = static_cast<std::uint8_t>(pages.second);
        verdict = classify_dawn_scene(fields, true);
        CHECK(!verdict.singleScreen && !verdict.gameplay && !verdict.mapReady);
        CHECK(title_presentation(dawn, verdict, true, false, Presentation::Gameplay) == Presentation::Original);
    }
    // No other sub-state takes the exception, and no other game type.
    for (unsigned state = 0; state <= 0xff; ++state) {
        auto fields = choice;
        fields.gameState = static_cast<std::uint8_t>(state);
        CHECK(classify_dawn_scene(fields, true).singleScreen == (state <= 3 || state == 4 || state == 6));
    }
    for (const std::uint8_t type : {1, 2, 3, 4, 5, 0xff}) {
        auto fields = choice;
        fields.gameType = type;
        CHECK(!classify_dawn_scene(fields, true).singleScreen);
    }
    // The step is part of a stable sample.
    auto other = choice;
    other.subState = 0;
    CHECK(same_dawn_fields(choice, choice) && !same_dawn_fields(choice, other));
}

void title_presentation_keeps_portrait_and_follows_dawn_scenes() {
    const auto& portrait = *adaptation_profile(Title::PortraitOfRuin);
    const auto& dawn = *adaptation_profile(Title::DawnOfSorrow);
    const auto title = classify_portrait_scene(scene_fields(5, 0, 0, 6, 6), true);
    const auto play = classify_portrait_scene(scene_fields(2, 2, 0, 0, 0), true);
    // Portrait (v0.1.1): the main screen alone in every verified scene.
    for (const auto previous : {Presentation::Original, Presentation::Gameplay, Presentation::Map}) {
        CHECK(title_presentation(portrait, title, true, false, previous) == Presentation::Gameplay);
        CHECK(title_presentation(portrait, play, true, false, previous) == Presentation::Gameplay);
        CHECK(title_presentation(portrait, play, true, true, previous) == Presentation::Map);
        CHECK(title_presentation(portrait, play, false, false, previous) == Presentation::Original);
        CHECK(title_presentation(portrait, play, false, true, previous) == Presentation::Original);
    }
    // Dawn: single screen in gameplay, the title menus and the pause menu;
    // a start/load mode holds what was shown; anything else is original.
    const auto pause = classify_dawn_scene(dawn_fields(0x0a, 2, 0, 3, 3), true);
    const auto field = classify_dawn_scene(dawn_fields(2, 2, 0, 3, 3), true);
    const auto menu = classify_dawn_scene(dawn_fields(0x04, 0, 0, 3, 3), true);
    const auto over = classify_dawn_scene(dawn_fields(0x05, 0, 0, 3, 3), true);
    const auto load = classify_dawn_scene(dawn_fields(0x01, 0, 0, 3, 3), true);
    CHECK(title_presentation(dawn, pause, true, false) == Presentation::Gameplay);
    CHECK(title_presentation(dawn, field, true, false) == Presentation::Gameplay);
    CHECK(title_presentation(dawn, menu, true, false) == Presentation::Gameplay);
    CHECK(title_presentation(dawn, field, true, true) == Presentation::Map);
    CHECK(title_presentation(dawn, field, false, false) == Presentation::Original);
    CHECK(title_presentation(dawn, over, true, false, Presentation::Gameplay) == Presentation::Original);
    CHECK(title_presentation(dawn, load, true, false, Presentation::Gameplay) == Presentation::Gameplay);
    CHECK(title_presentation(dawn, load, true, false, Presentation::Original) == Presentation::Original);
    CHECK(title_presentation(dawn, load, true, true, Presentation::Original) == Presentation::Map);
    CHECK(title_presentation(dawn, SceneVerdict{}, true, false, Presentation::Gameplay) == Presentation::Original);
}

// Mirrors live_controller for Dawn over a whole session: the gate (input,
// gameplay, view readiness), the recognizer (View, View+Y), the mod's switch,
// the view and title_presentation.
void dawn_mod_toggles_in_menus_and_single_screen_starts_with_gameplay() {
    TitleGate gate;
    InputRecognizer recognizer;
    TitleObservation dawn{};
    dawn.title = Title::DawnOfSorrow;
    dawn.reason = TitleReason::Verified;
    dawn.fields = {FakeVtable, Dra01, DawnModuleIndex, 1};
    const auto& profile = *adaptation_profile(Title::DawnOfSorrow);
    bool enabled = false, view = false, refused = false;
    InputSnapshot sample{};
    Presentation presentation = Presentation::Original;
    const auto step = [&](std::uint16_t buttons, const DawnStateFields& fields, bool collectionPause = false) {
        const auto scene = classify_dawn_scene(fields, true);
        // A pause the mod does not own (the collection's menu) keeps input and
        // layout original (live_controller's gate and frame guard).
        const auto state = gate.update({dawn, true, collectionPause || view, view, scene});
        if (state.inputChanged) recognizer.reset();
        refused = false;
        std::uint16_t forwarded = buttons;
        if (state.input) {
            sample.buttons = buttons;
            const auto result = recognizer.update(sample, {true, enabled, enabled && state.gameplay, 1});
            forwarded = result.forwardedButtons;
            if (result.action == InputAction::ToggleMod) {
                enabled = !enabled;
                view = false;
            } else if (result.action == InputAction::ToggleMap) {
                if (view) view = false;
                else if (state.mapReady) view = true;
                else refused = true;
            }
        }
        presentation = state.identity && !collectionPause
                           ? title_presentation(profile, scene, enabled, view, presentation)
                           : Presentation::Original;
        return forwarded;
    };
    const auto collection = logged_dawn(0x14, 0, 11, 10, 0, 0, 0, 0, 0);
    const auto startMenu = logged_dawn(0x04, 0, 3, 3, 0, 0, 0, 0, 0);
    const auto loading = logged_dawn(0x01, 0, 3, 3, 0, 0, 0x51140081, 0, 0);
    const auto opening = logged_dawn(2, 2, 3, 3, 0, 0, 0x50040081, 0, 0);
    const auto field = logged_dawn(2, 2, 3, 3, 0, 0, 0x50040000, 0, 0);
    const auto blocked = logged_dawn(2, 2, 0, 0, 0, 0, 0x40040081, 0, 0);
    const auto pair = logged_dawn(2, 2, 0, 0, 0, 0, 0x40000000, 0, 0);
    const auto pauseMenu = logged_dawn(0x0a, 2, 0, 0, 0, 1, 0x48000000, 0, 0);
    // The collection's own menu pauses the emulator: View+Y passes unchanged
    // there and does not switch the mod (v0.1.1 isolation, kept).
    step(0, collection, true);
    step(0, collection, true);
    CHECK(step(ButtonView, collection, true) == ButtonView);
    CHECK(step(ButtonView | ButtonY, collection, true) == (ButtonView | ButtonY) && !enabled);
    step(0, collection, true);
    CHECK(presentation == Presentation::Original);
    // Dawn's title menu: View+Y switches the mod on and the main screen
    // appears alone at once.
    step(0, startMenu);
    CHECK(step(ButtonView, startMenu) == 0);
    CHECK(step(ButtonView | ButtonY, startMenu) == 0 && enabled);
    step(0, startMenu);
    CHECK(presentation == Presentation::Gameplay);
    // View alone in a menu stays native Select (replayed on release), no view.
    step(ButtonView, startMenu);
    CHECK(step(0, startMenu) == ButtonView && !view && presentation == Presentation::Gameplay);
    // Loading keeps the single screen into gameplay: no flash.
    step(0, loading);
    CHECK(presentation == Presentation::Gameplay);
    step(0, opening);
    CHECK(presentation == Presentation::Gameplay);
    // No map yet (decorative top page): View is native Select, nothing opens.
    CHECK(step(ButtonView, opening) == 0);
    CHECK(step(0, opening) == ButtonView && !refused && !view && presentation == Presentation::Gameplay);
    step(ButtonView, field);
    CHECK(step(0, field) == ButtonView && !refused && !view);
    // The map is there but the game refuses its own menu: View consumed, refused.
    CHECK(step(ButtonView, blocked) == 0);
    CHECK(step(0, blocked) == 0 && refused && !view && presentation == Presentation::Gameplay);
    // Map ready: View opens the enlarged map, View closes it.
    step(ButtonView, pair);
    CHECK(step(0, pair) == 0 && view && presentation == Presentation::Map);
    step(ButtonView, pair);
    step(0, pair);
    CHECK(!view && presentation == Presentation::Gameplay);
    // The pause menu: the main screen alone, View native; back to gameplay.
    step(0, pauseMenu);
    CHECK(presentation == Presentation::Gameplay);
    step(ButtonView, pauseMenu);
    CHECK(step(0, pauseMenu) == ButtonView && !view);
    step(0, pair);
    CHECK(presentation == Presentation::Gameplay);
    // View+Y in gameplay switches the mod off (both screens) and on again.
    step(ButtonView, pair);
    step(ButtonView | ButtonY, pair);
    step(0, pair);
    CHECK(!enabled && presentation == Presentation::Original);
    // Off: View is native Select even with the map.
    step(ButtonView, pair);
    CHECK(step(0, pair) == ButtonView && !view);
    step(ButtonView, pair);
    step(ButtonView | ButtonY, pair);
    step(0, pair);
    CHECK(enabled && presentation == Presentation::Gameplay);
    // View+Y with the view open closes it with the mod.
    step(ButtonView, pair);
    step(0, pair);
    CHECK(view);
    step(ButtonView, pair);
    step(ButtonView | ButtonY, pair);
    step(0, pair);
    CHECK(!enabled && !view && presentation == Presentation::Original);
    // Off in a menu, through loading: original layout, held as such.
    step(0, startMenu);
    step(0, loading);
    CHECK(presentation == Presentation::Original);
    // The collection's menu over gameplay with the mod on: original layout.
    step(ButtonView, pair);
    step(ButtonView | ButtonY, pair);
    step(0, pair);
    CHECK(enabled && presentation == Presentation::Gameplay);
    step(0, pair, true);
    CHECK(presentation == Presentation::Original);
    step(0, pair);
    CHECK(presentation == Presentation::Gameplay);
}

void scene_dispatch_follows_the_verified_title() {
    TitleSceneFields fields{};
    fields.portrait = scene_fields(2, 2, 0, 0, 0);
    fields.dawn = dawn_fields(2);
    CHECK(classify_scene(fields, true).scene == GameScene::Unknown);
    fields.title = Title::OrderOfEcclesia;
    CHECK(classify_scene(fields, true).scene == GameScene::Unknown);
    fields.title = Title::PortraitOfRuin;
    auto verdict = classify_scene(fields, true);
    // Portrait's verdict is the v0.1.1 one: no Dawn page fields involved.
    CHECK(verdict.gameplay && verdict.mapReady && verdict.mapPanel == -1 && !verdict.singleScreen);
    fields.portrait = scene_fields(7, 2, 0, 0, 0);
    CHECK(classify_scene(fields, true).scene == GameScene::InGameMenu);
    fields.title = Title::DawnOfSorrow;
    verdict = classify_scene(fields, true);
    CHECK(verdict.singleScreen && verdict.mapReady && verdict.mapPanel == 0 && verdict.viewPages == 2);
    CHECK(!classify_scene(fields, false).singleScreen);
}

void page_buttons_reserve_until_release_and_never_leak() {
    PageButtonFilter f;
    // Outside the view LB/RB are the game's.
    auto r = f.update(ButtonLeftShoulder, true, false);
    CHECK(r.consumedButtons == 0 && r.step == 0);
    CHECK(f.update(0, true, false).consumedButtons == 0);
    // In the view: LB back, RB forward, consumed while held.
    r = f.update(ButtonRightShoulder, true, true);
    CHECK(r.consumedButtons == ButtonRightShoulder && r.step == 1);
    r = f.update(ButtonRightShoulder, true, true);
    CHECK(r.consumedButtons == ButtonRightShoulder && r.step == 0);  // no repeat
    f.update(0, true, true);
    r = f.update(ButtonLeftShoulder, true, true);
    CHECK(r.consumedButtons == ButtonLeftShoulder && r.step == -1);
    f.update(0, true, true);
    // Both starting together cancel out, both consumed.
    r = f.update(PageButtons, true, true);
    CHECK(r.consumedButtons == PageButtons && r.step == 0);
    // The view closes with both held: still consumed until each is released.
    r = f.update(PageButtons, true, false);
    CHECK(r.consumedButtons == PageButtons);
    r = f.update(ButtonRightShoulder, true, false);
    CHECK(r.consumedButtons == ButtonRightShoulder && f.reserved() == ButtonRightShoulder);
    CHECK(f.update(0, true, false).consumedButtons == 0 && f.reserved() == 0);
    CHECK(f.update(ButtonLeftShoulder, true, false).consumedButtons == 0);  // a new press is the game's
    f.update(0, true, false);
    // Held before the view opened: not a command, passes, no edge on resume.
    f.update(ButtonLeftShoulder, true, false);
    r = f.update(ButtonLeftShoulder, true, true);
    CHECK(r.consumedButtons == 0 && r.step == 0);
    // Released and pressed again inside the view: a command, reserved.
    f.update(0, true, true);
    r = f.update(ButtonLeftShoulder, true, true);
    CHECK(r.consumedButtons == ButtonLeftShoulder && r.step == -1);
    // A disconnection keeps the reservation; after it a held button is not new.
    f.update(0, false, true);
    r = f.update(ButtonLeftShoulder | ButtonRightShoulder, true, true);
    CHECK(r.consumedButtons == ButtonLeftShoulder && r.step == 0);
    f.update(0, true, false);
    CHECK(f.reserved() == 0);
    // Only LB/RB are ever consumed, for every button combination.
    PageButtonFilter all;
    for (std::uint32_t buttons = 0; buttons <= 0xffff; ++buttons) {
        const auto raw = static_cast<std::uint16_t>(buttons);
        r = all.update(raw, true, buttons % 5 != 0);
        CHECK((r.consumedButtons & ~PageButtons) == 0 && (r.consumedButtons & ~raw) == 0);
    }
    // The view owns every press but View (the recognizer's) when asked to.
    PageButtonFilter owner;
    owner.update(ButtonB, true, false, ViewReservableButtons);
    r = owner.update(ButtonB | ButtonA | ButtonView | ButtonY, true, true, ViewReservableButtons);
    CHECK(r.consumedButtons == (ButtonA | ButtonY) && r.step == 0);   // B was already held
    r = owner.update(ButtonB | ButtonA | ButtonY, true, false, ViewReservableButtons);
    CHECK(r.consumedButtons == (ButtonA | ButtonY));
    r = owner.update(ButtonB | ButtonA, true, false, ViewReservableButtons);
    CHECK(r.consumedButtons == ButtonA && owner.reserved() == ButtonA);
    CHECK(owner.update(ButtonB, true, false, ViewReservableButtons).consumedButtons == 0 && owner.reserved() == 0);
    PageButtonFilter every;
    for (std::uint32_t buttons = 0; buttons <= 0xffff; ++buttons) {
        const auto raw = static_cast<std::uint16_t>(buttons);
        r = every.update(raw, true, buttons % 3 != 0, ViewReservableButtons);
        CHECK((r.consumedButtons & (ButtonView | DirectionButtons)) == 0 && (r.consumedButtons & ~raw) == 0);
    }
    CHECK(step_page(0, -1, 2) == 1 && step_page(1, 1, 2) == 0 && step_page(0, 0, 2) == 0);
    CHECK(step_page(1, 3, 2) == 0 && step_page(0, -5, 3) == 1 && step_page(4, 1, 0) == 4);
}

// Mirrors live_controller for Dawn: the gate gives input and gameplay, the
// recognizer View/Y, and the page filter LB/RB while the view reserves them
// (from the ToggleMap that opens it until its close is confirmed).
// Mirrors live_controller for Dawn, one controller sample then one frame per
// step: the gate, the recognizer, ViewInputOwner and PageButtonFilter in the
// filter; pause replies one frame after their request, then the pending View
// command, in frame().
struct DawnViewSession {
    TitleGate gate;
    InputRecognizer recognizer;
    PageButtonFilter pages;
    ViewInputOwner owner;
    TitleObservation dawn{};
    InputSnapshot sample{};
    bool shown = false, acquiring = false, releasing = false, reserving = false;
    InputAction pending = InputAction::None;
    unsigned page = 0, viewPages = 0;
    DawnViewSession() {
        dawn.title = Title::DawnOfSorrow;
        dawn.reason = TitleReason::Verified;
        dawn.fields = {FakeVtable, Dra01, DawnModuleIndex, 1};
    }
    std::uint16_t step(std::uint16_t buttons, const SceneVerdict& scene) {
        const auto state = gate.update({dawn, true, shown, shown || acquiring || releasing, scene});
        if (state.inputChanged) recognizer.reset();
        std::uint16_t forwarded = buttons;
        if (state.input) {
            sample.buttons = buttons;
            const auto result = recognizer.update(sample, {true, true, state.gameplay, 1});
            if (result.action == InputAction::ToggleMod || pending == InputAction::None) pending = result.action;
            const bool opening = acquiring || (pending == InputAction::ToggleMap && state.mapReady);
            reserving = owner.update(shown, opening, result.action);
            const auto p = pages.update(buttons, true, reserving, ViewReservableButtons);
            if (p.step && shown) page = step_page(page, p.step, viewPages);
            forwarded = static_cast<std::uint16_t>(result.forwardedButtons & ~p.consumedButtons);
        }
        // frame(): replies first, then the pending command.
        if (acquiring) { acquiring = false; shown = true; page = 0; viewPages = scene.viewPages; }
        else if (releasing) releasing = false;
        if (pending == InputAction::ToggleMap && !acquiring && !releasing) {
            if (shown) { shown = false; releasing = true; }
            else if (state.mapReady) acquiring = true;
        }
        pending = InputAction::None;
        return forwarded;
    }
};

void dawn_view_never_leaks_page_buttons_into_the_game() {
    DawnViewSession v;
    const auto ready = classify_dawn_scene(dawn_fields(2, 2, 0, 0, 0), true);
    const auto menu = classify_dawn_scene(dawn_fields(0x0a, 2, 0, 3, 3), true);
    const auto event = classify_dawn_scene(dawn_fields(2, 2, 0, 2, 2), true);
    v.step(0, ready);
    v.step(0, ready);
    // View opens the enlarged view on the map (its pause one frame later).
    CHECK(v.step(ButtonView, ready) == 0);
    CHECK(v.step(0, ready) == 0 && v.reserving && !v.shown);
    CHECK(v.step(0, ready) == 0 && v.shown && v.page == 0);
    // RB/LB switch pages; an action pressed in the view is kept from the
    // paused game too.
    CHECK(v.step(ButtonRightShoulder, ready) == 0 && v.page == 1);
    CHECK(v.step(ButtonA | ButtonRightShoulder, ready) == 0 && v.page == 1);
    CHECK(v.step(0, ready) == 0);
    CHECK(v.step(ButtonLeftShoulder, ready) == 0 && v.page == 0);
    // Close with LB still held: View consumed, LB consumed until released.
    CHECK(v.step(ButtonLeftShoulder | ButtonView, ready) == 0);
    CHECK(v.step(ButtonLeftShoulder, ready) == 0 && !v.shown && !v.reserving);
    CHECK(v.step(ButtonLeftShoulder, ready) == 0);
    CHECK(v.step(0, ready) == 0);
    // Now LB/RB are gameplay buttons again.
    CHECK(v.step(ButtonLeftShoulder, ready) == ButtonLeftShoulder);
    CHECK(v.step(ButtonRightShoulder, ready) == ButtonRightShoulder);
    v.step(0, ready);
    // A and Start pressed in the view and held at close reach the game only
    // after a release; directions pressed in the view are held movement and
    // pass; a direction already held when it opened passes throughout.
    constexpr std::uint16_t Start = 0x0010, Right = 0x0008, Up = 0x0001;
    CHECK(v.step(Right, ready) == Right);
    CHECK(v.step(Right | ButtonView, ready) == Right);
    CHECK(v.step(Right, ready) == Right);
    CHECK(v.step(Right, ready) == Right && v.shown);
    CHECK(v.step(Right | ButtonA | Start | Up, ready) == (Right | Up));
    CHECK(v.step(Right | ButtonA | Start | Up | ButtonView, ready) == (Right | Up));
    CHECK(v.step(Right | ButtonA | Start | Up, ready) == (Right | Up) && !v.shown);
    CHECK(v.step(Right | ButtonA | Start | Up, ready) == (Right | Up) && !v.reserving);
    CHECK(v.step(Right | ButtonA, ready) == Right);
    CHECK(v.step(Right, ready) == Right);
    CHECK(v.step(Right | ButtonA | Up, ready) == (Right | ButtonA | Up));
    v.step(0, ready);
    // Pause menu: View stays native Select, LB/RB untouched.
    CHECK(v.step(ButtonView, menu) == 0);
    CHECK(v.step(0, menu) == ButtonView && !v.shown);
    CHECK(v.step(ButtonRightShoulder, menu) == ButtonRightShoulder);
    v.step(0, menu);
    // An event's own top-screen page (no map): View stays native Select and
    // LB/RB are the game's.
    CHECK(v.step(ButtonView, event) == 0);
    CHECK(v.step(0, event) == ButtonView && !v.shown);
    CHECK(v.step(ButtonRightShoulder, event) == ButtonRightShoulder);
    v.step(0, event);
}

// Second session (28/10/2026): after closing the map with View, a direction
// pressed right away was sometimes ignored until released and pressed again;
// held from before opening it worked (logged: d-pad bits withheld by the
// filter). The view now lets go of input at the close request.
void directions_and_actions_after_closing_the_view_reach_the_game() {
    const auto ready = classify_dawn_scene(dawn_fields(2, 2, 0, 0, 0), true);
    constexpr std::uint16_t Right = 0x0008, Left = 0x0004;
    const auto open = [&](DawnViewSession& v) {
        v.step(ButtonView, ready);
        v.step(0, ready);
        v.step(0, ready);
        CHECK(v.shown);
    };
    // A: held before opening, held through it: always the game's.
    {
        DawnViewSession v;
        v.step(0, ready);
        v.step(0, ready);
        CHECK(v.step(Right, ready) == Right);
        CHECK(v.step(Right | ButtonView, ready) == Right);
        CHECK(v.step(Right, ready) == Right);
        CHECK(v.step(Right, ready) == Right && v.shown);
        CHECK(v.step(Right | ButtonView, ready) == Right);
        CHECK(v.step(Right, ready) == Right && !v.shown);
        CHECK(v.step(Right, ready) == Right && !v.releasing);
    }
    // B: pressed while its pause is being released, on the very sample View
    // is let go, or while View is still held to close: the game's at once.
    for (int when = 0; when < 3; ++when) {
        DawnViewSession v;
        v.step(0, ready);
        v.step(0, ready);
        open(v);
        if (when == 0) {
            CHECK(v.step(ButtonView, ready) == 0);
            CHECK(v.step(0, ready) == 0 && !v.shown && v.releasing);
            CHECK(v.step(Left, ready) == Left);
        } else if (when == 1) {
            CHECK(v.step(ButtonView, ready) == 0);
            CHECK(v.step(Left, ready) == Left);
        } else {
            CHECK(v.step(ButtonView, ready) == 0);
            CHECK(v.step(ButtonView | Left, ready) == Left);
            CHECK(v.step(Left, ready) == Left);
        }
        for (int i = 0; i < 5; ++i) CHECK(v.step(Left, ready) == Left);
        CHECK(v.pages.reserved() == 0);
    }
    // Actions: pressed in the view and held through the close, withheld until
    // released; pressed after the close request, the game's at once.
    {
        DawnViewSession v;
        v.step(0, ready);
        v.step(0, ready);
        open(v);
        CHECK(v.step(ButtonA, ready) == 0);
        CHECK(v.step(ButtonA | ButtonView, ready) == 0);
        CHECK(v.step(ButtonA, ready) == 0 && !v.shown);
        CHECK(v.step(ButtonA, ready) == 0);
        CHECK(v.step(0, ready) == 0);
        CHECK(v.step(ButtonA, ready) == ButtonA);
        v.step(0, ready);
        open(v);
        v.step(ButtonView, ready);
        CHECK(v.step(ButtonA, ready) == ButtonA);
        CHECK(v.step(ButtonB | ButtonA, ready) == (ButtonB | ButtonA));
        v.step(0, ready);
        open(v);
        v.step(ButtonView, ready);
        v.step(0, ready);
        CHECK(v.step(ButtonRightShoulder, ready) == ButtonRightShoulder);
        CHECK(v.step(ButtonLeftShoulder | ButtonRightShoulder, ready) == PageButtons);
    }
}

void view_input_owner_ends_at_the_close_request() {
    ViewInputOwner owner;
    CHECK(!owner.update(false, false, InputAction::None));
    CHECK(owner.update(false, true, InputAction::ToggleMap));   // the opening request
    CHECK(owner.update(false, true, InputAction::None));        // its pause in flight
    CHECK(owner.update(true, false, InputAction::None));        // shown
    CHECK(owner.update(true, true, InputAction::None));
    CHECK(!owner.update(true, false, InputAction::ToggleMap));  // the close request
    CHECK(!owner.update(true, false, InputAction::None));       // until frame() closes it
    CHECK(!owner.update(false, false, InputAction::None));      // its pause being released
    CHECK(owner.update(false, true, InputAction::ToggleMap));   // opened again
    CHECK(owner.update(true, false, InputAction::None));
    CHECK(!owner.update(true, false, InputAction::ToggleMod));  // View+Y closes too
    owner.reset();
    CHECK(owner.update(true, false, InputAction::None));
}

void presented_points_map_back_to_the_original_panel() {
    const NormalizedRect gameplay{0.f, 1.f / 18.f, 2.f / 3.f, 8.f / 9.f};
    const auto shown = fit_4_3(1920, 1080);
    CHECK(shown.valid && shown.left == 240 && shown.width == 1440 && shown.height == 1080);
    // The centre of the enlarged image is the centre of the original panel.
    auto p = presented_to_original(shown, 1920, 1080, gameplay, 1920, 1080, 960, 540);
    CHECK(p.inside && p.x == 640 && p.y == 540);
    p = presented_to_original(shown, 1920, 1080, gameplay, 1920, 1080, 240, 0);
    CHECK(p.inside && p.x == 0 && p.y == 60);
    p = presented_to_original(shown, 1920, 1080, gameplay, 1920, 1080, 1679, 1079);
    CHECK(p.inside && p.x == 1279 && p.y == 1019);
    // The black bars are no touch.
    CHECK(!presented_to_original(shown, 1920, 1080, gameplay, 1920, 1080, 239, 540).inside);
    CHECK(!presented_to_original(shown, 1920, 1080, gameplay, 1920, 1080, 1680, 540).inside);
    // A client area scaled from the back buffer maps proportionally.
    p = presented_to_original(shown, 1920, 1080, gameplay, 960, 540, 480, 270);
    CHECK(p.inside && p.x == 320 && p.y == 270);
    // 2560x1417 (a windowed session): still inside the gameplay panel.
    const auto tall = fit_4_3(2560, 1417);
    p = presented_to_original(tall, 2560, 1417, gameplay, 2560, 1417, 1280, 708);
    CHECK(p.inside && p.x >= 850 && p.x <= 856 && p.y >= 700 && p.y <= 716);
    // Invalid input is never a touch.
    CHECK(!presented_to_original(Geometry{}, 1920, 1080, gameplay, 1920, 1080, 960, 540).inside);
    CHECK(!presented_to_original(shown, 0, 1080, gameplay, 1920, 1080, 960, 540).inside);
    CHECK(!presented_to_original(shown, 1920, 1080, gameplay, 0, 1080, 960, 540).inside);
    CHECK(!presented_to_original(shown, 1920, 1080, gameplay, 1920, 1080, -10000, -10000).inside);
    CHECK(!presented_to_original(shown, 1920, 1080, gameplay, 1920, 1080, 1920, 540).inside);
    CHECK(!presented_to_original(shown, 1920, 1080, NormalizedRect{0.f, 0.f, 0.f, 1.f}, 1920, 1080, 960, 540).inside);
    // Every point inside the image lands inside the source panel.
    for (int y = 0; y < 1080; y += 7) {
        for (int x = 240; x < 1680; x += 11) {
            p = presented_to_original(shown, 1920, 1080, gameplay, 1920, 1080, x, y);
            CHECK(p.inside && p.x >= 0 && p.x < 1280 && p.y >= 60 && p.y < 1020);
        }
    }
}

void pointer_is_hidden_in_the_view_and_until_release() {
    PointerReservation r;
    CHECK(!r.update(false, false) && !r.update(false, true));  // gameplay: untouched
    CHECK(r.update(true, false) && r.update(true, true));      // view: hidden
    CHECK(r.update(false, true));                              // closed, button still held
    CHECK(!r.update(false, false));                            // released
    CHECK(!r.update(false, true));                             // a new click is the game's
    CHECK(r.update(true, false));
    CHECK(!r.update(false, false));                            // nothing held at close
    r.update(true, true);
    r.reset();
    CHECK(!r.update(false, true));
}
}

int main() {
    struct Test { const char* name; void (*run)(); };
    const Test tests[] = {
        {"marker_cycle_preserves_thirteen_states_and_thirty_nine_native_ticks", marker_cycle_preserves_thirteen_states_and_thirty_nine_native_ticks},
        {"marker_cycle_repeated_images_keep_order_and_individual_durations", marker_cycle_repeated_images_keep_order_and_individual_durations},
        {"marker_cycle_partial_contains_only_completed_observed_runs", marker_cycle_partial_contains_only_completed_observed_runs},
        {"marker_cycle_invalidates_movement_identity_background_and_stopped_source", marker_cycle_invalidates_movement_identity_background_and_stopped_source},
        {"marker_cycle_sampling_gaps_clock_reversal_reset_and_recover", marker_cycle_sampling_gaps_clock_reversal_reset_and_recover},
        {"marker_cycle_supports_sixteen_states_and_refuses_seventeenth", marker_cycle_supports_sixteen_states_and_refuses_seventeenth},
        {"marker_cycle_excludes_pause_without_changing_measured_holds", marker_cycle_excludes_pause_without_changing_measured_holds},
        {"marker_cycle_retains_across_ten_static_reopenings_and_repeated_images", marker_cycle_retains_across_ten_static_reopenings_and_repeated_images},
        {"marker_cycle_frame_boundaries_are_rational_and_pause_requires_fresh_capture", marker_cycle_frame_boundaries_are_rational_and_pause_requires_fresh_capture},
        {"marker_cycle_long_holds_obey_limits_and_stale_source_loses_confidence", marker_cycle_long_holds_obey_limits_and_stale_source_loses_confidence},
        {"marker_cycle_single_lap_thirty_nine_tick_revisit_closure", marker_cycle_single_lap_thirty_nine_tick_revisit_closure},
        {"image_difference_counts_and_bounds", image_difference_counts_and_bounds},
        {"blink_tracker_follows_strict_alternation", blink_tracker_follows_strict_alternation},
        {"blink_plan_accepts_only_a_live_small_alternation", blink_plan_accepts_only_a_live_small_alternation},
        {"map_panel_is_the_one_without_the_brown_enemy_panel", map_panel_is_the_one_without_the_brown_enemy_panel},
        {"geometry_and_touch", geometry_and_touch},
        {"title_profiles_require_runtime_validation", title_profiles_require_runtime_validation},
        {"view_release_and_repeated_polls", view_release_and_repeated_polls},
        {"disabled_and_unsupported_view_is_replayed_once", disabled_and_unsupported_view_is_replayed_once},
        {"chords_toggle_once_and_drain_across_mode_changes", chords_toggle_once_and_drain_across_mode_changes},
        {"y_before_view_is_not_retroactively_consumed", y_before_view_is_not_retroactively_consumed},
        {"release_rollover_with_new_y_does_not_leak_native_combo", release_rollover_with_new_y_does_not_leak_native_combo},
        {"pending_gestures_cancel_at_context_and_mode_boundaries", pending_gestures_cancel_at_context_and_mode_boundaries},
        {"focus_disconnect_and_controller_identity", focus_disconnect_and_controller_identity},
        {"unvalidated_input_is_passthrough", unvalidated_input_is_passthrough},
        {"exhaustive_short_input_sequences_preserve_other_buttons", exhaustive_short_input_sequences_preserve_other_buttons},
        {"owned_pause_and_idempotent_close", owned_pause_and_idempotent_close},
        {"preexisting_pause_is_never_owned_or_resumed", preexisting_pause_is_never_owned_or_resumed},
        {"external_pause_requests_supersede_ownership", external_pause_requests_supersede_ownership},
        {"failed_releases_are_retained_and_retried_safely", failed_releases_are_retained_and_retried_safely},
        {"acquire_failures_cannot_produce_a_fake_pause", acquire_failures_cannot_produce_a_fake_pause},
        {"lost_pause_availability_closes_map_without_blind_resume", lost_pause_availability_closes_map_without_blind_resume},
        {"coordinator_defaults_fail_open", coordinator_defaults_fail_open},
        {"coordinator_map_and_global_toggle", coordinator_map_and_global_toggle},
        {"coordinator_falls_back_for_special_scenes_and_identity_changes", coordinator_falls_back_for_special_scenes_and_identity_changes},
        {"coordinator_pause_failure_and_external_changes", coordinator_pause_failure_and_external_changes},
        {"coordinator_focus_reconnect_release_pending_and_shutdown", coordinator_focus_reconnect_release_pending_and_shutdown},
        {"coordinator_shared_profiles_never_imply_support", coordinator_shared_profiles_never_imply_support},
        {"title_detector_never_verifies_boot_menus_or_other_titles", title_detector_never_verifies_boot_menus_or_other_titles},
        {"title_detector_requires_the_loaded_verified_dra02", title_detector_requires_the_loaded_verified_dra02},
        {"title_detector_caches_file_checks_per_image_and_epoch", title_detector_caches_file_checks_per_image_and_epoch},
        {"title_detector_rejects_races_and_unloads", title_detector_rejects_races_and_unloads},
        {"title_detector_follows_portrait_dawn_portrait_transitions", title_detector_follows_portrait_dawn_portrait_transitions},
        {"portrait_gate_confirms_identity_and_revokes_at_once", portrait_gate_confirms_identity_and_revokes_at_once},
        {"portrait_gate_keeps_input_out_of_foreign_pauses", portrait_gate_keeps_input_out_of_foreign_pauses},
        {"gate_changes_never_carry_a_gesture_across_contexts", gate_changes_never_carry_a_gesture_across_contexts},
        {"portrait_scene_separates_title_menus_and_gameplay", portrait_scene_separates_title_menus_and_gameplay},
        {"map_command_opens_only_ready_maps_and_always_closes", map_command_opens_only_ready_maps_and_always_closes},
        {"portrait_gate_separates_identity_gameplay_and_map", portrait_gate_separates_identity_gameplay_and_map},
        {"view_follows_scene_without_duplicate_or_stray_commands", view_follows_scene_without_duplicate_or_stray_commands},
        {"adaptation_profiles_keep_portrait_v011_and_isolate_dawn", adaptation_profiles_keep_portrait_v011_and_isolate_dawn},
        {"dawn_marker_cycle_fits_its_sprite_and_keeps_portrait_limits", dawn_marker_cycle_fits_its_sprite_and_keeps_portrait_limits},
        {"title_detector_keeps_dawn_inert_unless_enabled", title_detector_keeps_dawn_inert_unless_enabled},
        {"title_detector_hashes_each_title_once_across_switches", title_detector_hashes_each_title_once_across_switches},
        {"title_gate_needs_fresh_confirmations_for_each_title", title_gate_needs_fresh_confirmations_for_each_title},
        {"dawn_scene_follows_gameplay_and_never_trusts_the_page_alone", dawn_scene_follows_gameplay_and_never_trusts_the_page_alone},
        {"dawn_first_session_fields_reach_single_screen_in_gameplay", dawn_first_session_fields_reach_single_screen_in_gameplay},
        {"dawn_second_session_menus_and_map_fields", dawn_second_session_menus_and_map_fields},
        {"dawn_warp_choice_stays_on_the_main_screen", dawn_warp_choice_stays_on_the_main_screen},
        {"title_presentation_keeps_portrait_and_follows_dawn_scenes", title_presentation_keeps_portrait_and_follows_dawn_scenes},
        {"dawn_mod_toggles_in_menus_and_single_screen_starts_with_gameplay", dawn_mod_toggles_in_menus_and_single_screen_starts_with_gameplay},
        {"scene_dispatch_follows_the_verified_title", scene_dispatch_follows_the_verified_title},
        {"page_buttons_reserve_until_release_and_never_leak", page_buttons_reserve_until_release_and_never_leak},
        {"dawn_view_never_leaks_page_buttons_into_the_game", dawn_view_never_leaks_page_buttons_into_the_game},
        {"directions_and_actions_after_closing_the_view_reach_the_game", directions_and_actions_after_closing_the_view_reach_the_game},
        {"view_input_owner_ends_at_the_close_request", view_input_owner_ends_at_the_close_request},
        {"presented_points_map_back_to_the_original_panel", presented_points_map_back_to_the_original_panel},
        {"pointer_is_hidden_in_the_view_and_until_release", pointer_is_hidden_in_the_view_and_until_release},
    };
    int failures = 0;
    for (const auto& test : tests) {
        try {
            test.run();
            std::cout << "PASS " << test.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL " << test.name << ": " << error.what() << '\n';
        }
    }
    std::cout << (sizeof(tests) / sizeof(tests[0])) << " test groups, " << failures << " failures\n";
    return failures ? 1 : 0;
}
