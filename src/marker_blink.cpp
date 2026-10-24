#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "dominus/marker_blink.hpp"
#include "dominus/core.hpp"
#include "dominus/marker_identity.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace dominus::marker {
namespace {
constexpr unsigned SettleFrames = 3;
constexpr unsigned long long SettleBudgetMs = 1500;
constexpr unsigned TraceLinesPerSecond = 24, MaxDumps = 40;
struct Panel {
    gfx::PanelProbe probe;
    MarkerCycleTracker tracker;
    std::vector<std::uint32_t> image = std::vector<std::uint32_t>(NativeWidth * NativeHeight);
    unsigned long long window = 0, dropped = 0;
    unsigned linesInWindow = 0;
    unsigned long long lastIdentity = 0, currentIdentity = 0;
    bool identityAvailable = false;
    bool cycleReady = false;
};
struct State {
    Settings settings{};
    std::wstring directory;
    Log log = nullptr;
    bool configured = false, following = false;
    ID3D11Device* device = nullptr; // identity only; never dereferenced
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    UINT width = 0, height = 0;
    gfx::SourceRect regions[2]{};
    Panel panels[2];
    int mapPanel = -1;
    bool planned = false, replaying = false;
    unsigned settle = 0, flips = 0, dumps = 0, shownSlot = 0;
    unsigned long long mapOpenedMs = 0;
    MarkerCyclePlan plan{};
    unsigned long long planIdentity = 0;
    std::vector<std::uint32_t> frozen = std::vector<std::uint32_t>(NativeWidth * NativeHeight);
    bool openPauseSupplied = false;
    std::uint64_t openTaskEpoch = 0, openLeaseToken = 0, openConnectionEpoch = 0;
    bool awaitingUnpause = false;
    int awaitingPanel = -1;
    unsigned long long pauseStartMs = 0, mapClosedMs = 0;
    std::uint64_t awaitingTaskEpoch = 0, awaitingLeaseToken = 0, awaitingConnectionEpoch = 0;
    unsigned long long awaitingIdentity = 0, awaitingGeneration = 0;
};
State& state() { static auto* s = new State; return *s; }
unsigned long long now_ms() noexcept {
    static const LONGLONG frequency = [] { LARGE_INTEGER f{}; QueryPerformanceFrequency(&f); return f.QuadPart; }();
    LARGE_INTEGER c{}; QueryPerformanceCounter(&c);
    return frequency ? static_cast<unsigned long long>((c.QuadPart / frequency) * 1000 +
                       (c.QuadPart % frequency) * 1000 / frequency) : GetTickCount64();
}
void emit(const char* line) noexcept { if (state().log) state().log(line); }
void clear_panel(Panel& p) noexcept {
    p.tracker.reset(); p.probe.invalidate_kept(); p.probe.discard_pending();
    p.lastIdentity = p.currentIdentity = 0; p.identityAvailable = false;
    p.cycleReady = false;
}
void clear_awaiting() noexcept {
    auto& s = state();
    s.awaitingUnpause = false; s.awaitingPanel = -1;
    s.pauseStartMs = s.mapClosedMs = 0;
    s.awaitingTaskEpoch = s.awaitingLeaseToken = s.awaitingConnectionEpoch = 0;
    s.awaitingIdentity = s.awaitingGeneration = 0;
}
void clear_captures() noexcept {
    for (auto& p : state().panels) clear_panel(p);
    clear_awaiting();
}
void end_map(const char* reason, int nextMapPanel = -1, unsigned long long now = 0,
             const PauseContext& pause = {}) noexcept {
    auto& s = state();
    if (s.mapPanel >= 0) {
        Panel& p = s.panels[s.mapPanel];
        const auto pos = s.settings.nativeIdentity ? identity::sample(static_cast<unsigned>(s.mapPanel))
                                                   : identity::Sample{true, 0};
        bool keptAll = s.plan.valid && s.plan.confidence == MarkerCycleConfidence::Complete &&
                       s.plan.count > 0 && p.tracker.generation() == s.plan.generation;
        if (keptAll) {
            for (unsigned i = 0; i < s.plan.count; ++i)
                keptAll = keptAll && p.probe.kept(s.plan.slots[i]);
        }
        const bool identityOk = !s.settings.nativeIdentity ||
                                (p.identityAvailable && pos.available &&
                                 p.lastIdentity == s.planIdentity &&
                                 p.currentIdentity == s.planIdentity &&
                                 pos.epoch == s.planIdentity);
        const bool pauseOk = pause.supplied && s.openPauseSupplied && pause.voluntaryClose &&
                             s.openLeaseToken != 0 && pause.leaseToken == s.openLeaseToken &&
                             pause.taskEpoch == s.openTaskEpoch &&
                             pause.connectionEpoch == s.openConnectionEpoch &&
                             (pause.releasePending || (pause.releaseConfirmed && !pause.paused));
        const bool retain = nextMapPanel < 0 && std::strcmp(reason, "map-closed") == 0 &&
                            s.replaying && keptAll && identityOk && pauseOk;
        const bool awaitingRelease = retain && (pause.releasePending || pause.paused || !pause.releaseConfirmed);
        char line[220]{};
        std::snprintf(line, sizeof(line),
                      "marker,map-closed,panel=%d,replayed=%u,flips=%u,reason=%s,retained=%u,awaitingRelease=%u",
                      s.mapPanel, s.replaying ? 1u : 0u, s.flips, reason,
                      retain ? 1u : 0u, awaitingRelease ? 1u : 0u);
        emit(line);
        if (retain) {
            clear_panel(s.panels[1 - s.mapPanel]);
            p.probe.release_cycle();
            p.probe.discard_pending();
            if (awaitingRelease) {
                s.awaitingUnpause = true;
                s.awaitingPanel = s.mapPanel;
                s.pauseStartMs = s.mapOpenedMs;
                s.mapClosedMs = now;
                s.awaitingTaskEpoch = s.openTaskEpoch;
                s.awaitingLeaseToken = s.openLeaseToken;
                s.awaitingConnectionEpoch = s.openConnectionEpoch;
                s.awaitingIdentity = s.planIdentity;
                s.awaitingGeneration = s.plan.generation;
            } else {
                clear_awaiting();
                const auto pausedMs = now >= s.mapOpenedMs ? now - s.mapOpenedMs : 0;
                p.tracker.exclude_pause(s.mapOpenedMs, pausedMs);
                std::snprintf(line, sizeof(line),
                              "marker,pause-release-confirmed,panel=%d,generation=%llu,pausedMs=%llu,positionEpoch=%llu",
                              s.mapPanel, static_cast<unsigned long long>(s.plan.generation),
                              pausedMs, s.planIdentity);
                emit(line);
            }
        } else {
            // Non-voluntary close, unvalidated close, or lost confidence
            // reacquires a cycle. Readbacks from the pause are discarded.
            clear_captures();
        }
    }
    s.mapPanel = -1; s.planned = s.replaying = false; s.settle = s.flips = 0;
    s.openPauseSupplied = false; s.openTaskEpoch = s.openLeaseToken = s.openConnectionEpoch = 0;
}
void reset_all() noexcept {
    for (auto& p : state().panels) { p.probe.reset(); p.tracker.reset(); p.identityAvailable = p.cycleReady = false; p.lastIdentity = p.currentIdentity = 0; }
    clear_awaiting();
    state().following = false;
}
bool same_rect(const gfx::SourceRect& a, const gfx::SourceRect& b) noexcept {
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}
void dump(unsigned panel, const MarkerCycleTracker& tracker, unsigned slot) noexcept {
    auto& s = state();
    const auto* image = tracker.image(slot);
    if (!image || !s.settings.trace || s.directory.empty() || s.dumps >= MaxDumps) return;
    const auto folder = s.directory + L"\\marker";
    CreateDirectoryW(folder.c_str(), nullptr);
    wchar_t name[96]{}; std::swprintf(name, 96, L"\\p%u-%03u-state%u.ppm", panel, s.dumps, slot);
    HANDLE f = CreateFileW((folder + name).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    ++s.dumps;
    char header[32]{}; const int n = std::snprintf(header, sizeof(header), "P6\n%u %u\n255\n", NativeWidth, NativeHeight);
    std::vector<unsigned char> rgb(static_cast<std::size_t>(NativeWidth) * NativeHeight * 3);
    for (std::size_t i = 0; i < static_cast<std::size_t>(NativeWidth) * NativeHeight; ++i) {
        rgb[i*3] = static_cast<unsigned char>(image[i] >> 16);
        rgb[i*3+1] = static_cast<unsigned char>(image[i] >> 8);
        rgb[i*3+2] = static_cast<unsigned char>(image[i]);
    }
    DWORD written = 0;
    WriteFile(f, header, static_cast<DWORD>(n), &written, nullptr);
    WriteFile(f, rgb.data(), static_cast<DWORD>(rgb.size()), &written, nullptr);
    CloseHandle(f);
}
void trace(unsigned index, Panel& p, MarkerCycleTracker::Event event, unsigned long long t) noexcept {
    const auto window = t / 1000;
    if (window != p.window) { p.window = window; p.linesInWindow = 0; }
    if (p.linesInWindow >= TraceLinesPerSecond) { ++p.dropped; return; }
    ++p.linesInWindow;
    const auto d = p.tracker.difference();
    char line[300]{};
    std::snprintf(line, sizeof(line), "marker,trace,panel=%u,event=%s,t=%llu,generation=%llu,slot=%u,images=%u,runs=%u,changed=%u,box=%u-%u-%u-%u,dropped=%llu",
        index, event == MarkerCycleTracker::Event::Reset ? "reset" : event == MarkerCycleTracker::Event::Revisited ? "revisit" : "captured",
        t, static_cast<unsigned long long>(p.tracker.generation()), p.tracker.latest(), p.tracker.imageCount(),
        p.tracker.completedRuns(), d.changed, d.left, d.top, d.right, d.bottom, p.dropped);
    emit(line);
}
void observe_readiness(unsigned index, Panel& p, unsigned long long time) noexcept {
    const auto plan = p.tracker.plan(time);
    bool ready = plan.valid && plan.confidence == MarkerCycleConfidence::Complete;
    if (ready) for (unsigned i = 0; i < plan.count; ++i) ready = ready && p.probe.kept(plan.slots[i]);
    if (ready == p.cycleReady) return;
    p.cycleReady = ready;
    char line[260]{};
    std::snprintf(line, sizeof(line), "marker,cycle-%s,panel=%u,count=%u,periodTicks=%u,generation=%llu,positionEpoch=%llu,t=%llu,reason=%s",
        ready ? "ready" : "invalidated", index, plan.count, plan.periodTicks,
        static_cast<unsigned long long>(plan.generation), p.lastIdentity, time, plan.reason);
    emit(line);
}
void stop_replay(const char* reason) noexcept {
    auto& s = state();
    if (s.replaying) { char line[160]{}; std::snprintf(line, sizeof(line), "marker,replay-stopped,reason=%s", reason); emit(line); }
    s.replaying = false;
    if (s.mapPanel >= 0) s.panels[s.mapPanel].probe.release_cycle();
}
void plan_now(Panel& p, ID3D11DeviceContext* context) noexcept {
    auto& s = state();
    s.plan = plan_marker_cycle(p.tracker, s.mapOpenedMs);
    s.planned = true;
    // A partial ordered sequence is evidence, not an authentic loop. Only two
    // matching whole repetitions qualify for replay at the current position.
    if (s.plan.confidence == MarkerCycleConfidence::Partial) {
        s.plan.valid = false; s.plan.reason = "partial-cycle-frozen";
    }
    const auto* current = p.tracker.image(s.plan.frozenSlot);
    if (s.settings.nativeIdentity && (!p.identityAvailable || p.lastIdentity != p.currentIdentity)) {
        s.plan.valid = false; s.plan.reason = "position-identity-changed";
    }
    if (s.plan.valid && (!current || !std::equal(s.frozen.begin(), s.frozen.end(), current))) {
        s.plan.valid = false; s.plan.reason = "frozen-frame-not-in-sequence";
    }
    const bool images = s.plan.valid && p.probe.prepare_cycle(context, s.plan.slots.data(), s.plan.count, s.plan.diff);
    s.replaying = s.settings.replay && s.plan.valid && images;
    s.planIdentity = p.currentIdentity;
    s.shownSlot = s.plan.frozenSlot;
    char line[520]{};
    std::snprintf(line, sizeof(line), "marker,plan,panel=%d,valid=%u,reason=%s,images=%u,replay=%u,confidence=%s,count=%u,periodTicks=%u,generation=%llu,frozen=%u,changed=%u,box=%u-%u-%u-%u",
        s.mapPanel, s.plan.valid ? 1u : 0u, s.plan.reason, images ? 1u : 0u, s.replaying ? 1u : 0u,
        s.plan.confidence == MarkerCycleConfidence::Complete ? "complete" : s.plan.confidence == MarkerCycleConfidence::Partial ? "partial" : "frozen",
        s.plan.count, s.plan.periodTicks, static_cast<unsigned long long>(s.plan.generation), s.plan.frozenSlot,
        s.plan.diff.changed, s.plan.diff.left, s.plan.diff.top, s.plan.diff.right, s.plan.diff.bottom);
    emit(line);
    if (s.settings.trace && s.plan.valid) {
        for (unsigned i = 0; i < s.plan.count; ++i) {
            std::snprintf(line, sizeof(line), "marker,phase,index=%u,slot=%u,holdTicks=%u", i, s.plan.slots[i], s.plan.holdTicks[i]);
            emit(line); dump(static_cast<unsigned>(s.mapPanel), p.tracker, s.plan.slots[i]);
        }
    }
}
} // namespace
void configure(const Settings& settings, const wchar_t* directory, Log log) noexcept {
    try {
        auto& s = state(); end_map("configured"); reset_all();
        s.settings = settings; s.directory = directory ? directory : L""; s.log = log; s.configured = true;
        char line[120]{};
        std::snprintf(line, sizeof(line), "marker,configured,trace=%u,replay=%u,tracker=multi-state,maxStates=%u,positionGuard=%u",
                      settings.trace ? 1u : 0u, settings.replay ? 1u : 0u, MaxMarkerCycleStates, settings.nativeIdentity ? 1u : 0u);
        emit(line);
    } catch (...) {}
}
bool active() noexcept {
    const auto& s = state(); return s.configured && (s.settings.trace || s.settings.replay);
}
Patch on_present(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* backbuffer,
                 const gfx::SourceRect (&panels)[2], int mapPanel, bool tracking,
                 const PauseContext& pause) noexcept {
    try {
        auto& s = state(); Patch out{};
        if (!active() || !device || !context || !backbuffer) {
            if (s.following) { end_map("surface-unavailable"); reset_all(); }
            return out;
        }
        D3D11_TEXTURE2D_DESC desc{}; backbuffer->GetDesc(&desc);
        const bool changed = s.device != device || desc.Format != s.format || desc.Width != s.width || desc.Height != s.height ||
                             !same_rect(panels[0], s.regions[0]) || !same_rect(panels[1], s.regions[1]);
        if (!tracking || changed) {
            if (s.following) { end_map(tracking ? "resources-changed" : "tracking-off"); reset_all(); }
            s.device = device; s.format = desc.Format; s.width = desc.Width; s.height = desc.Height;
            s.regions[0] = panels[0]; s.regions[1] = panels[1];
            if (!tracking) return out;
        }
        s.following = true;
        const auto now = now_ms();
        if (mapPanel != s.mapPanel) {
            if (s.awaitingUnpause) {
                char line[180]{};
                std::snprintf(line, sizeof(line),
                              "marker,retention-dropped,panel=%d,generation=%llu,reason=reopened-before-release",
                              s.awaitingPanel, s.awaitingGeneration);
                emit(line);
                clear_captures();
            }
            end_map(mapPanel < 0 ? "map-closed" : "panel-changed", mapPanel, now, pause);
            if (mapPanel == 0 || mapPanel == 1) {
                s.mapPanel = mapPanel;
                s.mapOpenedMs = now;
                s.openPauseSupplied = pause.supplied;
                s.openTaskEpoch = pause.taskEpoch;
                s.openLeaseToken = pause.leaseToken;
                s.openConnectionEpoch = pause.connectionEpoch;
            }
        }
        if (s.mapPanel >= 0 && pause.supplied) {
            const bool ownershipValid = pause.paused && pause.pauseOwned && s.openPauseSupplied &&
                                        s.openLeaseToken != 0 && pause.leaseToken == s.openLeaseToken &&
                                        pause.taskEpoch == s.openTaskEpoch &&
                                        pause.connectionEpoch == s.openConnectionEpoch;
            if (!ownershipValid) {
                stop_replay("pause-ownership-lost");
                if (!s.planned) { s.planned = true; emit("marker,plan,valid=0,reason=pause-ownership-lost"); }
            }
        }
        if (s.awaitingUnpause) {
            const int ap = s.awaitingPanel;
            const auto pos = (ap >= 0 && ap < 2 && s.settings.nativeIdentity)
                                 ? identity::sample(static_cast<unsigned>(ap))
                                 : identity::Sample{true, 0};
            const char* dropReason = nullptr;
            if (ap < 0 || ap > 1 || s.mapPanel >= 0) dropReason = "invalid-awaiting-state";
            else if (!pause.supplied || !pause.voluntaryClose) dropReason = "voluntary-close-cancelled";
            else if (pause.taskEpoch != s.awaitingTaskEpoch) dropReason = "task-epoch-changed";
            else if (pause.leaseToken != s.awaitingLeaseToken || !s.awaitingLeaseToken) dropReason = "lease-token-changed";
            else if (pause.connectionEpoch != s.awaitingConnectionEpoch) dropReason = "connection-epoch-changed";
            else if (s.settings.nativeIdentity && (!pos.available || pos.epoch != s.awaitingIdentity)) dropReason = "position-identity-changed";
            else if (s.panels[ap].tracker.generation() != s.awaitingGeneration) dropReason = "capture-generation-changed";
            else if (now < s.mapClosedMs || now - s.mapClosedMs > SettleBudgetMs) dropReason = "pause-release-timeout";
            if (dropReason) {
                char line[180]{};
                std::snprintf(line, sizeof(line),
                              "marker,retention-dropped,panel=%d,generation=%llu,reason=%s",
                              ap, s.awaitingGeneration, dropReason);
                emit(line);
                if (ap >= 0 && ap < 2) {
                    s.panels[ap].tracker.reset();
                    observe_readiness(static_cast<unsigned>(ap), s.panels[ap], now);
                }
                clear_captures();
                if (pause.supplied && (pause.paused || pause.releasePending)) return out;
            } else if (pause.releasePending || pause.paused || !pause.releaseConfirmed) {
                // Emulation is still paused while the release command is in
                // flight: do not capture or collect paused frames.
                s.panels[ap].probe.discard_pending();
                return out;
            } else {
                auto& p = s.panels[ap];
                p.probe.discard_pending();
                const auto pausedMs = now >= s.pauseStartMs ? now - s.pauseStartMs : 0;
                p.tracker.exclude_pause(s.pauseStartMs, pausedMs);
                char line[200]{};
                std::snprintf(line, sizeof(line),
                              "marker,pause-release-confirmed,panel=%d,generation=%llu,pausedMs=%llu,positionEpoch=%llu",
                              ap, s.awaitingGeneration, pausedMs, s.awaitingIdentity);
                emit(line);
                clear_awaiting();
            }
        } else if (s.mapPanel < 0 && pause.supplied && (pause.paused || pause.releasePending)) {
            return out;
        }
        for (unsigned i = 0; i < 2; ++i) {
            auto& p = s.panels[i];
            const auto position = s.settings.nativeIdentity ? identity::sample(i) : identity::Sample{true, 0};
            p.currentIdentity = position.epoch;
            if (!position.available) {
                if (p.identityAvailable) { p.tracker.reset(); p.probe.invalidate_kept(); p.probe.discard_pending(); observe_readiness(i, p, now); }
                p.identityAvailable = false;
                if (static_cast<int>(i) == s.mapPanel) {
                    stop_replay("position-identity-unavailable");
                    if (!s.planned) { s.planned = true; emit("marker,plan,valid=0,reason=position-identity-unavailable"); }
                }
                continue;
            }
            p.identityAvailable = true;
            if (s.replaying && static_cast<int>(i) == s.mapPanel && p.currentIdentity != s.planIdentity)
                stop_replay("position-identity-changed");
            p.probe.capture(device, context, backbuffer, panels[i], now, position.epoch);
            unsigned long long t = 0, capturedIdentity = 0;
            while (p.probe.collect(context, p.image.data(), t, &capturedIdentity)) {
                if (s.mapPanel >= 0 && t >= s.mapOpenedMs) {
                    // Post-open captures verify a frozen source; they never
                    // train timings or create a cycle while the game is paused.
                    if (static_cast<int>(i) == s.mapPanel) {
                        if (s.planned) {
                            if (p.image != s.frozen) stop_replay("panel-changed-under-pause");
                        } else {
                            if (!s.settle || p.image != s.frozen) { s.frozen = p.image; s.settle = 1; }
                            else ++s.settle;
                        }
                    }
                    continue;
                }
                const auto generation = p.tracker.generation();
                const auto event = p.tracker.observe(p.image.data(), t, capturedIdentity);
                p.lastIdentity = capturedIdentity;
                if (generation != p.tracker.generation()) p.probe.invalidate_kept();
                if (event == MarkerCycleTracker::Event::Captured || event == MarkerCycleTracker::Event::Reset)
                    p.probe.keep(context, p.tracker.latest());
                observe_readiness(i, p, t);
                if (event != MarkerCycleTracker::Event::Same && s.settings.trace) trace(i, p, event, t);
            }
        }
        if (s.mapPanel < 0) return out;
        auto& p = s.panels[s.mapPanel];
        if (!s.planned) {
            if (s.settle >= SettleFrames) plan_now(p, context);
            else if (now - s.mapOpenedMs > SettleBudgetMs) {
                s.planned = true; emit("marker,plan,valid=0,reason=panel-not-settling");
            }
        }
        if (!s.replaying) return out;
        if (p.tracker.generation() != s.plan.generation) { stop_replay("capture-generation-changed"); return out; }
        const auto frame = marker_cycle_frame(s.plan, now);
        if (frame.slot != s.shownSlot) { s.shownSlot = frame.slot; ++s.flips; }
        if (frame.slot != s.plan.frozenSlot) out.texture = p.probe.cycle_image(frame.slot, out.left, out.top);
        return out;
    } catch (...) { return {}; }
}
}
