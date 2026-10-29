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
// The collection layout (gameplay left, map and Status in the two right-hand
// panels) comes from the verified title's AdaptationProfile. Portrait keeps
// the exact v0.1.1 preset observed at 1920x1080; its panel order differed
// between sessions (native Select), so its map panel is chosen from pixels
// each time View opens it. Dawn reads it from its native page.
gfx::SourceRect rect(const NormalizedRect& r) noexcept {return {r.x,r.y,r.width,r.height};}
const AdaptationProfile& layout_of(const AdaptationProfile* profile) noexcept {
    // Without a verified title the marker probe still needs two stable regions.
    return profile?*profile:*adaptation_profile(Title::PortraitOfRuin);
}
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
    // Map panel selection pending before the pause request (Portrait).
    bool choosing=false,panelsWanted=false,panelsReady=false;
    MapPanel choice=MapPanel::Unknown;
    double upperBrown=0,lowerBrown=0;
    ULONGLONG chooseDeadline=0;
    gfx::SourceRect mapRect=rect(layout_of(nullptr).upperPanel);
    // The panel holding the map while the enlarged view is open (0 upper,
    // 1 lower) and the page shown there (0 the map, 1 the other page).
    int mapPanel=0;
    unsigned page=0,viewPages=1;
    int pageSteps=0;
    PageButtonFilter pages;
    ViewInputOwner viewInput;
    unsigned controller=UINT32_MAX;
    std::uint64_t connection=0,epoch=0,revision=0,token=0,leaseEpoch=0,leaseRevision=0;
    std::uint64_t request=0,awaiting=0,lastCommand=0;
    bool voluntaryClose=false,releaseConfirmed=false;
    std::uint64_t releasedToken=0,releasedEpoch=0;
    marker::PauseContext pauseContext{};
    MarkerTarget markerTarget{};
    PointerPolicy pointerPolicy{};
    // The last presentation frame() returned, for scenes that hold the layout.
    Presentation lastLayout=Presentation::Original;
    native::PauseCommandKind waitingKind=native::PauseCommandKind::Acquire;
    DWORD lastPoll=0;
    InputAction action=InputAction::None;
    InputRecognizer recognizer;
    // Title features run only while title_identity verifies a title with an
    // adaptation profile; the filter also stays out of pauses the mod does
    // not own (collection menus).
    TitleGate gate;
    Title title=Title::Unknown;
    const AdaptationProfile* profile=nullptr;
    bool identityActive=false,inputActive=false;
    // The title's own scene: View is the map command only in gameplay, and
    // the map opens only while the sub-screen shows it (classify_scene).
    bool gameplay=false,mapReady=false;
    SceneVerdict scene{};
    TitleSceneFields sceneFields{};
    bool sceneAvailable=false;
    std::uint64_t titleGeneration=0;
    std::wstring directory;
    Log log=nullptr;
};
State& state(){static auto* s=new State;return *s;}
void log(const char* name) noexcept {if(state().log)state().log(name);}
const char* title_name(Title title) noexcept {
    switch(title){case Title::PortraitOfRuin:return "portrait";case Title::DawnOfSorrow:return "dawn";case Title::OrderOfEcclesia:return "ecclesia";default:return "none";}
}
void clear_release_tracking(State& s) noexcept {
    s.voluntaryClose=s.releaseConfirmed=false;
    s.releasedToken=s.releasedEpoch=0;
}
// What the scene log reports; Dawn's fade counter is reduced to "fading"
// so a fade does not log every frame.
bool scene_key_changed(const TitleSceneFields& a,const TitleSceneFields& b) noexcept {
    if(a.title!=b.title)return true;
    if(a.title==Title::DawnOfSorrow){
        const auto& x=a.dawn;const auto& y=b.dawn;
        constexpr auto Flags=DawnPageLockFlag|DawnMenuBlockFlags;
        return x.mode!=y.mode||x.gameState!=y.gameState||x.subState!=y.subState||x.gameType!=y.gameType||x.page!=y.page||x.requestedPage!=y.requestedPage||
               x.pageOverlay!=y.pageOverlay||x.frozen!=y.frozen||(x.eventFlags&Flags)!=(y.eventFlags&Flags)||
               (x.fadeFrames!=0)!=(y.fadeFrames!=0)||(x.brightness!=0)!=(y.brightness!=0);
    }
    const auto& x=a.portrait;const auto& y=b.portrait;
    return x.mode!=y.mode||x.page!=y.page||x.requestedPage!=y.requestedPage;
}
void log_scene(State& s,const char* what) noexcept {
    char line[260]{};
    if(s.sceneFields.title==Title::DawnOfSorrow){
        const auto& f=s.sceneFields.dawn;
        std::snprintf(line,sizeof(line),"%s,title=dawn,scene=%s,available=%u,mode=0x%x,state=%u,step=%u,type=%u,page=%u/%u,overlay=%u,frozen=%u,flags=0x%x,fade=%d/%d,single=%u,hold=%u,gameplay=%u,mapReady=%u,mapPanel=%d",what,game_scene_name(s.scene.scene),unsigned(s.sceneAvailable),unsigned(f.mode),unsigned(f.gameState),unsigned(f.subState),unsigned(f.gameType),unsigned(f.page),unsigned(f.requestedPage),unsigned(f.pageOverlay),unsigned(f.frozen),f.eventFlags,f.fadeFrames,f.brightness,unsigned(s.scene.singleScreen),unsigned(s.scene.holdLayout),unsigned(s.gameplay),unsigned(s.mapReady),s.scene.mapPanel);
    }else{
        const auto& f=s.sceneFields.portrait;
        std::snprintf(line,sizeof(line),"%s,scene=%s,available=%u,mode=0x%x,state=%u,type=%u,page=%u/%u,gameplay=%u,mapReady=%u",what,game_scene_name(s.scene.scene),unsigned(s.sceneAvailable),unsigned(f.mode),unsigned(f.gameState),unsigned(f.gameType),unsigned(f.page),unsigned(f.requestedPage),unsigned(s.gameplay),unsigned(s.mapReady));
    }
    log(line);
}
void update_gate(State& s,const TitleObservation& title,const title::SceneSample& sample,const native::Status& st) noexcept {
    const auto scene=classify_scene(sample.fields,sample.available);
    const auto gate=s.gate.update({title,st.pauseKnown,st.paused,s.token!=0||s.awaiting!=0||st.stalePause,scene});
    const bool sceneChanged=scene.scene!=s.scene.scene||scene.singleScreen!=s.scene.singleScreen||scene.holdLayout!=s.scene.holdLayout||gate.gameplay!=s.gameplay||gate.mapReady!=s.mapReady||
                            sample.available!=s.sceneAvailable||scene_key_changed(sample.fields,s.sceneFields);
    s.scene=scene;s.sceneFields=sample.fields;s.sceneAvailable=sample.available;s.gameplay=gate.gameplay;s.mapReady=gate.mapReady;
    if(sceneChanged&&gate.identity)log_scene(s,"live,scene");
    if(title.generation!=s.titleGeneration){
        s.titleGeneration=title.generation;char line[240]{};
        std::snprintf(line,sizeof(line),"title,portrait=%u,reason=%s,index=%d,ready=%u,module=0x%llx,epoch=%llu,title=%s",unsigned(title.portrait),title_reason_name(title.reason),title.fields.index,unsigned(title.fields.ready),static_cast<unsigned long long>(title.fields.module),static_cast<unsigned long long>(title.taskEpoch),title_name(title.title));log(line);
    }
    if(gate.identityChanged){char line[80]{};std::snprintf(line,sizeof(line),"live,title-gate,%s-%s",title_name(gate.identity?gate.title:s.title),gate.identity?"verified":"lost");log(line);}
    // A gesture never spans a context change; the next activation starts from
    // neutral (a View still held then is drained, never acted on).
    if(gate.inputChanged){s.recognizer.reset();log(gate.input?"live,input-gate,active":"live,input-gate,passthrough");}
    if(!gate.identity)s.replayUntil=0;
    const auto* profile=gate.identity?adaptation_profile(gate.title):nullptr;
    // Page reservations survive a foreign pause (the input gate), not a title change.
    if(gate.title!=s.title||!profile){s.pages.reset();s.viewInput.reset();s.page=0;s.pageSteps=0;}
    s.title=gate.title;s.profile=profile;
    s.identityActive=gate.identity&&profile;s.inputActive=gate.input&&profile;
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
    // Outside verified gameplay every button passes exactly as read: no
    // View/Y reservation, delay or chord. Only a View pulse that the title
    // already started may finish, and only while it is still verified.
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
    // The enlarged view owns the controller from the View release that asks
    // for it until the View release (or View+Y) that closes it: LB/RB switch
    // pages and no action pressed then reaches the game, also when the view
    // closes with the button still held. Directions always pass, and presses
    // made while its pause is being released are the game's.
    if(s.profile&&s.profile->pageNavigation){
        const bool opening=s.choosing||(s.awaiting!=0&&s.waitingKind==native::PauseCommandKind::Acquire)||
                           (s.action==InputAction::ToggleMap&&s.mapReady);
        const bool reserving=s.viewInput.update(s.map,opening,result.action);
        const auto pages=s.pages.update(raw.buttons,raw.connected,reserving,ViewReservableButtons);
        if(pages.step&&s.map)s.pageSteps+=pages.step;
        forwarded=static_cast<std::uint16_t>(forwarded&~pages.consumedButtons);
    }
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
    s.choosing=s.panelsWanted=s.panelsReady=false;s.map=false;s.page=0;s.pageSteps=0;
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
    const auto& layout=layout_of(s.profile);
    const char* reason=s.panelsReady?(s.choice==MapPanel::Unknown?"undecided-default-upper":"pixels"):"timeout-default-upper";
    s.mapPanel=s.panelsReady&&s.choice==MapPanel::Second?1:0;
    s.mapRect=rect(s.mapPanel?layout.lowerPanel:layout.upperPanel);
    char line[200]{};std::snprintf(line,sizeof(line),"live,map-panel,%s,reason=%s,upperBrown=%.3f,lowerBrown=%.3f",s.mapPanel?"lower":"upper",reason,s.upperBrown,s.lowerBrown);log(line);
    s.choosing=s.panelsWanted=s.panelsReady=false;
    if(!s.awaiting&&!s.token)queue(native::PauseCommandKind::Acquire);
}
// The native page tells where the map is (Dawn): no pixels, no delay.
void open_native_page(State& s) noexcept {
    const auto& layout=layout_of(s.profile);
    s.mapPanel=s.scene.mapPanel;s.viewPages=s.scene.viewPages?s.scene.viewPages:1;s.page=0;s.pageSteps=0;
    s.mapRect=rect(s.mapPanel?layout.lowerPanel:layout.upperPanel);
    char line[120]{};std::snprintf(line,sizeof(line),"live,map-panel,%s,reason=native-page,pages=%u",s.mapPanel?"lower":"upper",s.viewPages);log(line);
    if(!s.awaiting&&!s.token)queue(native::PauseCommandKind::Acquire);
}
}
bool panels_wanted(gfx::SourceRect& first,gfx::SourceRect& second) noexcept {
    auto& s=state();std::lock_guard<std::mutex> lock(s.mutex);
    if(!s.panelsWanted)return false;
    const auto& layout=layout_of(s.profile);
    s.panelsWanted=false;first=rect(layout.upperPanel);second=rect(layout.lowerPanel);return true;
}
void panel_rects(gfx::SourceRect& upper,gfx::SourceRect& lower) noexcept {
    auto& s=state();std::lock_guard<std::mutex> lock(s.mutex);
    const auto& layout=layout_of(s.profile);
    upper=rect(layout.upperPanel);lower=rect(layout.lowerPanel);
}
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
        // Outside our lock: the first sight of a loaded title DLL hashes it once.
        const auto title=title::observe(st.taskObserved?st.task:0,st.taskEpoch);
        const auto sceneSample=title::scene(title);
        auto& s=state();std::lock_guard<std::mutex> lock(s.mutex);if(!s.ready)return {};
        s.markerTarget={};s.pointerPolicy={};
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
        // Nothing requested while (or before) the title was verified applies
        // to another title; an owned map pause is released below by close().
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
            case MapCommand::Open:
                clear_release_tracking(s);
                if(s.profile->mapPanel==MapPanelSource::NativePage){
                    if(s.scene.mapPanel==0||s.scene.mapPanel==1)open_native_page(s);
                    else log_scene(s,"live,map-refused");
                }else{s.choosing=s.panelsWanted=true;s.panelsReady=false;s.chooseDeadline=GetTickCount64()+ChooseBudgetMs;}
                break;
            // Gameplay without a map to show (fade, death, sub-screen switch): nothing.
            case MapCommand::Refuse:log_scene(s,"live,map-refused");break;
            case MapCommand::None:break;
            }
        }
        s.action=InputAction::None;
        if(s.choosing){if(st.paused)close(false);else finish_choice(GetTickCount64());}
        // LB/RB steps collected by the filter apply only to a shown view.
        if(s.pageSteps){
            if(s.map&&s.profile&&s.profile->pageNavigation){
                const auto previous=s.page;s.page=step_page(s.page,s.pageSteps,s.viewPages);
                if(s.page!=previous)log(s.page?"live,view-page,status":"live,view-page,map");
            }
            s.pageSteps=0;
        }
        // View is the map command only in the title's gameplay; elsewhere
        // (title, menus, sequences, unknown states) it stays the game's own
        // Select. A stale mod pause keeps View as a mod command so it can
        // release it.
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
        // The pointer stays hidden from the paused game while the view holds input.
        if(s.identityActive&&s.profile->touchRemap)s.pointerPolicy.hide=s.map||s.choosing||s.awaiting!=0;
        if(!s.identityActive||!s.enabled||!s.focused||!s.nativeReady||(st.paused&&!s.token)){s.lastLayout=Presentation::Original;return {};}
        // The original compositor still renders every screen; no game setting
        // changes. The marker follows only titles whose replay was validated.
        const auto& layout=*s.profile;
        const auto presentation=title_presentation(layout,s.scene,s.enabled,s.map,s.lastLayout);
        s.lastLayout=presentation;
        if(presentation==Presentation::Map){
            s.markerTarget={layout.markerReplay,s.mapPanel,layout.title};
            if(s.page==0)return {gfx::Mode::Map,s.mapRect,false};
            return {gfx::Mode::Map,rect(s.mapPanel?layout.upperPanel:layout.lowerPanel),false};
        }
        // Outside the title's single-screen scenes: the original layout.
        if(presentation!=Presentation::Gameplay)return {};
        s.markerTarget={layout.markerReplay,-1,layout.title};
        s.pointerPolicy.remap=layout.touchRemap;
        return {gfx::Mode::Gameplay,rect(layout.gameplay),false};
    } catch(...) {return {};}
}
marker::PauseContext pause_context() noexcept {
    auto& s=state();std::lock_guard<std::mutex> lock(s.mutex);return s.pauseContext;
}
MarkerTarget marker_target() noexcept {
    auto& s=state();std::lock_guard<std::mutex> lock(s.mutex);return s.markerTarget;
}
PointerPolicy pointer_policy() noexcept {
    auto& s=state();std::lock_guard<std::mutex> lock(s.mutex);return s.pointerPolicy;
}
}
