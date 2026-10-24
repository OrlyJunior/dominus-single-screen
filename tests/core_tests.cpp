#include "dominus/core.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>

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
