#include "dominus/live_controller.hpp"
#include "dominus/core.hpp"
#include "dominus/native_runtime.hpp"
#include "dominus/input_observer.hpp"
#include "dominus/title_identity.hpp"
#include <mutex>
#include <string>
#include <cstdio>

namespace dominus::live {
namespace {
// Exact Portrait preset observed at 1920x1080 with the collection layout that
// shows gameplay (left, 1280x960) plus map and Status in the two right-hand
// 640x480 panels. Their order differed between sessions (user screenshots,
// 23/10/2026; native Select suspected), so the map panel is chosen from
// pixels each time View opens it.
constexpr gfx::SourceRect GameplayRect{0.f,1.f/18.f,2.f/3.f,8.f/9.f};
constexpr gfx::SourceRect UpperRightRect{2.f/3.f,1.f/18.f,1.f/3.f,4.f/9.f},LowerRightRect{2.f/3.f,0.5f,1.f/3.f,4.f/9.f};
// One frame normally suffices; past this the default panel is used.
constexpr ULONGLONG ChooseBudgetMs=500;
// A View handed back to the game (mod off, or map not allowed) is held this
// long: a single-sample pulse was not registered as Select in the real game.
constexpr ULONGLONG ReplayHoldMs=80;
struct State {
    std::mutex mutex;
    bool ready=false,enabled=false,focused=false,map=false,disconnected=false;
    ULONGLONG replayUntil=0;
    bool nativeReady=false,mapAllowed=false;
    // Map panel selection pending before the pause request.
    bool choosing=false,panelsWanted=false,panelsReady=false;
    MapPanel choice=MapPanel::Unknown;
    double upperBrown=0,lowerBrown=0;
    ULONGLONG chooseDeadline=0;
    gfx::SourceRect mapRect=UpperRightRect;
    unsigned controller=UINT32_MAX;
    std::uint64_t connection=0,epoch=0,revision=0,token=0,leaseEpoch=0,leaseRevision=0;
    std::uint64_t request=0,awaiting=0,lastCommand=0;
    bool voluntaryClose=false,releaseConfirmed=false;
    std::uint64_t releasedToken=0,releasedEpoch=0;
    marker::PauseContext pauseContext{};
    native::PauseCommandKind waitingKind=native::PauseCommandKind::Acquire;
    DWORD lastPoll=0;
    InputAction action=InputAction::None;
    InputRecognizer recognizer;
    // Portrait features run only while title_identity verifies Portrait; the
    // filter also stays out of pauses the mod does not own (collection menus).
    PortraitGate gate;
    bool identityActive=false,inputActive=false;
    // Portrait's own scene: View is the map command only in gameplay, and the
    // map opens only while the sub-screen shows it (classify_portrait_scene).
    bool gameplay=false,mapReady=false;
    SceneVerdict scene{};
    PortraitStateFields sceneFields{};
    bool sceneAvailable=false;
    std::uint64_t titleGeneration=0;
    std::wstring directory;
    Log log=nullptr;
};
State& state(){static auto* s=new State;return *s;}
void log(const char* name) noexcept {if(state().log)state().log(name);}
void clear_release_tracking(State& s) noexcept {
    s.voluntaryClose=s.releaseConfirmed=false;
    s.releasedToken=s.releasedEpoch=0;
}
void log_scene(State& s,const char* what) noexcept {
    char line[200]{};const auto& f=s.sceneFields;
    std::snprintf(line,sizeof(line),"%s,scene=%s,available=%u,mode=0x%x,state=%u,type=%u,page=%u/%u,gameplay=%u,mapReady=%u",what,game_scene_name(s.scene.scene),unsigned(s.sceneAvailable),unsigned(f.mode),unsigned(f.gameState),unsigned(f.gameType),unsigned(f.page),unsigned(f.requestedPage),unsigned(s.gameplay),unsigned(s.mapReady));log(line);
}
void update_gate(State& s,const TitleObservation& title,const title::PortraitSceneSample& sample,const native::Status& st) noexcept {
    const auto scene=classify_portrait_scene(sample.fields,sample.available);
    const auto gate=s.gate.update({title,st.pauseKnown,st.paused,s.token!=0||s.awaiting!=0||st.stalePause,scene});
    const bool sceneChanged=scene.scene!=s.scene.scene||gate.gameplay!=s.gameplay||gate.mapReady!=s.mapReady||sample.available!=s.sceneAvailable||
                            sample.fields.mode!=s.sceneFields.mode||sample.fields.page!=s.sceneFields.page||sample.fields.requestedPage!=s.sceneFields.requestedPage;
    s.scene=scene;s.sceneFields=sample.fields;s.sceneAvailable=sample.available;s.gameplay=gate.gameplay;s.mapReady=gate.mapReady;
    if(sceneChanged&&gate.identity)log_scene(s,"live,scene");
    if(title.generation!=s.titleGeneration){
        s.titleGeneration=title.generation;char line[220]{};
        std::snprintf(line,sizeof(line),"title,portrait=%u,reason=%s,index=%d,ready=%u,module=0x%llx,epoch=%llu",unsigned(title.portrait),title_reason_name(title.reason),title.fields.index,unsigned(title.fields.ready),static_cast<unsigned long long>(title.fields.module),static_cast<unsigned long long>(title.taskEpoch));log(line);
    }
    if(gate.identityChanged)log(gate.identity?"live,title-gate,portrait-verified":"live,title-gate,portrait-lost");
    // A gesture never spans a context change; the next activation starts from
    // neutral (a View still held then is drained, never acted on).
    if(gate.inputChanged){s.recognizer.reset();log(gate.input?"live,input-gate,active":"live,input-gate,passthrough");}
    if(!gate.identity)s.replayUntil=0;
    s.identityActive=gate.identity;s.inputActive=gate.input;
}
std::uint16_t filter(const input::Snapshot& raw,bool fromGame) noexcept {
    auto& s=state();std::lock_guard<std::mutex> lock(s.mutex);
    if(!s.ready)return raw.buttons;
    // Select a controller on the first real sample; own polling of disconnected
    // slots must not cancel a gesture belonging to the chosen connected pad.
    if(s.controller==UINT32_MAX&&raw.connected){s.controller=raw.controller;s.disconnected=false;++s.connection;s.recognizer.reset();}
    if(raw.controller!=s.controller)return raw.buttons;
    if(!raw.connected){
        s.controller=UINT32_MAX;s.disconnected=true;++s.connection;s.replayUntil=0;
        clear_release_tracking(s);
        // Later in this same frame the collection opens its menu for the lost
        // pad and only unpauses on close what it found unpaused: drop our map
        // pause first, on this (pause) thread, or the game stays frozen.
        if(s.token&&native::release_owned_now()){s.token=0;s.map=false;log("live,controller-lost,map-pause-released-first");}
    }
    // Outside verified Portrait gameplay every button passes exactly as read:
    // no View/Y reservation, delay or chord. Only a View pulse that Portrait
    // already started may finish, and only while Portrait is still verified.
    if(!s.inputActive){
        if(s.replayUntil&&fromGame&&s.identityActive){if(GetTickCount64()<s.replayUntil)return static_cast<std::uint16_t>(raw.buttons|ButtonView);s.replayUntil=0;}
        return raw.buttons;
    }
    InputSnapshot sample{raw.buttons,raw.connected,s.focused,s.connection};
    InputPolicy policy{true,s.enabled,s.mapAllowed,s.epoch};
    auto result=s.recognizer.update(sample,policy);
    if(result.action==InputAction::ToggleMod||s.action==InputAction::None)s.action=result.action;
    const ULONGLONG now=GetTickCount64();
    if(result.delayedViewPulse)s.replayUntil=now+ReplayHoldMs;
    // A new View press belongs to a new gesture: never extend the old pulse into it.
    else if(raw.buttons&ButtonView)s.replayUntil=0;
    auto forwarded=result.forwardedButtons;
    if(s.replayUntil&&fromGame){if(now<s.replayUntil)forwarded|=ButtonView;else s.replayUntil=0;}
    return forwarded;
}
bool queue(native::PauseCommandKind kind) noexcept {
    auto& s=state();native::PauseRequest request{};request.kind=kind;request.requestId=++s.request;
    request.expectedTaskEpoch=kind==native::PauseCommandKind::Release?s.leaseEpoch:s.epoch;
    request.expectedRevision=kind==native::PauseCommandKind::Release?s.leaseRevision:s.revision;
    request.leaseToken=s.token;
    if(!native::queue_pause_request(request)){log("live,pause-queue-rejected");return false;}
    s.awaiting=request.requestId;s.waitingKind=kind;return true;
}
void close(bool voluntary=false) noexcept {
    auto& s=state();
    const bool hadOwnedMap=s.map&&s.token!=0&&!s.awaiting;
    s.choosing=s.panelsWanted=s.panelsReady=false;s.map=false;
    if(s.token&&!s.awaiting){
        if(queue(native::PauseCommandKind::Release)){
            if(voluntary&&hadOwnedMap){
                s.voluntaryClose=true;s.releaseConfirmed=false;
                s.releasedToken=s.token;s.releasedEpoch=s.leaseEpoch;
                return;
            }
        }
    }
    if(!voluntary||!hadOwnedMap)clear_release_tracking(s);
}
// Picks the map panel once the samples arrive (or the budget expires) and
// then requests the pause. Unknown keeps the game's default upper panel.
void finish_choice(ULONGLONG now) noexcept {
    auto& s=state();
    if(!s.panelsReady&&now<=s.chooseDeadline)return;
    const char* reason=s.panelsReady?(s.choice==MapPanel::Unknown?"undecided-default-upper":"pixels"):"timeout-default-upper";
    s.mapRect=s.panelsReady&&s.choice==MapPanel::Second?LowerRightRect:UpperRightRect;
    char line[200]{};std::snprintf(line,sizeof(line),"live,map-panel,%s,reason=%s,upperBrown=%.3f,lowerBrown=%.3f",s.mapRect.y>0.25f?"lower":"upper",reason,s.upperBrown,s.lowerBrown);log(line);
    s.choosing=s.panelsWanted=s.panelsReady=false;
    if(!s.awaiting&&!s.token)queue(native::PauseCommandKind::Acquire);
}
}
bool panels_wanted(gfx::SourceRect& first,gfx::SourceRect& second) noexcept {
    auto& s=state();std::lock_guard<std::mutex> lock(s.mutex);
    if(!s.panelsWanted)return false;
    s.panelsWanted=false;first=UpperRightRect;second=LowerRightRect;return true;
}
void panel_rects(gfx::SourceRect& upper,gfx::SourceRect& lower) noexcept {upper=UpperRightRect;lower=LowerRightRect;}
void panels_sampled(const unsigned* first,const unsigned* second,unsigned count) noexcept {
    auto& s=state();std::lock_guard<std::mutex> lock(s.mutex);
    if(!s.choosing)return;
    const bool valid=first&&second&&count==PanelColumns*PanelRows;
    s.upperBrown=valid?lower_brown_fraction(first,PanelColumns,PanelRows):-1;
    s.lowerBrown=valid?lower_brown_fraction(second,PanelColumns,PanelRows):-1;
    s.choice=valid?choose_map_panel(first,second,PanelColumns,PanelRows):MapPanel::Unknown;
    s.panelsReady=true;
}
void initialize(const wchar_t* directory,bool enabled,Log output) noexcept {
    try {auto& s=state();std::lock_guard<std::mutex> lock(s.mutex);if(s.ready)return;
        s.directory=directory?directory:L"";s.enabled=enabled;s.log=output;s.ready=true;
        input::set_filter(filter);log("live,experimental-portrait-controls-ready");
    } catch(...) {}
}
gfx::Request frame(HWND window) noexcept {
    try {
        // Poll outside our state lock: the adapter invokes filter synchronously.
        input::on_present();auto st=native::status_snapshot();auto reply=native::pause_reply_snapshot();
        // Outside our lock: the first sight of a loaded dra02.dll hashes it once.
        const auto title=title::observe(st.taskObserved?st.task:0,st.taskEpoch);
        const auto sceneSample=title::portrait_scene(title);
        auto& s=state();std::lock_guard<std::mutex> lock(s.mutex);if(!s.ready)return {};
        const auto foreground=GetForegroundWindow();s.focused=foreground==window||(foreground&&IsChild(window,foreground));
        s.epoch=st.taskEpoch;s.revision=st.pauseRevision;
        s.nativeReady=st.hooksInstalled&&st.pauseKnown&&st.pauseCommandsEnabled&&st.lifecycleHooksInstalled;
        if(s.awaiting&&reply.requestId==s.awaiting){
            char line[160]{};std::snprintf(line,sizeof(line),"live,pause-reply,request=%llu,code=%u,token=%llu",static_cast<unsigned long long>(reply.requestId),unsigned(reply.code),static_cast<unsigned long long>(reply.leaseToken));log(line);
            if(s.waitingKind==native::PauseCommandKind::Acquire){
                clear_release_tracking(s);
                if(reply.code==native::PauseReplyCode::Acquired){s.token=reply.leaseToken;s.leaseEpoch=reply.taskEpoch;s.leaseRevision=reply.revision;s.map=true;}
                else s.map=false;
            } else if(reply.code==native::PauseReplyCode::Released||reply.code==native::PauseReplyCode::Superseded||reply.code==native::PauseReplyCode::Rejected){
                if(s.voluntaryClose&&s.waitingKind==native::PauseCommandKind::Release&&
                   reply.code==native::PauseReplyCode::Released&&reply.taskEpoch==s.releasedEpoch&&
                   st.taskEpoch==s.releasedEpoch&&st.pauseKnown&&!st.paused){
                    s.releaseConfirmed=true;
                }else{
                    clear_release_tracking(s);
                }
                s.token=0;s.map=false;
            }
            s.awaiting=0;
        }
        if(s.token&&st.ownedPauseToken!=s.token){s.token=0;s.map=false;clear_release_tracking(s);log("live,pause-ownership-lost");}
        update_gate(s,title,sceneSample,st);
        const auto now=GetTickCount();bool releaseStale=false;
        if(now-s.lastPoll>=120&&!s.directory.empty()){
            s.lastPoll=now;const auto path=s.directory+L"\\control.ini";
            const auto sequence=GetPrivateProfileIntW(L"control",L"Sequence",0,path.c_str());
            if(sequence&&sequence!=s.lastCommand){s.lastCommand=sequence;const auto command=GetPrivateProfileIntW(L"control",L"Action",0,path.c_str());
                if(command==1)s.action=InputAction::ToggleMap;else if(command==2)s.action=InputAction::ToggleMod;else if(command==3)releaseStale=true;
            }
        }
        // Nothing requested while (or before) Portrait was verified applies to
        // another title; an owned map pause is released below by close().
        if(!s.identityActive){s.action=InputAction::None;releaseStale=false;}
        // Recovery for a pause the mod created that nothing owns any more (the
        // collection's menu stacked on it and closed without unpausing): an
        // explicit View, or control.ini Action=3, releases exactly that pause.
        // The DS stays frozen in the gameplay the map was opened from, so View
        // is still the mod's command there.
        if(st.stalePause&&!s.token&&!s.awaiting&&(releaseStale||(s.action==InputAction::ToggleMap&&s.enabled&&s.focused&&s.nativeReady))){
            s.action=InputAction::None;
            clear_release_tracking(s);
            if(queue(native::PauseCommandKind::ReleaseStale))log("live,stale-mod-pause-release-requested");
        }
        // A map that stopped being valid (its pause request raced a scene
        // change) closes through the lease checks; frozen under our pause, a
        // valid one stays valid.
        if(!s.identityActive||!s.focused||s.disconnected||!s.nativeReady||!s.enabled||(st.paused&&!s.token)||((s.token||s.choosing)&&!s.mapReady))close(false);
        if(s.action==InputAction::ToggleMod){s.enabled=!s.enabled;close(false);log(s.enabled?"live,mod-enabled":"live,mod-disabled");}
        else if(s.action==InputAction::ToggleMap&&s.enabled&&s.focused&&s.nativeReady&&!s.awaiting){
            switch(map_command({s.map&&s.token!=0,s.token!=0,s.choosing,st.paused,s.mapReady})){
            case MapCommand::CloseVoluntary:close(true);break;
            case MapCommand::Cancel:close(false);break;
            case MapCommand::Open:clear_release_tracking(s);s.choosing=s.panelsWanted=true;s.panelsReady=false;s.chooseDeadline=GetTickCount64()+ChooseBudgetMs;break;
            // Gameplay without a map to show (fade, death, sub-screen switch): nothing.
            case MapCommand::Refuse:log_scene(s,"live,map-refused");break;
            case MapCommand::None:break;
            }
        }
        s.action=InputAction::None;
        if(s.choosing){if(st.paused)close(false);else finish_choice(GetTickCount64());}
        // View is the map command only in Portrait gameplay; elsewhere (title,
        // menus, sequences, unknown states) it stays the game's own Select. A
        // stale mod pause keeps View as a mod command so it can release it.
        s.mapAllowed=s.identityActive&&s.gameplay&&s.enabled&&s.focused&&s.nativeReady&&(!st.paused||s.token||st.stalePause);
        const bool waitingRelease=s.awaiting!=0&&s.waitingKind==native::PauseCommandKind::Release;
        if(s.releaseConfirmed&&(st.paused||!st.pauseKnown||st.taskEpoch!=s.releasedEpoch))clear_release_tracking(s);
        if(waitingRelease&&st.taskEpoch!=s.releasedEpoch)clear_release_tracking(s);
        s.pauseContext.supplied=true;
        s.pauseContext.paused=st.paused;
        s.pauseContext.pauseOwned=s.token!=0&&st.ownedPauseToken==s.token&&st.taskEpoch==s.leaseEpoch;
        s.pauseContext.voluntaryClose=s.voluntaryClose;
        s.pauseContext.releasePending=waitingRelease&&s.voluntaryClose;
        s.pauseContext.releaseConfirmed=s.releaseConfirmed&&!st.paused&&st.pauseKnown&&st.taskEpoch==s.releasedEpoch;
        s.pauseContext.taskEpoch=st.taskEpoch;
        s.pauseContext.leaseToken=s.token?s.token:s.releasedToken;
        s.pauseContext.connectionEpoch=s.connection;
        if(!s.identityActive||!s.enabled||!s.focused||!s.nativeReady||(st.paused&&!s.token))return {};
        // Exact preset observed in the current real Portrait session. The
        // original compositor still renders every screen; no game setting changes.
        if(s.map)return {gfx::Mode::Map,s.mapRect,false};
        return {gfx::Mode::Gameplay,GameplayRect,false};
    } catch(...) {return {};}
}
marker::PauseContext pause_context() noexcept {
    auto& s=state();std::lock_guard<std::mutex> lock(s.mutex);return s.pauseContext;
}
}
