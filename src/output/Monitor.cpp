#include "Monitor.hpp"
#include "../desktop/view/window/WindowEffectsController.hpp"
#include "../desktop/view/window/WindowPresentation.hpp"
#include "../helpers/MiscFunctions.hpp"
#include "../helpers/Format.hpp"
#include "../macros.hpp"
#include "SharedDefs.hpp"
#include "../helpers/TransferFunction.hpp"
#include "../helpers/math/Math.hpp"
#include "../protocols/ColorManagement.hpp"
#include "../Compositor.hpp"
#include "../config/ConfigValue.hpp"
#include "../config/shared/monitor/MonitorRuleManager.hpp"
#include "../config/shared/workspace/WorkspaceRuleManager.hpp"
#include "../config/shared/animation/AnimationTree.hpp"
#include "../protocols/GammaControl.hpp"
#include "../protocols/LayerShell.hpp"
#include "../protocols/PresentationTime.hpp"
#include "../protocols/DRMLease.hpp"
#include "../protocols/DRMSyncobj.hpp"
#include "../protocols/core/Output.hpp"
#include "../protocols/Screencopy.hpp"
#include "../protocols/ToplevelExport.hpp"
#include "../pointer/PointerManager.hpp"
#include "../pointer/PointerController.hpp"
#include "../layout/target/Target.hpp"
#include "../managers/eventLoop/EventLoopManager.hpp"
#include "../protocols/core/Compositor.hpp"
#include "../protocols/core/Subcompositor.hpp"
#include "../protocols/core/DataDevice.hpp"
#include "../render/Renderer.hpp"
#include "../ipc/s2/S2.hpp"
#include "../managers/screenshare/ScreenshareManager.hpp"
#include "../animation/AnimationManager.hpp"
#include "../animation/WorkspaceAnimationController.hpp"
#include "../managers/input/InputManager.hpp"
#include "../managers/fullscreen/FullscreenController.hpp"
#include "../errorOverlay/Overlay.hpp"
#include "../layout/LayoutManager.hpp"
#include "../layout/space/Space.hpp"
#include "../layout/algorithm/Algorithm.hpp"
#include "../i18n/Engine.hpp"
#include "../helpers/cm/ColorManagement.hpp"
#include "../state/MonitorState.hpp"
#include "../state/MonitorLayoutController.hpp"
#include "../state/WorkspacePlacementController.hpp"
#include "../state/WorkspaceState.hpp"
#include "../state/workspace/LifecyclePolicyAdapter.hpp"
#include "../workspace/query/Query.hpp"
#include "../helpers/time/Time.hpp"
#include "../desktop/view/LayerSurface.hpp"
#include "../desktop/state/GlobalWindowController.hpp"
#include "../desktop/state/FocusState.hpp"
#include "../desktop/state/FadingOutState.hpp"
#include "../event/EventBus.hpp"
#include "../helpers/Drm.hpp"
#include "MonitorFrameScheduler.hpp"
#include "OutputCommitCoordinator.hpp"
#include "MonitorWindowTransaction.hpp"
#include <aquamarine/output/Output.hpp>
#include "debug/log/Logger.hpp"
#include "notification/NotificationOverlay.hpp"
#include "MonitorFrameScheduler.hpp"
#include <hyprutils/memory/UniquePtr.hpp>
#include <hyprutils/string/String.hpp>
#include <hyprutils/utils/ScopeGuard.hpp>
#include <cstring>
#include <climits>
#include <optional>
#include <ranges>
#include <vector>
#include <algorithm>

using namespace Hyprutils::String;
using namespace Hyprutils::Utils;
using namespace Hyprutils::OS;
using enum NContentType::eContentType;
using namespace NColorManagement;
using namespace Desktop::View;
using namespace Render::GL;
using namespace Monitor;

constexpr const char* drmFormatToString(uint32_t drmFormat) {
    switch (drmFormat) {
        case DRM_FORMAT_XRGB2101010: return "DRM_FORMAT_XRGB2101010";
        case DRM_FORMAT_XBGR2101010: return "DRM_FORMAT_XBGR2101010";
        case DRM_FORMAT_XRGB8888: return "DRM_FORMAT_XRGB8888";
        case DRM_FORMAT_XBGR8888: return "DRM_FORMAT_XBGR8888";
        default: return "Invalid";
    }
}

CMonitor::CMonitor(SP<Aquamarine::IOutput> output_) : m_name(output_->name), m_state(this), m_output(output_), m_imageDescription(getDefaultImageDescription()) {
    Animation::mgr()->createAnimation(0.f, m_specialFade, Config::animationTree()->getAnimationPropertyConfig("specialWorkspaceIn"), AVARDAMAGE_NONE);
    m_specialFade->setUpdateCallback([this](auto) { g_pHyprRenderer->damageMonitor(m_self.lock()); });
    Animation::mgr()->createAnimation(0.f, m_specialDim, Config::animationTree()->getAnimationPropertyConfig("specialWorkspaceIn"), AVARDAMAGE_NONE);
    m_specialDim->setUpdateCallback([this](auto) { g_pHyprRenderer->damageMonitor(m_self.lock()); });
    Animation::mgr()->createAnimation(0.f, m_specialBlur, Config::animationTree()->getAnimationPropertyConfig("specialWorkspaceIn"), AVARDAMAGE_NONE);
    m_specialBlur->setUpdateCallback([this](auto) { g_pHyprRenderer->damageMonitor(m_self.lock()); });
    static auto PZOOMFACTOR = CConfigValue<Config::FLOAT>("cursor:zoom_factor");
    Animation::mgr()->createAnimation(*PZOOMFACTOR, m_cursorZoom, Config::animationTree()->getAnimationPropertyConfig("zoomFactor"), AVARDAMAGE_NONE);
    m_cursorZoom->setUpdateCallback([this](auto) { g_pHyprRenderer->damageMonitor(m_self.lock()); });
    Animation::mgr()->createAnimation(0.F, m_zoomAnimProgress, Config::animationTree()->getAnimationPropertyConfig("monitorAdded"), AVARDAMAGE_NONE);
    m_zoomAnimProgress->setUpdateCallback([this](auto) { g_pHyprRenderer->damageMonitor(m_self.lock()); });
    Animation::mgr()->createAnimation(0.F, m_backgroundOpacity, Config::animationTree()->getAnimationPropertyConfig("monitorAdded"), AVARDAMAGE_NONE);
    m_backgroundOpacity->setUpdateCallback([this](auto) { g_pHyprRenderer->damageMonitor(m_self.lock()); });
    Animation::mgr()->createAnimation(0.F, m_dpmsBlackOpacity, Config::animationTree()->getAnimationPropertyConfig("fadeDpms"), AVARDAMAGE_NONE);
    m_dpmsBlackOpacity->setUpdateCallback([this](auto) { g_pHyprRenderer->damageMonitor(m_self.lock()); });

    m_commitCoordinator = makeUnique<COutputCommitCoordinator>(this);
}

CMonitor::~CMonitor() {
    m_events.destroy.emit();
    if (g_pHyprRenderer && g_pHyprRenderer->glBackend())
        g_pHyprRenderer->glBackend()->destroyMonitorResources(m_self);
}

void CMonitor::emitReservedChangedIfNeeded() {
    if (m_lastAnnouncedReservedArea == m_reservedArea)
        return;
    m_lastAnnouncedReservedArea = m_reservedArea;
    Event::bus()->m_events.monitor.reservedChanged.emit(m_self.lock());
}

void CMonitor::onConnect(bool noRule) {
    m_primaryTakesCache = {};
    m_dsFailedOn        = {};
    Event::bus()->m_events.monitor.preAdded.emit(m_self.lock());
    CScopeGuard x = {[]() { State::monitorLayoutController()->arrange(); }};

    m_zoomAnimProgress->setValueAndWarp(0.F);
    m_zoomAnimFrameCounter = 0;

    g_pEventLoopManager->doLater([] {
        State::Workspace::placementController()->ensurePersistentWorkspacesPresent(
            nullptr, [](PHLWORKSPACE ws, PHLMONITOR mon, bool noWarp) { State::Workspace::placementController()->moveWorkspaceToMonitor(ws, mon, noWarp); });
        State::Workspace::placementController()->ensureWorkspacesOnAssignedMonitors(
            [](PHLWORKSPACE ws, PHLMONITOR mon, bool noWarp) { State::Workspace::placementController()->moveWorkspaceToMonitor(ws, mon, noWarp); });
    });

    m_listeners.frame        = m_output->events.frame.listen([this] {
        if (m_frameScheduler)
            m_frameScheduler->onFrame();
    });
    m_listeners.commit       = m_output->events.commit.listen([this] {
        if (!m_commitCoordinator->shouldForwardCommitEvent())
            return;

        m_events.commit.emit();
    });
    m_listeners.commitResult = m_output->events.commitResult.listen([this](const Aquamarine::IOutput::SCommitResult& result) { m_commitCoordinator->onCommitResult(result); });
    m_listeners.needsFrame   = m_output->events.needsFrame.listen([this] { scheduleFrame(Aquamarine::IOutput::AQ_SCHEDULE_NEEDS_FRAME); });

    m_listeners.presented = m_output->events.present.listen([this](const Aquamarine::IOutput::SPresentEvent& event) {
        timespec ts{};
        auto     flags = event.flags;

        if (event.when && event.when->tv_sec > 2) {
            // drop this timestamp, it's not valid. Likely drm is cringe. We can't push it further because
            // a) it's wrong, b) our translations aren't 100% accurate and risk underflows
            ts = *event.when;
        } else {
            clock_gettime(CLOCK_MONOTONIC, &ts);
            flags &= ~Aquamarine::IOutput::AQ_OUTPUT_PRESENT_HW_CLOCK;
        }

        PROTO::presentation->onPresented(m_self.lock(), ts, event.refresh, event.seq, flags, event.commitID, event.presented);

        // a skipped underlay frame's frame events follow the last presentation by a refresh
        if (event.presented)
            m_underlay.lastFrameEvents = Time::fromTimespec(&ts);

        m_commitCoordinator->onPresented(event.commitID, event.presented);

        if (!event.presented) {
            m_damage.damageEntire();
            return;
        }

        if (m_pendingDpmsAnimation) {
            m_pendingDpmsAnimationCounter++;
            // we give ourselves 5 frames of a buffer. The first presentation event still doesn't usually say that we actually
            // are scanning out to the CRTC, and it could still be modesetting.
            // this is not ideal (some CRTCs will just eat frames) but it's better than nothing

            m_dpmsBlackOpacity->setValueAndWarp(1.F);

            if (m_pendingDpmsAnimationCounter == 5) {
                *m_dpmsBlackOpacity    = 0.F;
                m_pendingDpmsAnimation = false;
            }
        }

        if (m_zoomAnimFrameCounter < 5) {
            m_zoomAnimFrameCounter++;

            // we give ourselves 5 frames of a buffer. The first presentation event still doesn't usually say that we actually
            // are scanning out to the CRTC, and it could still be modesetting.
            // this is not ideal (some CRTCs will just eat frames) but it's better than nothing
            m_zoomAnimProgress->setValueAndWarp(0.F);
            if (m_zoomAnimFrameCounter == 5) {
                // start the animation for realzies
                *m_zoomAnimProgress = 1.F;
            }

            // damage the entire display to force a frame immediately
            g_pEventLoopManager->doLater([self = m_self] {
                if (!self)
                    return;

                g_pHyprRenderer->damageMonitor(self.lock());
            });
        }

        // if the monitor has no pending frames, we wont hit the no damage send frame callback path in rendermonitor
        // this becomes really noticeable in new render scheduling. causing firefox to simply wait for a new frame callback.
        // when nothing is scheduling new frames.
        auto mon = m_self.lock();
        if (!isMirror() && !g_pHyprRenderer->shouldRenderMonitor(mon)) {
            auto const NOW = Time::steadyNow();
            if (m_activeWorkspace)
                g_pHyprRenderer->sendFrameEventsToWorkspace(mon, m_activeWorkspace, NOW);
            if (m_activeSpecialWorkspace)
                g_pHyprRenderer->sendFrameEventsToWorkspace(mon, m_activeSpecialWorkspace, NOW);
        }

        m_frameScheduler->onPresented();

        m_events.presented.emit(Time::fromTimespec(&ts));
    });

    m_listeners.destroy = m_output->events.destroy.listen([this] {
        LOG(Log::DEBUG, "Destroy called for monitor {}", m_name);

        m_listeners = {};

        onDisconnect(true);

        m_output              = nullptr;
        m_renderingInitPassed = false;

        State::monitorState()->remove(m_self.lock());
    });

    m_listeners.state = m_output->events.state.listen([this](const Aquamarine::IOutput::SStateEvent& event) {
        if (event.size == Vector2D{}) {
            // an indication to re-set state
            // we can't do much for createdByUser displays I think
            if (m_createdByUser)
                return;

            LOG(Log::DEBUG, "Reapplying monitor rule for {} from a state request", m_name);
            auto cpy = m_activeMonitorRule;
            applyMonitorRule(std::move(cpy));
            State::monitorLayoutController()->scheduleRecheck();
            return;
        }

        if (!m_createdByUser)
            return;

        const auto SIZE = event.size;

        m_forceSize = SIZE;

        auto rule = m_activeMonitorRule;

        if (SIZE == rule.m_resolution)
            return;

        rule.m_resolution = SIZE;

        applyMonitorRule(std::move(rule));
        State::monitorLayoutController()->scheduleRecheck();
    });

    m_frameScheduler         = makeUnique<CMonitorFrameScheduler>(m_self.lock());
    m_frameScheduler->m_self = WP<CMonitorFrameScheduler>(m_frameScheduler);
    m_windowTransaction      = makeUnique<CMonitorWindowTransaction>(m_self.lock());

    m_tearingState.canTear = m_output->getBackend()->type() == Aquamarine::AQ_BACKEND_DRM;

    m_description = m_output->description;
    // remove comma character from description. This allow monitor specific rules to work on monitor with comma on their description
    std::erase(m_description, ',');

    // field is backwards-compatible with intended usage of `szDescription` but excludes the parenthesized DRM node name suffix
    m_shortDescription = trim(std::format("{} {} {}", m_output->make, m_output->model, m_output->serial));
    std::erase(m_shortDescription, ',');

    if (m_output->getBackend()->type() != Aquamarine::AQ_BACKEND_DRM)
        m_createdByUser = true; // should be true. WL and Headless backends should be addable / removable

    // get monitor rule that matches
    auto monitorRule = Config::monitorRuleMgr()->get(m_self.lock());

    if (m_enabled && !monitorRule.m_disabled) {
        auto cpy = monitorRule;
        applyMonitorRule(std::move(cpy));

        m_output->state->resetExplicitFences();
        m_output->state->setEnabled(true);
        m_state.commit();
        return;
    }

    // if it's disabled, disable and ignore
    if (monitorRule.m_disabled) {

        m_output->state->resetExplicitFences();
        m_output->state->setEnabled(false);

        if (!m_state.commit())
            LOG(Log::ERR, "Couldn't commit disabled state on output {}", m_name);

        m_enabled = false;

        m_listeners.frame.reset();
        return;
    }

    if (m_output->nonDesktop) {
        LOG(Log::DEBUG, "Not configuring non-desktop output");

        for (auto& [name, lease] : PROTO::lease) {
            if (!lease || m_output->getBackend() != lease->getBackend())
                continue;

            lease->offer(m_self.lock());
        }

        return;
    }

    m_enabled = true;

    m_output->state->resetExplicitFences();
    m_output->state->setEnabled(true);

    // set mode, also applies
    if (!noRule) {
        auto cpy = monitorRule;
        applyMonitorRule(std::move(cpy));
    }

    if (!m_state.commit())
        LOG(Log::WARN, "state.commit() failed in CMonitor::onCommit");

    m_damage.setSize(m_transformedSize);

    LOG(Log::DEBUG, "Added new monitor with name {} at {:j0} with size {:j0}, pointer {:x}", m_name, m_position, m_pixelSize, rc<uintptr_t>(m_output.get()));

    // Arrange monitors before handing workspaces back to this monitor. A freshly created monitor has an unset
    // position (-1, -1) until it's arranged, and moveWorkspaceToMonitor translates floating windows relative to
    // the target's position. CMonitor::moveTo deliberately doesn't shift windows when moving away from the unset
    // position, so arranging afterwards would leave returning floating windows outside of the monitor.
    State::monitorLayoutController()->arrange();

    if (!isMirror())
        State::Workspace::monitorConnected(m_self.lock());

    m_forceFullFrames = std::max(m_forceFullFrames, 3); // force at least 3 full frames to make sure there is no blinking due to double-buffering.

    if (!m_activeMonitorRule.m_mirrorOf.empty())
        setMirror(m_activeMonitorRule.m_mirrorOf);

    if (!Desktop::focusState()->monitor()) // set the last monitor if it isn't set yet
        Desktop::focusState()->rawMonitorFocus(m_self.lock());

    g_pHyprRenderer->arrangeLayersForMonitor(m_id);
    g_layoutManager->recalculateMonitor(m_self.lock());

    // ensure VRR (will enable if necessary)
    Config::monitorRuleMgr()->ensureVRR(m_self.lock());

    // verify last mon valid
    bool found = false;
    for (auto const& m : State::monitorState()->monitors()) {
        if (m == Desktop::focusState()->monitor()) {
            found = true;
            break;
        }
    }

    if (!found)
        Desktop::focusState()->rawMonitorFocus(m_self.lock());

    scheduleFrame(Aquamarine::IOutput::AQ_SCHEDULE_NEW_MONITOR);

    PROTO::gamma->applyGammaToState(m_self.lock());

    m_events.connect.emit();

    IPC::Socket2::sock()->postEvent({"monitoradded", m_name});
    IPC::Socket2::sock()->postEvent({"monitoraddedv2", std::format("{},{},{}", m_id, m_name, m_shortDescription)});
    Event::bus()->m_events.monitor.added.emit(m_self.lock());
}

void CMonitor::onDisconnect(bool destroy) {
    Event::bus()->m_events.monitor.preRemoved.emit(m_self.lock());
    CScopeGuard x = {[this]() {
        if (g_pCompositor->m_isShuttingDown)
            return;
        IPC::Socket2::sock()->postEvent({"monitorremoved", m_name});
        IPC::Socket2::sock()->postEvent({"monitorremovedv2", std::format("{},{},{}", m_id, m_name, m_shortDescription)});
        Event::bus()->m_events.monitor.removed.emit(m_self.lock());
        State::monitorLayoutController()->scheduleRecheck();
    }};

    m_frameScheduler.reset();
    m_windowTransaction.reset();
    m_commitCoordinator->cancelPending();
    clearModeRetry();

    if (!m_enabled || g_pCompositor->m_isShuttingDown)
        return;

    LOG(Log::DEBUG, "onDisconnect called for {}", m_name);

    m_events.disconnect.emit();
    if (g_pHyprRenderer && g_pHyprRenderer->glBackend())
        g_pHyprRenderer->glBackend()->destroyMonitorResources(m_self);

    // Cleanup everything. Move windows back, snap cursor, shit.
    PHLMONITOR BACKUPMON = nullptr;
    for (auto const& m : State::monitorState()->monitors()) {
        if (m.get() != this) {
            BACKUPMON = m;
            break;
        }
    }

    // remove mirror
    if (m_mirrorOf) {
        m_mirrorOf->m_mirrors.erase(std::ranges::find_if(m_mirrorOf->m_mirrors, [&](const auto& other) { return other == m_self; }));

        // unlock software for mirrored monitor
        Pointer::mgr()->unlockSoftwareForMonitor(m_mirrorOf.lock());
        m_mirrorOf.reset();
    }

    if (!m_mirrors.empty()) {
        for (auto const& m : m_mirrors) {
            m->setMirror("");
        }

        Config::monitorRuleMgr()->scheduleReload();
    }

    m_listeners.frame.reset();
    m_listeners.presented.reset();
    m_listeners.needsFrame.reset();
    m_listeners.commit.reset();
    m_listeners.commitResult.reset();

    for (size_t i = 0; i < 4; ++i) {
        for (auto const& ls : m_layerSurfaceLayers[i]) {
            if (ls->m_layerSurface)
                ls->m_layerSurface->sendClosed();
        }
        m_layerSurfaceLayers[i].clear();
    }

    LOG(Log::DEBUG, "Removed monitor {}!", m_name);

    m_enabled             = false;
    m_renderingInitPassed = false;

    State::Workspace::monitorDisconnected(m_self.lock());

    if (BACKUPMON) {
        // snap cursor
        Pointer::pointerController()->warpTo(BACKUPMON->m_position + BACKUPMON->m_transformedSize / 2.F, true);
    } else {
        Desktop::focusState()->surface().reset();
        Desktop::focusState()->window().reset();
        Desktop::focusState()->monitor().reset();
    }

    if (m_activeWorkspace)
        m_activeWorkspace->setVisible(false);
    m_activeWorkspace.reset();

    if (m_output) {
        m_output->state->resetExplicitFences();
        m_output->state->setAdaptiveSync(false);
        m_output->state->setEnabled(false);

        if (!m_state.commit())
            LOG(Log::WARN, "state.commit() failed in CMonitor::onDisconnect");
    }

    if (Desktop::focusState()->monitor() == m_self)
        Desktop::focusState()->rawMonitorFocus(BACKUPMON);

    if (g_pHyprRenderer->m_mostHzMonitor == m_self) {
        int        mostHz         = 0;
        PHLMONITOR pMonitorMostHz = nullptr;

        for (auto const& m : State::monitorState()->monitors()) {
            if (m->m_refreshRate > mostHz && m != m_self) {
                pMonitorMostHz = m;
                mostHz         = m->m_refreshRate;
            }
        }

        g_pHyprRenderer->m_mostHzMonitor = pMonitorMostHz;
    }
}

static NColorManagement::eTransferFunction chooseTF(NTransferFunction::eTF tf) {
    const auto sdrEOTF = NTransferFunction::fromConfig();

    switch (tf) {
        case NTransferFunction::TF_GAMMA22:
        case NTransferFunction::TF_FORCED_GAMMA22: return NColorManagement::CM_TRANSFER_FUNCTION_GAMMA22;
        case NTransferFunction::TF_DEFAULT:
        case NTransferFunction::TF_SRGB: return NColorManagement::CM_TRANSFER_FUNCTION_SRGB;

        case NTransferFunction::TF_AUTO: // use global setting
            switch (sdrEOTF) {
                case NTransferFunction::TF_AUTO: return NColorManagement::CM_TRANSFER_FUNCTION_GAMMA22;
                default: return chooseTF(sdrEOTF);
            }

        default: UNREACHABLE();
    }
}

void CMonitor::applyCMType(NCMType::eCMType cmType, NTransferFunction::eTF cmSdrEotf) {
    // SDR presets must not carry EDID HDR metadata, or clients see headroom an SDR transfer function can't encode
    const bool                                                        hdrPreset           = cmType == NCMType::CM_HDR || cmType == NCMType::CM_HDR_EDID;
    auto                                                              oldImageDescription = m_imageDescription;
    const auto                                                        chosenSdrEotf       = chooseTF(cmSdrEotf);

    const auto                                                        masteringPrimaries  = getMasteringPrimaries();
    const NColorManagement::SImageDescription::SPCMasteringLuminances masteringLuminances = hdrPreset ? getMasteringLuminances() : SImageDescription::SPCMasteringLuminances{};

    const auto                                                        maxFALL = hdrPreset ? this->maxFALL() : 0;
    const auto                                                        maxCLL  = hdrPreset ? this->maxCLL() : 0;

    switch (cmType) {
        case NCMType::CM_SRGB:
            m_imageDescription = CImageDescription::from({.transferFunction = chosenSdrEotf,
                                                          .primariesNamed   = NColorManagement::CM_PRIMARIES_SRGB,
                                                          .primaries        = NColorManagement::getPrimaries(NColorManagement::CM_PRIMARIES_SRGB)});
            break; // assumes SImageDescription defaults to sRGB
        case NCMType::CM_WIDE:
            m_imageDescription = CImageDescription::from({.transferFunction    = chosenSdrEotf,
                                                          .primariesNameSet    = true,
                                                          .primariesNamed      = NColorManagement::CM_PRIMARIES_BT2020,
                                                          .primaries           = NColorManagement::getPrimaries(NColorManagement::CM_PRIMARIES_BT2020),
                                                          .masteringPrimaries  = masteringPrimaries,
                                                          .masteringLuminances = masteringLuminances,
                                                          .maxCLL              = maxCLL,
                                                          .maxFALL             = maxFALL});
            break;
        case NCMType::CM_DCIP3:
            m_imageDescription = CImageDescription::from({.transferFunction    = chosenSdrEotf,
                                                          .primariesNameSet    = true,
                                                          .primariesNamed      = NColorManagement::CM_PRIMARIES_DCI_P3,
                                                          .primaries           = NColorManagement::getPrimaries(NColorManagement::CM_PRIMARIES_DCI_P3),
                                                          .masteringPrimaries  = masteringPrimaries,
                                                          .masteringLuminances = masteringLuminances,
                                                          .maxCLL              = maxCLL,
                                                          .maxFALL             = maxFALL});
            break;
        case NCMType::CM_DP3:
            m_imageDescription = CImageDescription::from({.transferFunction    = chosenSdrEotf,
                                                          .primariesNameSet    = true,
                                                          .primariesNamed      = NColorManagement::CM_PRIMARIES_DISPLAY_P3,
                                                          .primaries           = NColorManagement::getPrimaries(NColorManagement::CM_PRIMARIES_DISPLAY_P3),
                                                          .masteringPrimaries  = masteringPrimaries,
                                                          .masteringLuminances = masteringLuminances,
                                                          .maxCLL              = maxCLL,
                                                          .maxFALL             = maxFALL});
            break;
        case NCMType::CM_ADOBE:
            m_imageDescription = CImageDescription::from({.transferFunction    = chosenSdrEotf,
                                                          .primariesNameSet    = true,
                                                          .primariesNamed      = NColorManagement::CM_PRIMARIES_ADOBE_RGB,
                                                          .primaries           = NColorManagement::getPrimaries(NColorManagement::CM_PRIMARIES_ADOBE_RGB),
                                                          .masteringPrimaries  = masteringPrimaries,
                                                          .masteringLuminances = masteringLuminances,
                                                          .maxCLL              = maxCLL,
                                                          .maxFALL             = maxFALL});
            break;
        case NCMType::CM_EDID:
            m_imageDescription = CImageDescription::from({.transferFunction    = chosenSdrEotf,
                                                          .primariesNameSet    = false,
                                                          .primariesNamed      = NColorManagement::CM_PRIMARIES_BT2020,
                                                          .primaries           = masteringPrimaries,
                                                          .masteringPrimaries  = masteringPrimaries,
                                                          .masteringLuminances = masteringLuminances,
                                                          .maxCLL              = maxCLL,
                                                          .maxFALL             = maxFALL});
            break;
        case NCMType::CM_HDR:
        case NCMType::CM_HDR_EDID: m_imageDescription = getHDRImageDescription(cmType); break;
        default: UNREACHABLE();
    }

    if (oldImageDescription != m_imageDescription) {
        if (PROTO::colorManagement)
            PROTO::colorManagement->onMonitorImageDescriptionChanged(m_self);
        m_blurFBDirty = true;
        // the output's colour transform changed, so everything already composited is stale
        g_pHyprRenderer->damageMonitor(m_self.lock());
    }
}

bool CMonitor::applyMonitorRuleSoft(Config::CMonitorRule&& pMonitorRule) {
    m_activeMonitorRule = std::move(pMonitorRule);
    m_reservedArea.setStatic(m_activeMonitorRule.m_reservedArea);
    m_transform         = m_activeMonitorRule.m_transform;
    m_supportsWideColor = m_activeMonitorRule.m_supportsWideColor;
    m_supportsHDR       = m_activeMonitorRule.m_supportsHDR;

    if (m_activeMonitorRule.m_iccFile.empty()) {
        // only apply explicit cm settings if we have no icc file

        m_cmType = m_activeMonitorRule.m_cmType;
        switch (m_cmType) {
            case NCMType::CM_AUTO: m_cmType = m_enabled10bit && supportsWideColor() ? NCMType::CM_WIDE : NCMType::CM_SRGB; break;
            case NCMType::CM_EDID: m_cmType = m_output->parsedEDID.chromaticityCoords.has_value() ? NCMType::CM_EDID : NCMType::CM_SRGB; break;
            case NCMType::CM_HDR:
            case NCMType::CM_HDR_EDID: m_cmType = supportsHDR() ? m_cmType : NCMType::CM_SRGB; break;
            default: break;
        }

        m_sdrEotf = m_activeMonitorRule.m_sdrEotf;

        m_sdrMinLuminance = m_activeMonitorRule.m_sdrMinLuminance;
        m_sdrMaxLuminance = m_activeMonitorRule.m_sdrMaxLuminance;

        m_minLuminance    = m_activeMonitorRule.m_minLuminance;
        m_maxLuminance    = m_activeMonitorRule.m_maxLuminance;
        m_maxAvgLuminance = m_activeMonitorRule.m_maxAvgLuminance;

        applyCMType(m_cmType, m_sdrEotf);

        m_sdrSaturation = m_activeMonitorRule.m_sdrSaturation;
        m_sdrBrightness = m_activeMonitorRule.m_sdrBrightness;
    } else {
        auto image = NColorManagement::SImageDescription::fromICC(m_activeMonitorRule.m_iccFile);
        if (!image) {
            LOG(Log::ERR, "icc for {} ({}) failed: {}", m_name, m_activeMonitorRule.m_iccFile, image.error());
            ErrorOverlay::overlay()->queueError(std::format("failed to apply icc {} to {}: {}", m_activeMonitorRule.m_iccFile, m_name, image.error()));
        } else {
            m_imageDescription = CImageDescription::from(*image);
            if (!m_imageDescription) {
                LOG(Log::ERR, "icc for {} ({}) failed 2: {}", m_name, m_activeMonitorRule.m_iccFile, image.error());
                ErrorOverlay::overlay()->queueError(std::format("failed to apply icc {} to {}: {}", m_activeMonitorRule.m_iccFile, m_name, image.error()));
                m_imageDescription = CImageDescription::from(SImageDescription{});
            }
        }
    }

    Vector2D xfmd     = m_transform % 2 == 1 ? Vector2D{m_pixelSize.y, m_pixelSize.x} : m_pixelSize;
    m_size            = (xfmd / m_scale).round();
    m_transformedSize = xfmd;

    if (m_createdByUser) {
        CBox transformedBox = {0, 0, m_transformedSize.x, m_transformedSize.y};
        transformedBox.transform(Math::wlTransformToHyprutils(Math::invertTransform(m_transform)), m_transformedSize.x, m_transformedSize.y);

        m_pixelSize = Vector2D(transformedBox.width, transformedBox.height);
    }

    updateMatrix();

    m_damage.setSize(m_transformedSize);

    setMirror(m_activeMonitorRule.m_mirrorOf);

    return true;
}

bool CMonitor::applyMonitorRule(Config::CMonitorRule&& pMonitorRule) {

    static auto PDISABLESCALECHECKS = CConfigValue<Config::INTEGER>("debug:disable_scale_checks");

    LOG(Log::DEBUG, "Applying monitor rule for {}", m_name);

    // a modeset would state the overlay again at the old mode: back to the primary alone first
    leaveUnderlay();

    if (m_forceSize.has_value())
        m_activeMonitorRule.m_resolution = m_forceSize.value();

    const auto RULE = &pMonitorRule;

    // if it's disabled, disable and ignore
    if (RULE->m_disabled) {
        m_output->state->resetExplicitFences();
        m_output->state->setAdaptiveSync(false);
        m_output->state->setEnabled(false);

        m_activeMonitorRule = std::move(pMonitorRule);

        if (!m_state.commit())
            LOG(Log::WARN, "state.commit() failed in CMonitor::applyMonitorRule");

        m_events.modeChanged.emit();

        return true;
    }

    // don't touch VR headsets
    if (m_output->nonDesktop)
        return true;

    const bool autoScale = RULE->m_scale <= 0.1;

    m_transform = RULE->m_transform;

    // accumulate requested modes in reverse order (cause inesrting at front is inefficient)
    std::vector<SP<Aquamarine::SOutputMode>> requestedModes;
    std::string                              requestedStr = "unknown";

    // use sortFunc, add best 3 to requestedModes in reverse, since we test in reverse
    auto addBest3Modes = [&](auto const& sortFunc) {
        auto sortedModes = m_output->modes;
        std::ranges::sort(sortedModes, sortFunc);
        if (sortedModes.size() > 3)
            sortedModes.erase(sortedModes.begin() + 3, sortedModes.end());
        requestedModes.insert_range(requestedModes.end(), sortedModes | std::views::reverse);
    };

    // last fallback is always preferred mode
    if (!m_output->preferredMode())
        LOG(Log::ERR, "Monitor {} has NO PREFERRED MODE", m_name);
    else
        requestedModes.push_back(m_output->preferredMode());

    if (RULE->m_resolution == Vector2D()) {
        requestedStr = "preferred";

        // fallback to first 3 modes if preferred fails/doesn't exist
        requestedModes = m_output->modes;
        if (requestedModes.size() > 3)
            requestedModes.erase(requestedModes.begin() + 3, requestedModes.end());
        std::ranges::reverse(requestedModes.begin(), requestedModes.end());

        if (m_output->preferredMode())
            requestedModes.push_back(m_output->preferredMode());
    } else if (RULE->m_resolution == Vector2D(-1, -1)) {
        requestedStr = "highrr";

        // sort prioritizing refresh rate 1st and resolution 2nd, then add best 3
        addBest3Modes([](auto const& a, auto const& b) {
            if (std::round(a->refreshRate) > std::round(b->refreshRate))
                return true;
            else if (DELTALESSTHAN(sc<float>(a->refreshRate), sc<float>(b->refreshRate), 1.F) && a->pixelSize.x > b->pixelSize.x && a->pixelSize.y > b->pixelSize.y)
                return true;
            return false;
        });
    } else if (RULE->m_resolution == Vector2D(-1, -2)) {
        requestedStr = "highres";

        // sort prioritizing resolution 1st and refresh rate 2nd, then add best 3
        addBest3Modes([](auto const& a, auto const& b) {
            if (a->pixelSize.x > b->pixelSize.x && a->pixelSize.y > b->pixelSize.y)
                return true;
            else if (DELTALESSTHAN(a->pixelSize.x, b->pixelSize.x, 1) && DELTALESSTHAN(a->pixelSize.y, b->pixelSize.y, 1) &&
                     std::round(a->refreshRate) > std::round(b->refreshRate))
                return true;
            return false;
        });
    } else if (RULE->m_resolution == Vector2D(-1, -3)) {
        requestedStr = "maxwidth";

        // sort prioritizing widest resolution 1st and refresh rate 2nd, then add best 3
        addBest3Modes([](auto const& a, auto const& b) {
            if (a->pixelSize.x > b->pixelSize.x)
                return true;
            if (a->pixelSize.x == b->pixelSize.x && std::round(a->refreshRate) > std::round(b->refreshRate))
                return true;
            return false;
        });
    } else if (RULE->m_resolution != Vector2D()) {
        // user requested mode
        requestedStr = std::format("{:X0}@{:.2f}Hz", RULE->m_resolution, RULE->m_refreshRate);

        // sort by closeness to requested, then add best 3
        addBest3Modes([&](auto const& a, auto const& b) {
            if (abs(a->pixelSize.x - RULE->m_resolution.x) < abs(b->pixelSize.x - RULE->m_resolution.x))
                return true;
            if (a->pixelSize.x == b->pixelSize.x && abs(a->pixelSize.y - RULE->m_resolution.y) < abs(b->pixelSize.y - RULE->m_resolution.y))
                return true;
            if (a->pixelSize == b->pixelSize && abs((a->refreshRate / 1000.f) - RULE->m_refreshRate) < abs((b->refreshRate / 1000.f) - RULE->m_refreshRate))
                return true;
            return false;
        });

        // if the best mode isn't close to requested, then try requested as custom mode first
        if (!requestedModes.empty()) {
            auto bestMode = requestedModes.back();
            if (!DELTALESSTHAN(bestMode->pixelSize.x, RULE->m_resolution.x, 1) || !DELTALESSTHAN(bestMode->pixelSize.y, RULE->m_resolution.y, 1) ||
                !DELTALESSTHAN(bestMode->refreshRate / 1000.f, RULE->m_refreshRate, 1))
                requestedModes.push_back(
                    makeShared<Aquamarine::SOutputMode>(Aquamarine::SOutputMode{.pixelSize = RULE->m_resolution, .refreshRate = RULE->m_refreshRate * 1000.f}));
        }

        // then if requested is custom, try custom mode first
        if (RULE->m_drmMode.type == DRM_MODE_TYPE_USERDEF) {
            if (m_output->getBackend()->type() != Aquamarine::eBackendType::AQ_BACKEND_DRM)
                LOG(Log::ERR, "Tried to set custom modeline on non-DRM output");
            else
                requestedModes.push_back(makeShared<Aquamarine::SOutputMode>(Aquamarine::SOutputMode{
                    .pixelSize = {RULE->m_drmMode.hdisplay, RULE->m_drmMode.vdisplay}, .refreshRate = RULE->m_drmMode.vrefresh, .modeInfo = RULE->m_drmMode}));
        }
    }

    const auto WAS10B             = m_enabled10bit;
    const auto OLDPIXELSIZE       = m_pixelSize;
    const auto OLDTRANSFORMEDSIZE = m_transformedSize;
    bool       success            = false;

    // Needed in case we are switching from a custom modeline to a standard mode
    m_customDrmMode = {};
    m_currentMode   = nullptr;

    const auto initialFormat = m_drmFormat != DRM_FORMAT_INVALID ? m_drmFormat : DRM_FORMAT_XRGB8888;
    m_output->state->setFormat(initialFormat);
    m_prevDrmFormat = m_drmFormat;
    m_drmFormat     = initialFormat;
    m_output->state->resetExplicitFences();

    if (Env::isTrace()) {
        LOG(Log::TRACE, "Monitor {} requested modes:", m_name);
        if (requestedModes.empty())
            LOG(Log::TRACE, "| None");
        else {
            for (auto const& mode : requestedModes | std::views::reverse) {
                LOG(Log::TRACE, "| {:X0}@{:.2f}Hz", mode->pixelSize, mode->refreshRate / 1000.f);
            }
        }
    }

    for (auto const& mode : requestedModes | std::views::reverse) {
        std::string modeStr = std::format("{:X0}@{:.2f}Hz", mode->pixelSize, mode->refreshRate / 1000.f);

        if (mode->modeInfo.has_value() && mode->modeInfo->type == DRM_MODE_TYPE_USERDEF) {
            m_state.applyCustomModeWithSwapchain(mode);

            if (!m_state.test()) {
                LOG(Log::ERR, "Monitor {}: REJECTED custom mode {}!", m_name, modeStr);
                continue;
            }

            m_customDrmMode = mode->modeInfo.value();
        } else {
            m_state.applyModeWithSwapchain(mode);

            if (!m_state.test()) {
                LOG(Log::ERR, "Monitor {}: REJECTED available mode {}!", m_name, modeStr);
                if (mode->preferred)
                    LOG(Log::ERR, "Monitor {}: REJECTED preferred mode!!!", m_name);
                continue;
            }

            m_customDrmMode = {};
        }

        m_refreshRate = mode->refreshRate / 1000.f;
        m_size        = mode->pixelSize;
        m_currentMode = mode;

        success = true;

        if (mode->preferred)
            LOG(Log::DEBUG, "Monitor {}: requested {}, using preferred mode {}", m_name, requestedStr, modeStr);
        else if (mode->modeInfo.has_value() && mode->modeInfo->type == DRM_MODE_TYPE_USERDEF)
            LOG(Log::DEBUG, "Monitor {}: requested {}, using custom mode {}", m_name, requestedStr, modeStr);
        else
            LOG(Log::DEBUG, "Monitor {}: requested {}, using available mode {}", m_name, requestedStr, modeStr);

        break;
    }

    // try requested as custom mode jic it works
    if (!success && RULE->m_resolution != Vector2D() && RULE->m_resolution != Vector2D(-1, -1) && RULE->m_resolution != Vector2D(-1, -2)) {
        auto        refreshRate = m_output->getBackend()->type() == Aquamarine::eBackendType::AQ_BACKEND_DRM ? RULE->m_refreshRate * 1000 : 0;
        auto        mode        = makeShared<Aquamarine::SOutputMode>(Aquamarine::SOutputMode{.pixelSize = RULE->m_resolution, .refreshRate = refreshRate});
        std::string modeStr     = std::format("{:X0}@{:.2f}Hz", mode->pixelSize, mode->refreshRate / 1000.f);

        m_state.applyCustomModeWithSwapchain(mode);

        if (m_state.test()) {
            LOG(Log::DEBUG, "Monitor {}: requested {}, using custom mode {}", m_name, requestedStr, modeStr);

            refreshRate     = mode->refreshRate / 1000.f;
            m_size          = mode->pixelSize;
            m_currentMode   = mode;
            m_customDrmMode = {};

            success = true;
        } else
            LOG(Log::ERR, "Monitor {}: REJECTED custom mode {}!", m_name, modeStr);
    }

    // try any of the modes if none of the above work
    if (!success) {
        for (auto const& mode : m_output->modes) {
            m_state.applyModeWithSwapchain(mode);

            if (!m_state.test())
                continue;

            auto errorMessage = I18n::i18nEngine()->localize(I18n::TXT_KEY_NOTIF_MONITOR_MODE_FAIL,
                                                             {{"name", m_name}, {"mode", std::format("{:X0}@{:.2f}Hz", mode->pixelSize, mode->refreshRate / 1000.f)}});
            LOG(Log::WARN, errorMessage);
            Notification::overlay()->addNotification(errorMessage, CHyprColor(0xff0000ff), 5000, ICON_WARNING);

            m_refreshRate   = mode->refreshRate / 1000.f;
            m_size          = mode->pixelSize;
            m_currentMode   = mode;
            m_customDrmMode = {};

            success = true;

            break;
        }
    }

    if (!success) {
        m_activeMonitorRule = std::move(pMonitorRule);
        LOG(Log::ERR, "Monitor {} has NO FALLBACK MODES, and an INVALID one was requested: {:X0}@{:.2f}Hz", m_name, RULE->m_resolution, RULE->m_refreshRate);
        scheduleModeRetry();
        return true;
    }

    clearModeRetry();

    const auto OLD_PIXEL_SIZE = m_pixelSize;

    m_vrrActive = m_output->state->state().adaptiveSync // disabled here, will be tested in CConfigManager::ensureVRR()
        || m_createdByUser;                             // wayland backend doesn't allow for disabling adaptive_sync

    m_pixelSize = m_size;

    m_output->state->setMaxBpc(RULE->m_enable10bit ? 10 : 8);

    static constexpr auto formats10bit = std::to_array<uint32_t>({DRM_FORMAT_XRGB2101010, DRM_FORMAT_XBGR2101010});
    static constexpr auto formats8bit  = std::to_array<uint32_t>({DRM_FORMAT_XRGB8888, DRM_FORMAT_XBGR8888});

    const bool            set10bit = RULE->m_enable10bit && trySetFormat(formats10bit);
    if (!set10bit)
        trySetFormat(formats8bit);

    m_enabled10bit = set10bit;

    if (autoScale)
        m_scale = getDefaultScale();
    else
        m_scale = RULE->m_scale;

    m_setScale = m_scale;

    Vector2D logicalSize = m_pixelSize / m_scale;
    if (!*PDISABLESCALECHECKS && (logicalSize.x != std::round(logicalSize.x) || logicalSize.y != std::round(logicalSize.y))) {
        // invalid scale, will produce fractional pixels.
        // find the nearest valid.

        float    searchScale = std::round(m_scale * 120.0);
        bool     found       = false;

        double   scaleZero = searchScale / 120.0;

        Vector2D logicalZero = m_pixelSize / scaleZero;
        if (logicalZero == logicalZero.round())
            m_scale = scaleZero;
        else {
            for (size_t i = 1; i < 90; ++i) {
                double   scaleUp   = (searchScale + i) / 120.0;
                double   scaleDown = (searchScale - i) / 120.0;

                Vector2D logicalUp   = m_pixelSize / scaleUp;
                Vector2D logicalDown = m_pixelSize / scaleDown;

                if (logicalUp == logicalUp.round()) {
                    found       = true;
                    searchScale = scaleUp;
                    break;
                }
                if (logicalDown == logicalDown.round()) {
                    found       = true;
                    searchScale = scaleDown;
                    break;
                }
            }

            if (!found) {
                if (autoScale)
                    m_scale = std::round(scaleZero);
                else {
                    LOG(Log::ERR, "Invalid scale passed to monitor, {} failed to find a clean divisor", m_scale);
                    ErrorOverlay::overlay()->queueError(std::format("Invalid scale passed to monitor {}, failed to find a clean divisor", m_name));
                    m_scale = getDefaultScale();
                }
            } else {
                if (!autoScale) {
                    LOG(Log::ERR, "Invalid scale passed to monitor, {} found suggestion {}", m_scale, searchScale);
                    static auto PDISABLENOTIFICATION = CConfigValue<Config::INTEGER>("misc:disable_scale_notification");
                    if (!*PDISABLENOTIFICATION) {
                        Notification::overlay()->addNotification(
                            I18n::i18nEngine()->localize(I18n::TXT_KEY_NOTIF_MONITOR_AUTO_SCALE,
                                                         {{"name", m_name}, {"scale", std::format("{:.2f}", m_scale)}, {"fixed_scale", std::format("{:.2f}", searchScale)}}),
                            CHyprColor(1.0, 0.0, 0.0, 1.0), 5000, ICON_WARNING);
                    }
                }
                m_scale = searchScale;
            }
        }
    }

    m_output->scheduleFrame();

    if (!m_state.commit())
        LOG(Log::ERR, "Couldn't commit output named {}", m_name);

    Vector2D xfmd     = m_transform % 2 == 1 ? Vector2D{m_pixelSize.y, m_pixelSize.x} : m_pixelSize;
    m_size            = (xfmd / m_scale).round();
    m_transformedSize = xfmd;

    if (WAS10B != m_enabled10bit || OLDPIXELSIZE != m_pixelSize || OLDTRANSFORMEDSIZE != m_transformedSize) {
        m_resources.reset(); // TODO skip for 10bit change and fp16?

        if (g_pHyprRenderer && g_pHyprRenderer->glBackend())
            g_pHyprRenderer->glBackend()->destroyMonitorResources(m_self);
    }

    applyMonitorRuleSoft(std::move(pMonitorRule));

    if (OLD_PIXEL_SIZE != m_pixelSize || OLDTRANSFORMEDSIZE != m_transformedSize) {
        m_background.reset();
        m_splash.reset();
    }

    updateVCGTRamps();

    LOG(Log::DEBUG, "Monitor {} data dump: res {:X}@{:.2f}Hz, scale {:.2f}, transform {}, pos {:X}, 10b {}", m_name, m_pixelSize, m_refreshRate, m_scale, sc<int>(m_transform),
        m_position, sc<int>(m_enabled10bit));

    m_events.modeChanged.emit();

    return true;
}

void CMonitor::scheduleModeRetry() {
    static constexpr int  MAX_MODE_RETRIES = 3;
    static constexpr auto RETRY_DELAY      = std::chrono::seconds(1);

    if (m_modeRetryTimer || m_modeRetryCount >= MAX_MODE_RETRIES)
        return;

    m_modeRetryCount++;

    LOG(Log::WARN, "Monitor {} failed to find a valid mode, retrying in 1s ({}/{})", m_name, m_modeRetryCount, MAX_MODE_RETRIES);

    m_modeRetryTimer = makeShared<CEventLoopTimer>(
        RETRY_DELAY,
        [self = m_self](SP<CEventLoopTimer>, void*) {
            const auto PMONITOR = self.lock();
            if (!PMONITOR)
                return;

            PMONITOR->m_modeRetryTimer.reset();

            if (!PMONITOR->m_output || !PMONITOR->m_enabled)
                return;

            auto rule = PMONITOR->m_activeMonitorRule;
            PMONITOR->applyMonitorRule(std::move(rule));
        },
        nullptr);
    g_pEventLoopManager->addTimer(m_modeRetryTimer);
}

void CMonitor::clearModeRetry() {
    m_modeRetryCount = 0;

    if (!m_modeRetryTimer)
        return;

    m_modeRetryTimer->cancel();
    g_pEventLoopManager->removeTimer(m_modeRetryTimer);
    m_modeRetryTimer.reset();
}

void CMonitor::scheduleFrame(Aquamarine::IOutput::scheduleFrameReason reason) {
    if ((g_pCompositor->m_aqBackend->hasSession() && !g_pCompositor->m_aqBackend->session->active) || !g_pCompositor->m_sessionActive)
        return;

    if (!m_enabled)
        return;

    if (m_renderingActive)
        m_pendingFrame = true;

    m_output->scheduleFrame(reason);
}

void CMonitor::addDamage(const pixman_region32_t* rg) {
    if (m_cursorZoom->value() != 1.f && State::monitorState()->query().vec(Pointer::mgr()->untransformedPosition()).run() == m_self) {
        m_damage.damageEntire();
        scheduleFrame(Aquamarine::IOutput::AQ_SCHEDULE_DAMAGE);
    } else if (m_damage.damage(rg))
        scheduleFrame(Aquamarine::IOutput::AQ_SCHEDULE_DAMAGE);
}

void CMonitor::addDamage(const CRegion& rg) {
    addDamage(const_cast<CRegion*>(&rg)->pixman());
}

void CMonitor::addDamage(const CBox& box) {
    if (m_cursorZoom->value() != 1.f && State::monitorState()->query().vec(Pointer::mgr()->untransformedPosition()).run() == m_self) {
        m_damage.damageEntire();
        scheduleFrame(Aquamarine::IOutput::AQ_SCHEDULE_DAMAGE);
        return;
    }

    if (m_damage.damage(box))
        scheduleFrame(Aquamarine::IOutput::AQ_SCHEDULE_DAMAGE);
}

bool CMonitor::shouldSkipScheduleFrameOnMouseEvent() {
    static auto PNOBREAK = CConfigValue<Config::INTEGER>("cursor:no_break_fs_vrr");
    static auto PMINRR   = CConfigValue<Config::INTEGER>("cursor:min_refresh_rate");

    // skip scheduling extra frames for fullsreen apps with vrr
    const auto FULLSCREEN_WINDOW  = Fullscreen::controller()->getFullscreenWindow(m_self.lock());
    const bool shouldRenderCursor = g_pHyprRenderer->shouldRenderCursor();
    const bool noBreak            = FULLSCREEN_WINDOW && (*PNOBREAK == 1 || (*PNOBREAK == 2 && FULLSCREEN_WINDOW->getContentType() == CONTENT_TYPE_GAME));
    const bool shouldSkip         = (!shouldRenderCursor || noBreak) && m_output->state->state().adaptiveSync;

    // keep requested minimum refresh rate
    if (shouldSkip && *PMINRR && m_lastPresentationTimer.getMillis() > 1000.0f / *PMINRR) {
        // damage whole screen because some previous cursor box damages were skipped
        m_damage.damageEntire();
        return false;
    }

    return shouldSkip;
}

bool CMonitor::isMirror() {
    return m_mirrorOf != nullptr;
}

MONITORID CMonitor::id() const {
    return m_id;
}

std::string_view CMonitor::name() const {
    return m_name;
}

std::string_view CMonitor::description() const {
    return m_description;
}

std::string_view CMonitor::shortDescription() const {
    return m_shortDescription;
}

bool CMonitor::matchesStaticSelector(std::string_view selector) const {
    if (selector.starts_with("desc:")) {
        // match by description
        const auto DESCRIPTIONSELECTOR = trim(selector.substr(5));

        return m_description.starts_with(DESCRIPTIONSELECTOR) || m_shortDescription.starts_with(DESCRIPTIONSELECTOR);
    } else {
        // match by selector
        return m_name == selector;
    }
}

Vector2D CMonitor::position() const {
    return m_position;
}

Vector2D CMonitor::size() const {
    return m_size;
}

Vector2D CMonitor::pixelSize() const {
    return m_pixelSize;
}

Vector2D CMonitor::transformedSize() const {
    return m_transformedSize;
}

float CMonitor::scale() const {
    return m_scale;
}

Hyprutils::Math::eTransform CMonitor::transform() const {
    return Math::wlTransformToHyprutils(m_transform);
}

bool CMonitor::enabled() const {
    return m_enabled;
}

bool CMonitor::hasOutput() const {
    return !!m_output;
}

SP<Aquamarine::IOutput> CMonitor::output() const {
    return m_output;
}

std::optional<Vector2D> CMonitor::explicitPosition() const {
    if (m_activeMonitorRule.m_offset == Vector2D{-INT32_MAX, -INT32_MAX})
        return {};

    return m_activeMonitorRule.m_offset;
}

Config::eAutoDirs CMonitor::autoDirection() const {
    return m_activeMonitorRule.m_autoDir;
}

Vector2D CMonitor::xwaylandPosition() const {
    return m_xwaylandPosition;
}

float CMonitor::xwaylandScale() const {
    return m_xwaylandScale;
}

void CMonitor::setXWaylandPosition(const Vector2D& pos) {
    m_xwaylandPosition = pos;
}

void CMonitor::setXWaylandScale(float scale_) {
    m_xwaylandScale = scale_;
}

void CMonitor::setMirror(const std::string& mirrorOf) {
    const auto PMIRRORMON = State::monitorState()->query().relativeTo(Desktop::focusState()->monitor()).configString(mirrorOf).run();

    if (PMIRRORMON == m_mirrorOf)
        return;

    if (PMIRRORMON && PMIRRORMON->isMirror()) {
        LOG(Log::ERR, "Cannot mirror a mirror!");
        return;
    }

    if (PMIRRORMON == m_self) {
        LOG(Log::ERR, "Cannot mirror self!");
        return;
    }

    if (!PMIRRORMON) {
        // disable mirroring

        if (m_mirrorOf) {
            m_mirrorOf->m_mirrors.erase(std::ranges::find_if(m_mirrorOf->m_mirrors, [&](const auto& other) { return other == m_self; }));

            // unlock software for mirrored monitor
            Pointer::mgr()->unlockSoftwareForMonitor(m_mirrorOf.lock());
        }

        m_mirrorOf.reset();

        State::Workspace::monitorConnected(m_self.lock());
    } else {
        PHLMONITOR BACKUPMON = nullptr;
        for (auto const& m : State::monitorState()->monitors()) {
            if (m.get() != this) {
                BACKUPMON = m;
                break;
            }
        }

        // move all the WS
        std::vector<PHLWORKSPACE> wspToMove;
        for (auto const& w : State::Workspace::state()->workspaces()) {
            if (w->m_monitor == m_self || !w->m_monitor)
                wspToMove.emplace_back(w.lock());
        }

        for (auto const& w : wspToMove) {
            State::Workspace::placementController()->moveWorkspaceToMonitor(w, BACKUPMON);
            Animation::Workspace::startAnimation(w, Animation::Workspace::ANIMATION_TYPE_IN, true, true);
        }

        m_activeWorkspace.reset();

        m_position = PMIRRORMON->m_position;

        m_mirrorOf = PMIRRORMON;

        m_mirrorOf->m_mirrors.push_back(m_self);

        Desktop::focusState()->rawMonitorFocus(State::monitorState()->monitors().front());

        // Software lock mirrored monitor
        Pointer::mgr()->lockSoftwareForMonitor(PMIRRORMON);
    }

    m_events.modeChanged.emit();
    Event::bus()->m_events.monitor.layoutChanged.emit();
}

float CMonitor::getDefaultScale() {
    if (!m_output)
        return 1;

    static constexpr double MMPERINCH = 25.4;

    const auto              DIAGONALPX = sqrt(pow(m_pixelSize.x, 2) + pow(m_pixelSize.y, 2));
    const auto              DIAGONALIN = sqrt(pow(m_output->physicalSize.x / MMPERINCH, 2) + pow(m_output->physicalSize.y / MMPERINCH, 2));

    const auto              PPI = DIAGONALPX / DIAGONALIN;

    if (PPI > 200 /* High PPI, 2x*/)
        return 2;
    else if (PPI > 140 /* Medium PPI, 1.5x*/)
        return 1.5;
    return 1;
}

static bool shouldWraparound(const Workspace::WorkspaceIDContainer id1, const Workspace::WorkspaceIDContainer id2) {
    static auto PWORKSPACEWRAPAROUND = CConfigValue<Config::INTEGER>("animations:workspace_wraparound");

    if (!*PWORKSPACEWRAPAROUND)
        return false;

    int64_t lowestID  = INT64_MAX;
    int64_t highestID = INT64_MIN;

    for (auto const& w : State::Workspace::state()->workspaces()) {
        const auto ID = w->numberedID();
        if (!ID)
            continue;
        lowestID  = std::min(sc<int64_t>(*ID), lowestID);
        highestID = std::max(sc<int64_t>(*ID), highestID);
    }

    return std::min(id1, id2) == lowestID && std::max(id1, id2) == highestID;
}

void CMonitor::changeWorkspace(const PHLWORKSPACE& pWorkspace, bool internal, bool noMouseMove, bool noFocus) {
    if (!pWorkspace)
        return;

    if (pWorkspace->type() == Workspace::eWorkspaceType::SPECIAL) {
        if (m_activeSpecialWorkspace != pWorkspace) {
            LOG(Log::DEBUG, "changeworkspace on special, togglespecialworkspace to {}", pWorkspace->addressableName());
            setSpecialWorkspace(pWorkspace, noFocus);
        }
        return;
    }

    if (pWorkspace == m_activeWorkspace)
        return;

    auto constraintGuard = g_pInputManager->pointerConstraints().suspend();

    if (!internal) {
        g_pInputManager->m_emptyFocusCursorSet = false;
        g_pInputManager->releaseAllMouseButtons();
    }

    const auto POLDWORKSPACE = m_activeWorkspace;
    m_activeWorkspace        = pWorkspace;

    if (POLDWORKSPACE) {
        POLDWORKSPACE->setVisible(false);
        POLDWORKSPACE->m_events.activeChanged.emit();
    }

    pWorkspace->setVisible(true);

    if (!internal) {
        const auto NEW_ID     = pWorkspace->numberedID();
        const auto OLD_ID     = POLDWORKSPACE ? POLDWORKSPACE->numberedID() : std::nullopt;
        const auto ANIMTOLEFT = NEW_ID && OLD_ID && (shouldWraparound(*NEW_ID, *OLD_ID) ^ (*NEW_ID > *OLD_ID));
        const auto ANIMSTYLE  = pWorkspace->m_animationStyle;
        if (POLDWORKSPACE)
            Animation::Workspace::startAnimation(POLDWORKSPACE, Animation::Workspace::ANIMATION_TYPE_OUT, ANIMTOLEFT, false, ANIMSTYLE);
        Animation::Workspace::startAnimation(pWorkspace, Animation::Workspace::ANIMATION_TYPE_IN, ANIMTOLEFT, false, ANIMSTYLE);

        // move pinned windows
        for (auto const& w : Desktop::windowState()->windows()) {
            if (w->m_workspace == POLDWORKSPACE && (w->m_state & WINDOW_STATE_PINNED))
                w->layoutTarget()->assignToSpace(pWorkspace->space());
        }

        if (!noFocus && !Desktop::focusState()->monitor()->m_activeSpecialWorkspace &&
            !(Desktop::focusState()->window() && (Desktop::focusState()->window()->m_state & WINDOW_STATE_PINNED) && Desktop::focusState()->window()->m_monitor == m_self)) {
            static auto PFOLLOWMOUSE = CConfigValue<Config::INTEGER>("input:follow_mouse");

            auto        pWindow = pWorkspace->getLastFocusedWindow();

            if (!pWindow) {
                if (*PFOLLOWMOUSE == 1)
                    pWindow = Desktop::viewState()->hitTest().windowAt(g_pInputManager->getMouseCoordsInternal(),
                                                                       Desktop::View::RESERVED_EXTENTS | Desktop::View::INPUT_EXTENTS | Desktop::View::ALLOW_FLOATING);

                if (!pWindow)
                    pWindow = pWorkspace->getFocusCandidate();
            }

            Desktop::focusState()->fullWindowFocus(pWindow, Desktop::FOCUS_REASON_WORKSPACE_CHANGE);
        }

        if (!noMouseMove)
            g_pInputManager->simulateMouseMovement();

        g_layoutManager->recalculateMonitor(m_self.lock(), Layout::CLayoutManager::RECALCULATE_MONITOR_REASON_WORKSPACE_CHANGE);

        if (!noFocus) {
            IPC::Socket2::sock()->postEvent({"workspace", pWorkspace->addressableName()});
            IPC::Socket2::sock()->postEvent({"workspacev2", std::format("{},{}", pWorkspace->addressableName(), pWorkspace->displayName())});
            Event::bus()->m_events.workspace.active.emit(pWorkspace);
        }
    }

    // Reset layer surfaces state on workspace change
    for (auto const& ls : Desktop::layerState()->layers()) {
        if (ls->m_monitor == m_self)
            ls->m_flags &= ~LAYER_FLAG_ABOVE_FULLSCREEN;
    }

    pWorkspace->m_events.activeChanged.emit();

    g_pHyprRenderer->damageMonitor(m_self.lock());

    Animation::Workspace::setFullscreenFadeAnimation(
        pWorkspace, Fullscreen::controller()->hasFullscreen(pWorkspace) ? Animation::Workspace::ANIMATION_TYPE_IN : Animation::Workspace::ANIMATION_TYPE_OUT);

    Config::monitorRuleMgr()->ensureVRR(m_self.lock());

    Desktop::globalWindowController()->updateSuspendedStates();

    if (m_activeSpecialWorkspace)
        Animation::Workspace::setFullscreenFadeAnimation(m_activeSpecialWorkspace,
                                                         Fullscreen::controller()->hasFullscreen(m_activeSpecialWorkspace) ? Animation::Workspace::ANIMATION_TYPE_IN :
                                                                                                                             Animation::Workspace::ANIMATION_TYPE_OUT);
}

void CMonitor::setSpecialWorkspaceVisualState(bool active) {
    static auto PDIMSPECIAL  = CConfigValue<Config::FLOAT>("decoration:dim_special");
    static auto PBLURSPECIAL = CConfigValue<Config::INTEGER>("decoration:blur:special");
    static auto PBLUR        = CConfigValue<Config::INTEGER>("decoration:blur:enabled");

    const auto  ANIM = active ? "specialWorkspaceIn" : "specialWorkspaceOut";

    m_specialFade->setConfig(Config::animationTree()->getAnimationPropertyConfig(ANIM));
    *m_specialFade = active ? 1.F : 0.F;
    m_specialDim->setConfig(Config::animationTree()->getAnimationPropertyConfig(ANIM));
    *m_specialDim = active ? *PDIMSPECIAL : 0.F;
    m_specialBlur->setConfig(Config::animationTree()->getAnimationPropertyConfig(ANIM));
    *m_specialBlur = active && *PBLURSPECIAL && *PBLUR ? 1.F : 0.F;
}

void CMonitor::setSpecialWorkspace(const PHLWORKSPACE& pWorkspace, bool noFocus) {
    if (m_activeSpecialWorkspace == pWorkspace)
        return;

    const auto POLDSPECIAL = m_activeSpecialWorkspace;

    setSpecialWorkspaceVisualState(!!pWorkspace);

    g_pHyprRenderer->damageMonitor(m_self.lock());

    auto constraintGuard = g_pInputManager->pointerConstraints().suspend();

    if (!pWorkspace) {
        // remove special if exists
        if (m_activeSpecialWorkspace) {
            m_activeSpecialWorkspace->setVisible(false);
            Animation::Workspace::startAnimation(m_activeSpecialWorkspace, Animation::Workspace::ANIMATION_TYPE_OUT, false, false, m_activeSpecialWorkspace->m_animationStyle);
            IPC::Socket2::sock()->postEvent({"activespecial", std::format(",{}", m_name)});
            IPC::Socket2::sock()->postEvent({"activespecialv2", std::format(",,{}", m_name)});

            // Reset layer surface state when closing special workspace
            for (auto const& ls : Desktop::layerState()->layers()) {
                if (ls->m_monitor == m_self)
                    ls->m_flags &= ~LAYER_FLAG_ABOVE_FULLSCREEN;
            }
        }
        m_activeSpecialWorkspace.reset();

        if (POLDSPECIAL)
            POLDSPECIAL->m_events.activeChanged.emit();

        g_layoutManager->recalculateMonitor(m_self.lock(), Layout::CLayoutManager::RECALCULATE_MONITOR_REASON_TOGGLE_SPECIAL_WORKSPACE);

        if (!(noFocus && Desktop::focusState()->monitor() != m_self) &&
            !(Desktop::focusState()->window() && (Desktop::focusState()->window()->m_state & WINDOW_STATE_PINNED) && Desktop::focusState()->window()->m_monitor == m_self)) {
            if (const auto PLAST = m_activeWorkspace->getLastFocusedWindow(); PLAST)
                Desktop::focusState()->fullWindowFocus(PLAST, Desktop::FOCUS_REASON_TOGGLE_SPECIAL_WORKSPACE);
            else
                g_pInputManager->refocus();
        }

        Animation::Workspace::setFullscreenFadeAnimation(
            m_activeWorkspace, Fullscreen::controller()->hasFullscreen(m_activeWorkspace) ? Animation::Workspace::ANIMATION_TYPE_IN : Animation::Workspace::ANIMATION_TYPE_OUT);

        Config::monitorRuleMgr()->ensureVRR(m_self.lock());

        Desktop::globalWindowController()->updateSuspendedStates();

        Event::bus()->m_events.workspace.specialActive.emit(nullptr, m_self.lock());

        return;
    }

    if (m_activeSpecialWorkspace) {
        m_activeSpecialWorkspace->setVisible(false);
        Animation::Workspace::startAnimation(m_activeSpecialWorkspace, Animation::Workspace::ANIMATION_TYPE_OUT, false, false, pWorkspace->m_animationStyle);
    }

    bool wasActive = false;
    //close if open elsewhere
    const auto PMONITOR = pWorkspace->m_monitor.lock();

    if (PMONITOR && PMONITOR->m_activeSpecialWorkspace == pWorkspace) {
        PMONITOR->setSpecialWorkspaceVisualState(false);
        PMONITOR->m_activeSpecialWorkspace.reset();
        g_layoutManager->recalculateMonitor(PMONITOR, Layout::CLayoutManager::RECALCULATE_MONITOR_REASON_TOGGLE_SPECIAL_WORKSPACE);
        g_pHyprRenderer->damageMonitor(PMONITOR);
        if (noFocus &&
            (Desktop::focusState()->monitor() == PMONITOR &&
             !(Desktop::focusState()->window() && (Desktop::focusState()->window()->m_state & WINDOW_STATE_PINNED) && Desktop::focusState()->window()->m_monitor == PMONITOR))) {
            // leave focus behind
            if (const auto PLAST = PMONITOR->m_activeWorkspace->getLastFocusedWindow(); PLAST)
                Desktop::focusState()->fullWindowFocus(PLAST, Desktop::FOCUS_REASON_TOGGLE_SPECIAL_WORKSPACE);
            else
                g_pInputManager->refocus();
        }
        IPC::Socket2::sock()->postEvent({"activespecial", std::format(",{}", PMONITOR->m_name)});
        IPC::Socket2::sock()->postEvent({"activespecialv2", std::format(",,{}", PMONITOR->m_name)});

        // Reset layer surfaces state on the old monitor when special workspace is stolen
        for (auto const& ls : Desktop::layerState()->layers()) {
            if (ls->m_monitor == PMONITOR)
                ls->m_flags &= ~LAYER_FLAG_ABOVE_FULLSCREEN;
        }

        const auto PACTIVEWORKSPACE = PMONITOR->m_activeWorkspace;
        Animation::Workspace::setFullscreenFadeAnimation(PACTIVEWORKSPACE,
                                                         PACTIVEWORKSPACE && Fullscreen::controller()->hasFullscreen(PACTIVEWORKSPACE) ? Animation::Workspace::ANIMATION_TYPE_IN :
                                                                                                                                         Animation::Workspace::ANIMATION_TYPE_OUT);

        wasActive = true;
    }

    // open special
    pWorkspace->m_monitor    = m_self;
    m_activeSpecialWorkspace = pWorkspace;
    m_activeSpecialWorkspace->setVisible(true);

    // Reset layer surface state when opening special workspace
    for (auto const& ls : Desktop::layerState()->layers()) {
        if (ls->m_monitor == m_self)
            ls->m_flags &= ~LAYER_FLAG_ABOVE_FULLSCREEN;
    }

    if (POLDSPECIAL)
        POLDSPECIAL->m_events.activeChanged.emit();

    if (PMONITOR != m_self)
        pWorkspace->m_events.monitorChanged.emit();

    if (!wasActive)
        pWorkspace->m_events.activeChanged.emit();

    if (!wasActive)
        Animation::Workspace::startAnimation(pWorkspace, Animation::Workspace::ANIMATION_TYPE_IN, true, false, pWorkspace->m_animationStyle);

    for (auto const& w : Desktop::windowState()->windows()) {
        if (w->m_workspace == pWorkspace) {
            w->m_monitor = m_self;
            w->updateSurfaceScaleTransformDetails();
            w->presentation().setAnimationsToMove();

            const auto MIDDLE = w->middle();
            if (w->isFloating() && VECNOTINRECT(MIDDLE, m_position.x, m_position.y, m_position.x + m_size.x, m_position.y + m_size.y) && !w->backend().traits().overrideRedirect) {
                // if it's floating and the middle isn't on the current mon, move it to the center
                const auto PMONFROMMIDDLE = State::monitorState()->query().vec(MIDDLE).run();
                Vector2D   pos            = w->position(Desktop::View::IGeometric::GEOMETRIC_GOAL);
                if (VECNOTINRECT(MIDDLE, PMONFROMMIDDLE->m_position.x, PMONFROMMIDDLE->m_position.y, PMONFROMMIDDLE->m_position.x + PMONFROMMIDDLE->m_size.x,
                                 PMONFROMMIDDLE->m_position.y + PMONFROMMIDDLE->m_size.y)) {
                    // not on any monitor, center
                    pos = middle() - w->size(Desktop::View::IGeometric::GEOMETRIC_GOAL) / 2.f;
                } else
                    pos = pos - PMONFROMMIDDLE->m_position + m_position;

                w->layoutTarget()->setPositionGlobal(CBox{pos, w->layoutTarget()->position().size()});
            }
        }
    }

    g_layoutManager->recalculateMonitor(m_self.lock(), Layout::CLayoutManager::RECALCULATE_MONITOR_REASON_TOGGLE_SPECIAL_WORKSPACE);

    if (!noFocus ||
        (Desktop::focusState()->monitor() == m_self &&
         !(Desktop::focusState()->window() && (Desktop::focusState()->window()->m_state & WINDOW_STATE_PINNED) && Desktop::focusState()->window()->m_monitor == m_self))) {
        // focus the workspace we just moved
        if (const auto PLAST = pWorkspace->getLastFocusedWindow(); PLAST)
            Desktop::focusState()->fullWindowFocus(PLAST, Desktop::FOCUS_REASON_TOGGLE_SPECIAL_WORKSPACE);
        else
            g_pInputManager->refocus();
    }

    IPC::Socket2::sock()->postEvent({"activespecial", std::format("{},{}", pWorkspace->addressableName(), m_name)});
    IPC::Socket2::sock()->postEvent({"activespecialv2", std::format("{},{},{}", pWorkspace->addressableName(), pWorkspace->displayName(), m_name)});

    g_pHyprRenderer->damageMonitor(m_self.lock());

    Animation::Workspace::setFullscreenFadeAnimation(
        pWorkspace, Fullscreen::controller()->hasFullscreen(pWorkspace) ? Animation::Workspace::ANIMATION_TYPE_IN : Animation::Workspace::ANIMATION_TYPE_OUT);

    Config::monitorRuleMgr()->ensureVRR(m_self.lock());

    Desktop::globalWindowController()->updateSuspendedStates();

    Event::bus()->m_events.workspace.specialActive.emit(pWorkspace, m_self.lock());
}

PHLWORKSPACE CMonitor::getCurrentWorkspace() {
    const auto MONITOR = m_self.lock();
    if (!MONITOR)
        return nullptr;

    // Check special workspace first as it renders on top of regular workspaces
    if (MONITOR->m_activeSpecialWorkspace)
        return MONITOR->m_activeSpecialWorkspace;

    return MONITOR->m_activeWorkspace;
}

void CMonitor::moveTo(const Vector2D& pos) {
    if (m_position == pos)
        return;

    const auto OLD_POSITION = m_position;

    m_position = pos;

    if (OLD_POSITION == Vector2D{-1, -1})
        return;

    const auto DELTA = pos - OLD_POSITION;

    for (const auto& w : Desktop::windowState()->windows()) {
        if (!validMapped(w) || !w->isFloating() || w->m_monitor != m_self)
            continue;

        w->layoutTarget()->setPositionGlobal(w->layoutTarget()->position().translate(DELTA));
    }
}

Vector2D CMonitor::middle() const {
    return m_position + m_size / 2.f;
}

const Mat3x3& CMonitor::getTransformMatrix() {
    return m_projMatrix;
}

const Mat3x3& CMonitor::getScaleMatrix() {
    return m_projOutputMatrix;
}

void CMonitor::updateMatrix() {
    m_projMatrix = Mat3x3::identity();
    if (m_transform != WL_OUTPUT_TRANSFORM_NORMAL)
        m_projMatrix.translate(m_pixelSize / 2.0).transform(Math::wlTransformToHyprutils(m_transform)).translate(-m_transformedSize / 2.0);

    m_projOutputMatrix = Mat3x3::outputProjection(m_pixelSize, HYPRUTILS_TRANSFORM_NORMAL);
}

CBox CMonitor::logicalBox() const {
    return {m_position, m_size};
}

CBox CMonitor::logicalBoxMinusReserved() const {
    return m_reservedArea.apply(logicalBox());
}

void CMonitor::scheduleDone() {
    if (m_doneScheduled)
        return;

    m_doneScheduled = true;

    g_pEventLoopManager->doLater([M = m_self] {
        if (!M) // if M is gone, we got destroyed, doesn't matter.
            return;

        if (!PROTO::outputs.contains(M->m_name))
            return;

        PROTO::outputs.at(M->m_name)->sendDone();
        M->m_doneScheduled = false;
    });
}

void CMonitor::setCTM(const Mat3x3& ctm_) {
    m_ctm        = ctm_;
    m_ctmUpdated = true;
    scheduleFrame(Aquamarine::IOutput::scheduleFrameReason::AQ_SCHEDULE_NEEDS_FRAME);
}

uint32_t CMonitor::isSolitaryBlocked(bool full) {
    uint32_t reasons = 0;

    if (m_windowTransaction && m_windowTransaction->hasPendingPresentation()) {
        reasons |= SC_TRANSFORM;
        if (!full)
            return reasons;
    }

    const auto PWORKSPACE = m_activeWorkspace;
    if (!PWORKSPACE) {
        reasons |= SC_WORKSPACE;
        return reasons;
    }

    // Monitor considers only FSMODE_FULLSCREEN as FS
    if (Fullscreen::controller()->getFullscreenModes(m_self.lock()).internal != Fullscreen::FSMODE_FULLSCREEN) {
        reasons |= SC_WINDOWED;
        if (!full)
            return reasons;
    }

    if (m_activeSpecialWorkspace) {
        reasons |= SC_SPECIAL;
        if (!full)
            return reasons;
    }

    bool pluginOverlay = false;
    Event::bus()->m_events.monitor.blockSolitary.emit(m_self.lock(), pluginOverlay);

    if (Notification::overlay()->hasAny() || pluginOverlay) {
        reasons |= SC_NOTIFICATION;
        if (!full)
            return reasons;
    }

    if (ErrorOverlay::overlay()->active() && Desktop::focusState()->monitor() == m_self) {
        reasons |= SC_ERRORBAR;
        if (!full)
            return reasons;
    }

    if (g_pSessionLockManager->isSessionLocked()) {
        reasons |= SC_LOCK;
        if (!full)
            return reasons;
    }

    if (PROTO::data->dndActive()) {
        reasons |= SC_DND;
        if (!full)
            return reasons;
    }

    if (PWORKSPACE->m_alpha->value() != 1.f) {
        reasons |= SC_ALPHA;
        if (!full)
            return reasons;
    }

    if (PWORKSPACE->m_renderOffset->value() != Vector2D{}) {
        reasons |= SC_OFFSET;
        if (!full)
            return reasons;
    }

    const auto PCANDIDATE = Fullscreen::controller()->getFullscreenWindow(m_self.lock());

    if (!PCANDIDATE) {
        reasons |= SC_CANDIDATE;
        return reasons;
    }

    if (!PCANDIDATE->presentation().opaque()) {
        reasons |= SC_OPAQUE;
        if (!full)
            return reasons;
    }

    if (PCANDIDATE->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT) != m_size || PCANDIDATE->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT) != m_position ||
        PCANDIDATE->positionAnimation()->isBeingAnimated() || PCANDIDATE->sizeAnimation()->isBeingAnimated()) {
        reasons |= SC_TRANSFORM;
        if (!full)
            return reasons;
    }

    for (auto const& overlayls : m_layerSurfaceLayers[ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY]) {
        if (overlayls->alpha()[LS_ALPHA_FADE]->goal() != 0.F) {
            reasons |= SC_OVERLAYS;
            if (!full)
                return reasons;
        }
    }

    for (auto const& fadeout : Desktop::fadingOutState()->fadeouts()) {
        if (!fadeout || fadeout->monitor() != m_self || fadeout->plane() < Desktop::eFadeoutPlane::FADEOUT_PLANE_WINDOW_OVER_FULLSCREEN)
            continue;

        reasons |= SC_FADEOUT;
        if (!full)
            return reasons;
    }

    for (auto const& w : Desktop::windowState()->windows()) {
        if (w == PCANDIDATE || !w->mapped() || !w->acceptsInput() || !w->alphaNonZero())
            continue;

        if (w->m_workspace == PCANDIDATE->m_workspace && w->isFloating() && w->isAllowedOverFullscreen() && w->presentation().visibleOnMonitor(m_self.lock())) {
            reasons |= SC_FLOAT;
            if (!full)
                return reasons;
        }
    }

    for (auto const& ws : State::Workspace::state()->workspaces()) {
        if (ws->m_alpha->value() <= 0.F || ws->type() != Workspace::eWorkspaceType::SPECIAL || ws->m_monitor != m_self)
            continue;

        reasons |= SC_WORKSPACES;
        if (!full)
            return reasons;
    }

    // check if it did not open any subsurfaces or shit, but a video's, or an underlay's layers
    if (!PCANDIDATE->getScanoutResource() && !PCANDIDATE->getUnderlayResource())
        reasons |= SC_SURFACES;

    return reasons;
}

void CMonitor::recheckSolitary() {
    m_solitaryClient.reset(); // reset it, if we find one it will be set.

    // a failed direct-scanout or underlay test is tried again once solitary rendering ends
    const auto PWORKSPACE = m_activeWorkspace;
    if (!PWORKSPACE || isSolitaryBlocked()) {
        m_dsFailedOn       = {};
        m_underlayFailedOn = {};
        return;
    }

    m_solitaryClient = Fullscreen::controller()->getFullscreenWindow(m_self.lock());
}

uint8_t CMonitor::isTearingBlocked(bool full) {
    uint8_t     reasons = 0;

    static auto PTEARINGENABLED = CConfigValue<Config::INTEGER>("general:allow_tearing");

    if (!m_tearingState.nextRenderTorn) {
        reasons |= TC_NOT_TORN;
        if (!full)
            return reasons;
    }

    if (!*PTEARINGENABLED) {
        reasons |= TC_USER;
        if (!full) {
            LOG(Log::WARN, "Tearing commit requested but the master switch general:allow_tearing is off, ignoring");
            return reasons;
        }
    }

    if (CMonitorZoomController::zoomFactor(m_cursorZoom->value(), m_zoomAnimProgress->value(),
                                           m_self == State::monitorState()->query().vec(Pointer::mgr()->untransformedPosition()).run()) != 1.F) {
        reasons |= TC_ZOOM;
        if (!full) {
            LOG(Log::WARN, "Tearing commit requested but scale factor is not 1, ignoring");
            return reasons;
        }
    }

    if (!m_tearingState.canTear) {
        reasons |= TC_SUPPORT;
        if (!full) {
            LOG(Log::WARN, "Tearing commit requested but monitor doesn't support it, ignoring");
            return reasons;
        }
    }

    // TODO: remove this when kernel allows tearing + hw cursor updated
    if (Pointer::mgr()->hasVisibleHWCursor(m_self.lock()))
        reasons |= TC_HW_CURSOR;

    if (m_solitaryClient.expired()) {
        reasons |= TC_CANDIDATE;
        return reasons;
    }

    if (!m_solitaryClient->canBeTorn())
        reasons |= TC_WINDOW;

    return reasons;
}

void CMonitor::updateSurfaceScaleTransformDetails() {
    for (auto const& w : Desktop::windowState()->windows()) {
        if (w->m_monitor == m_self)
            w->updateSurfaceScaleTransformDetails();
    }

    for (auto const& lsl : m_layerSurfaceLayers) {
        for (auto const& ls : lsl) {
            ls->updateSurfaceScaleTransformDetails();
        }
    }
}

bool CMonitor::updateTearing() {
    m_tearingState.activelyTearing = !isTearingBlocked();
    m_tearingState.nextRenderTorn  = false;
    return m_tearingState.activelyTearing;
}

uint16_t CMonitor::isDSBlocked(bool full) {
    uint16_t    reasons        = 0;
    static auto PDIRECTSCANOUT = CConfigValue<Config::INTEGER>("render:direct_scanout");
    static auto PNONSHADER     = CConfigValue<Config::INTEGER>("render:non_shader_cm");
    const auto  PWORKSPACE     = m_activeWorkspace;

    if (m_windowTransaction && m_windowTransaction->hasPendingPresentation()) {
        reasons |= DS_BLOCK_TRANSFORM;
        if (!full)
            return reasons;
    }

    // Fast reject for the hot render path; full=true callers still collect
    // the remaining blockers for hyprctl/debug output below.
    if (!canAttemptDirectScanoutFast()) {
        reasons |= DS_BLOCK_CANDIDATE;
        if (!full)
            return reasons;
    }

    if (*PDIRECTSCANOUT == 0) {
        reasons |= DS_BLOCK_USER;
        if (!full)
            return reasons;
    }

    if (*PDIRECTSCANOUT == 2) {
        const auto FSWINDOW = Fullscreen::controller()->getFullscreenWindow(m_self.lock());

        if (!PWORKSPACE || !FSWINDOW || Fullscreen::controller()->getFullscreenModes(FSWINDOW).internal != Fullscreen::FSMODE_FULLSCREEN) {
            reasons |= DS_BLOCK_WINDOWED;
            if (!full)
                return reasons;
        } else if (FSWINDOW->getContentType() != CONTENT_TYPE_GAME) {
            reasons |= DS_BLOCK_CONTENT;
            if (!full)
                return reasons;
        }
    }

    if (!m_mirrors.empty() || isMirror()) {
        reasons |= DS_BLOCK_MIRROR;
        if (!full)
            return reasons;
    }

    if (Screenshare::mgr()->isOutputDSBlocked(m_self.lock())) {
        reasons |= DS_BLOCK_RECORD;
        if (!full)
            return reasons;
    }

    if (Pointer::mgr()->softwareLockedFor(m_self.lock())) {
        reasons |= DS_BLOCK_SW;
        if (!full)
            return reasons;
    }

    const auto PCANDIDATE = m_solitaryClient.lock();
    if (!PCANDIDATE) {
        reasons |= DS_BLOCK_CANDIDATE;
        return reasons;
    }

    if (PCANDIDATE->effects().blocksDirectScanout()) {
        reasons |= DS_BLOCK_TRANSFORM;
        if (!full)
            return reasons;
    }

    // the solitary surface, or a video's picture placed on the output
    const auto PSOLITARY = PCANDIDATE->getSolitaryResource();
    const auto PSURFACE  = PSOLITARY ? PSOLITARY : PCANDIDATE->getVideoResource();
    if (!PSURFACE || !PSURFACE->m_current.texture || !PSURFACE->m_current.buffer) {
        reasons |= DS_BLOCK_SURFACE;
        return reasons;
    }

    const bool VIDEO     = !PSOLITARY;
    const auto PLACEMENT = VIDEO ? videoPlacement(PCANDIDATE, PSURFACE) : std::optional{Aquamarine::COutputState::SBufferPlacement{}};

    if (!PLACEMENT || (!VIDEO && (PSURFACE->m_current.bufferSize != m_pixelSize || PSURFACE->m_current.transform != m_transform))) {
        reasons |= DS_BLOCK_TRANSFORM;
        if (!full)
            return reasons;
    }

    // we can't scanout shm buffers, nor place a buffer the mgpu blit copies
    const auto params = PSURFACE->m_current.buffer->dmabuf();
    if (!params.success || !PSURFACE->m_current.texture->isDMA() /* dmabuf */ || (VIDEO && isMultiGPU())) {
        reasons |= DS_BLOCK_DMA;
        if (!full)
            return reasons;
    }

    // nor a layout the primary plane does not take: the test would fail and
    // its rollback of the output's format reconfigure the swapchain every frame
    if (params.success && !primaryTakes(params.format, params.modifier)) {
        reasons |= DS_BLOCK_FORMAT;
        if (!full)
            return reasons;
    }

    if (params.success && m_dsFailedOn.surface.lock() == PSURFACE && m_dsFailedOn.format == params.format && m_dsFailedOn.modifier == params.modifier &&
        (!PLACEMENT || m_dsFailedOn.placement == *PLACEMENT)) {
        reasons |= DS_BLOCK_FAILED;
        if (!full)
            return reasons;
    }

    if (VIDEO) {
        if (!PLACEMENT || videoBlockedByCM(PSURFACE, *PLACEMENT))
            reasons |= DS_BLOCK_CM;

        return reasons;
    }

    const bool surfaceIsHDR   = PSURFACE->m_colorManagement.valid() && PSURFACE->m_colorManagement->isHDR();
    const bool surfaceIsScRGB = surfaceIsHDR && PSURFACE->m_colorManagement->isWindowsScRGB();

    if (surfaceIsScRGB)
        reasons |= DS_BLOCK_CM; // block scRGB
    else if (*PNONSHADER != CM_NS_IGNORE) {
        if (!surfaceIsHDR && needsCM() && !canNoShaderCM(true))
            reasons |= DS_BLOCK_CM; // block SDR that needs CM while non-shader CM isn't available
        else if (surfaceIsHDR && !inHDR())
            reasons |= DS_BLOCK_CM; // block HDR while monitor isn't in HDR mode
    }

    return reasons;
}

std::optional<Aquamarine::COutputState::SBufferPlacement> CMonitor::videoPlacement(PHLWINDOW window, SP<CWLSurfaceResource> video) {
    // the black main surface is the whole output, what the plane leaves uncovered
    const auto MAINBOX = window->wlSurface()->getSurfaceBoxGlobal();
    if (!MAINBOX || *MAINBOX != CBox{m_position, m_size})
        return std::nullopt;

    std::optional<Vector2D> offset;
    for (const auto& subsurface : window->wlSurface()->resource()->m_subsurfaces) {
        if (subsurface && subsurface->m_surface == video)
            offset = subsurface->posRelativeToParent();
    }

    if (!offset)
        return std::nullopt;

    return placementAt(video, CBox{m_position + *offset, video->m_current.size});
}

std::optional<Aquamarine::COutputState::SBufferPlacement> CMonitor::placementAt(SP<CWLSurfaceResource> video, const CBox& box) {
    // planes neither rotate nor flip here
    if (m_transform != WL_OUTPUT_TRANSFORM_NORMAL || video->m_current.transform != WL_OUTPUT_TRANSFORM_NORMAL)
        return std::nullopt;

    const auto& STATE = video->m_current;

    // in output pixels, on the output: a plane clipped by the output may show garbage
    const auto DST = box.copy().translate(-m_position).scale(m_scale).round();
    if (DST.empty() || DST.x < 0 || DST.y < 0 || DST.x + DST.w > m_pixelSize.x || DST.y + DST.h > m_pixelSize.y)
        return std::nullopt;

    // in buffer pixels: the viewport's source at the buffer scale, else the whole buffer
    const auto BUFFERBOX = CBox{{}, STATE.bufferSize};
    const auto SRC       = STATE.viewport.hasSource ? STATE.viewport.source.copy().scale(STATE.scale).intersection(BUFFERBOX) : BUFFERBOX;
    if (SRC.empty())
        return std::nullopt;

    Aquamarine::COutputState::SBufferPlacement placement = {.source = SRC, .destination = DST};

    // a YCbCr buffer converts as the renderer imports it, so a composited frame looks the same
    const auto ATTRS = STATE.buffer ? STATE.buffer->dmabuf() : Aquamarine::SDMABUFAttrs{};
    if (ATTRS.success && NFormatUtils::isFormatYUV(ATTRS.format)) {
        const auto YCBCR = STATE.ycbcr();
        switch (YCBCR.encoding) {
            case NColorManagement::CM_YCBCR_BT601: placement.ycbcrEncoding = Aquamarine::AQ_OUTPUT_YCBCR_ENCODING_BT601; break;
            case NColorManagement::CM_YCBCR_BT709: placement.ycbcrEncoding = Aquamarine::AQ_OUTPUT_YCBCR_ENCODING_BT709; break;
            case NColorManagement::CM_YCBCR_BT2020: placement.ycbcrEncoding = Aquamarine::AQ_OUTPUT_YCBCR_ENCODING_BT2020; break;
        }
        placement.ycbcrRange = YCBCR.fullRange ? Aquamarine::AQ_OUTPUT_COLOR_RANGE_FULL : Aquamarine::AQ_OUTPUT_COLOR_RANGE_LIMITED;
    }

    return placement;
}

// the plane converts YCbCr to RGB and nothing else, and the kernel sends a YCbCr
// layer as SDR: the video shows as composited only if it needs no more
bool CMonitor::videoBlockedByCM(SP<CWLSurfaceResource> video, const Aquamarine::COutputState::SBufferPlacement& placement) {
    static auto PNONSHADER = CConfigValue<Config::INTEGER>("render:non_shader_cm");

    const bool  HAS_DESCRIPTION = video->m_colorManagement.valid() && video->m_colorManagement->hasImageDescription();
    if (HAS_DESCRIPTION && video->m_colorManagement->isHDR())
        return true;

    if (*PNONSHADER == CM_NS_IGNORE)
        return false;

    if (inHDR() || (needsCM() && !canNoShaderCM(true)))
        return true;

    // the fullscreen rule above, and any non-shader CTM, are decided for the
    // main surface's description: the video must share its primaries
    const auto FS_DESCRIPTION = getFSImageDescription();
    if (!FS_DESCRIPTION)
        return true;

    const auto& VIDEO_DESCRIPTION = HAS_DESCRIPTION ? video->m_colorManagement->imageDescription() : getDefaultImageDescription()->value();
    if (VIDEO_DESCRIPTION.getPrimaries() != FS_DESCRIPTION.value()->value().getPrimaries())
        return true;

    // SDR, decoded alike within the precision of a scan-out
    switch (VIDEO_DESCRIPTION.transferFunction) {
        case CM_TRANSFER_FUNCTION_SRGB:
        case CM_TRANSFER_FUNCTION_GAMMA22:
        case CM_TRANSFER_FUNCTION_BT1886: break;
        default: return true;
    }

    // the DCP takes a YCbCr layer's primaries from its encoding
    const bool BT2020 = VIDEO_DESCRIPTION.getPrimaries() == NColorManagement::getPrimaries(CM_PRIMARIES_BT2020);
    return placement.ycbcrEncoding != Aquamarine::AQ_OUTPUT_YCBCR_ENCODING_DEFAULT && (placement.ycbcrEncoding == Aquamarine::AQ_OUTPUT_YCBCR_ENCODING_BT2020) != BT2020;
}

uint16_t CMonitor::isUnderlayBlocked(bool full) {
    static auto PSHADER   = CConfigValue<std::string>("decoration:screen_shader");
    uint16_t    reasons   = 0;
    static auto PUNDERLAY = CConfigValue<Config::INTEGER>("render:underlay");

    if (!*PUNDERLAY) {
        reasons |= UL_BLOCK_USER;
        if (!full)
            return reasons;
    }

    // what reads the composite back would find the hole
    if (!m_mirrors.empty() || isMirror()) {
        reasons |= UL_BLOCK_MIRROR;
        if (!full)
            return reasons;
    }

    if (Screenshare::mgr()->isOutputDSBlocked(m_self.lock()) || needsACopyFB()) {
        reasons |= UL_BLOCK_RECORD;
        if (!full)
            return reasons;
    }

    // the cursor stays on its own plane, and the overlay at 1:1
    if (Pointer::mgr()->softwareLockedFor(m_self.lock()) || shouldUseSoftwareCursors() || m_cursorZoom->value() != 1.F || m_zoomAnimProgress->value() != 1.F) {
        reasons |= UL_BLOCK_SW;
        if (!full)
            return reasons;
    }

    // a screen shader would not reach the video
    // a shader set, as applyScreenShader() reads the option: unset is STRVAL_EMPTY, not ""
    if (!(*PSHADER).empty() && *PSHADER != STRVAL_EMPTY) {
        reasons |= UL_BLOCK_SHADER;
        if (!full)
            return reasons;
    }

    if (m_tearingState.activelyTearing) {
        reasons |= UL_BLOCK_TEARING;
        if (!full)
            return reasons;
    }

    const auto CANDIDATE = underlayCandidate();
    if (!CANDIDATE) {
        reasons |= UL_BLOCK_CANDIDATE;
        return reasons;
    }

    const auto PCANDIDATE = CANDIDATE->window;
    const auto PVIDEO     = CANDIDATE->surface;
    if (!PVIDEO) {
        reasons |= UL_BLOCK_SURFACE;
        return reasons;
    }

    // the composite goes on an overlay plane above the primary, in ARGB8888 for its alpha
    if (!m_output->hasOverlayPlane() || isMultiGPU() || !std::ranges::any_of(m_output->getOverlayFormats(), [](const auto& f) { return f.drmFormat == DRM_FORMAT_ARGB8888; })) {
        reasons |= UL_BLOCK_PLANE;
        if (!full)
            return reasons;
    }

    if ((m_windowTransaction && m_windowTransaction->hasPendingPresentation()) || PCANDIDATE->effects().blocksDirectScanout()) {
        reasons |= UL_BLOCK_TRANSFORM;
        if (!full)
            return reasons;
    }

    const auto PLACEMENT = underlayPlacement(PCANDIDATE, PVIDEO);
    if (!PLACEMENT) {
        reasons |= UL_BLOCK_TRANSFORM;
        if (!full)
            return reasons;
    }

    // getUnderlayResource() took only a dmabuf
    const auto ATTRS = PVIDEO->m_current.buffer->dmabuf();
    if (!primaryTakes(ATTRS.format, ATTRS.modifier)) {
        reasons |= UL_BLOCK_FORMAT;
        if (!full)
            return reasons;
    }

    if (m_underlayFailedOn.surface.lock() == PVIDEO && m_underlayFailedOn.format == ATTRS.format && m_underlayFailedOn.modifier == ATTRS.modifier &&
        (!PLACEMENT || m_underlayFailedOn.placement == *PLACEMENT)) {
        reasons |= UL_BLOCK_FAILED;
        if (!full)
            return reasons;
    }

    if (inHDR() || (PLACEMENT && underlayBlockedByCM(PCANDIDATE, PVIDEO, *PLACEMENT)))
        reasons |= UL_BLOCK_CM;

    // in a window: what blurs over the hole would blur the hole, and a video must show inside its window
    if (CANDIDATE->windowed && PLACEMENT) {
        const auto HOLE = PLACEMENT->destination.copy().scale(1.F / m_scale).translate(m_position);
        if (underlayBlurredOver(PCANDIDATE, HOLE))
            reasons |= UL_BLOCK_BLUR;
        if (CANDIDATE->video && underlayClipped(PCANDIDATE, HOLE))
            reasons |= UL_BLOCK_CLIP;
    }

    return reasons;
}

std::optional<CMonitor::SUnderlayCandidate> CMonitor::underlayCandidate() {
    static auto PUNDERLAY = CConfigValue<Config::INTEGER>("render:underlay");

    // a fullscreen window alone on the monitor, with nothing above it but the cursor: its video. Without one, it is
    // scanned out directly
    if (const auto SOLITARY = m_solitaryClient.lock())
        return SUnderlayCandidate{.window = SOLITARY, .surface = SOLITARY->getUnderlayResource(), .video = true};

    if (*PUNDERLAY < 2 || !m_activeWorkspace || Fullscreen::controller()->getFullscreenWindow(m_self.lock()))
        return std::nullopt;

    // the focused window on the workspace shown, at rest: what changes is mostly its content
    const auto WINDOW = Desktop::focusState()->window();
    if (!WINDOW || WINDOW->m_monitor != m_self || !WINDOW->mapped() || WINDOW->isHidden() || !WINDOW->m_workspace)
        return std::nullopt;

    const auto SHOWN = m_activeSpecialWorkspace ? m_activeSpecialWorkspace : m_activeWorkspace;
    if (WINDOW->m_workspace != SHOWN || SHOWN->m_alpha->value() != 1.F || SHOWN->m_renderOffset->value() != Vector2D{})
        return std::nullopt;

    if (WINDOW->positionAnimation()->isBeingAnimated() || WINDOW->sizeAnimation()->isBeingAnimated() || WINDOW->presentation().dimPercent() > 0.F)
        return std::nullopt;

    // the composite would blend it: every alpha the renderer multiplies (GLRenderer's opaque test)
    float alpha = 1.F;
    for (const auto TYPE : {Desktop::View::WINDOW_ALPHA_FADE, Desktop::View::WINDOW_ALPHA_ACTIVE, Desktop::View::WINDOW_ALPHA_FULLSCREEN, Desktop::View::WINDOW_ALPHA_LAYOUT,
                            Desktop::View::WINDOW_ALPHA_MOVE_TO_WORKSPACE, Desktop::View::WINDOW_ALPHA_MOVE_FROM_WORKSPACE})
        alpha *= WINDOW->presentation().alphaValue(TYPE);
    if (alpha != 1.F)
        return std::nullopt;

    // its video, when it has one among its layers (WebRender's native compositor): the video changes, its page seldom
    if (const auto VIDEO = WINDOW->getWindowedVideoResource())
        return SUnderlayCandidate{.window = WINDOW, .surface = VIDEO, .video = true, .windowed = true};

    return SUnderlayCandidate{.window = WINDOW, .surface = WINDOW->getContentUnderlayResource(), .video = false, .windowed = true};
}

// a video's hole is square: it must lie inside its window's box. Where it reaches the window's rounded corners, the
// hole takes their shape (underlayCorners)
bool CMonitor::underlayClipped(PHLWINDOW window, const CBox& hole) {
    const auto BOX = window->getWindowMainSurfaceBox();
    // a fraction of a logical pixel from the placement's rounding to output pixels
    constexpr double SLACK = 0.5;
    return hole.x < BOX.x - SLACK || hole.y < BOX.y - SLACK || hole.x + hole.w > BOX.x + BOX.w + SLACK || hole.y + hole.h > BOX.y + BOX.h + SLACK;
}

// a window's video reaching its window's rounded corners: the window's box in output pixels, as the renderer rounds its
// main surface (ElementRenderer's windowBox), for the hole to take its corners. Empty when the hole is clear of them
CBox CMonitor::underlayCorners(PHLWINDOW window, const CBox& hole) {
    const auto R = window->presentation().rounding() * m_scale;
    if (R <= 0)
        return {};

    const auto BOX   = window->getWindowMainSurfaceBox().translate(-m_position).scale(m_scale).round();
    const auto INNER = hole.copy().expand(-0.5);
    for (const auto& CORNER :
         {CBox{BOX.x, BOX.y, R, R}, CBox{BOX.x + BOX.w - R, BOX.y, R, R}, CBox{BOX.x, BOX.y + BOX.h - R, R, R}, CBox{BOX.x + BOX.w - R, BOX.y + BOX.h - R, R, R}}) {
        if (INNER.overlaps(CORNER))
            return BOX;
    }

    return {};
}

// what blurs over a window's content would sample the hole: another window over it (the cached blur samples the
// wallpaper and layers only, but a window under its blur makes it live), or a top or overlay layer
bool CMonitor::underlayBlurredOver(PHLWINDOW window, const CBox& hole) {
    static auto PBLUR = CConfigValue<Config::INTEGER>("decoration:blur:enabled");
    if (!*PBLUR)
        return false;

    for (auto const& w : Desktop::windowState()->windows()) {
        if (w == window || !w->mapped() || w->isHidden() || !w->alphaNonZero() || !w->presentation().visibleOnMonitor(m_self.lock()))
            continue;

        if (w->m_ruleApplicator->noBlur().valueOrDefault() || !w->getFullWindowBoundingBox().overlaps(hole))
            continue;

        return true;
    }

    for (const auto LAYER : {ZWLR_LAYER_SHELL_V1_LAYER_TOP, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY}) {
        for (auto const& ls : m_layerSurfaceLayers[LAYER]) {
            if (!ls || !ls->m_ruleApplicator->blur().valueOrDefault())
                continue;

            if (const auto BOX = ls->surfaceLogicalBox(); BOX && BOX->overlaps(hole))
                return true;
        }
    }

    return false;
}

std::optional<Aquamarine::COutputState::SBufferPlacement> CMonitor::underlayPlacement(PHLWINDOW window, SP<CWLSurfaceResource> video) {
    const auto MAINBOX = window->wlSurface()->getSurfaceBoxGlobal();
    if (!MAINBOX)
        return std::nullopt;

    // a window's content may be its main surface
    Vector2D pos = MAINBOX->pos();
    if (video != window->wlSurface()->resource()) {
        if (!video->m_role || video->m_role->role() != SURFACE_ROLE_SUBSURFACE)
            return std::nullopt;

        const auto SUBSURFACE = sc<CSubsurfaceRole*>(video->m_role.get())->m_subsurface.lock();
        if (!SUBSURFACE)
            return std::nullopt;

        // its position through all its parents: Firefox's video is a grandchild of the main surface
        pos += SUBSURFACE->posRelativeToParent();
    }

    const auto PLACEMENT = placementAt(video, CBox{pos, video->m_current.size});
    if (!PLACEMENT)
        return std::nullopt;

    // what appledrm's planes scale: at most 2x up and 4x down, and at least 32x32 on the screen
    const auto& SRC = PLACEMENT->source;
    const auto& DST = PLACEMENT->destination;
    if (DST.w < 32 || DST.h < 32 || DST.w > SRC.w * 2 || DST.h > SRC.h * 2 || DST.w * 4 < SRC.w || DST.h * 4 < SRC.h)
        return std::nullopt;

    return PLACEMENT;
}

// the composite is colour-managed by the shader, the video by the DCP alone: it takes a YCbCr
// layer as SDR, with the primaries its encoding implies and BT.709's transfer function
bool CMonitor::underlayBlockedByCM(PHLWINDOW window, SP<CWLSurfaceResource> video, const Aquamarine::COutputState::SBufferPlacement& placement) {
    const bool HAS_DESCRIPTION = video->m_colorManagement.valid() && video->m_colorManagement->hasImageDescription();
    if (HAS_DESCRIPTION && video->m_colorManagement->isHDR())
        return true;

    // an ICC profile reaches the composite only
    if (m_imageDescription->value().icc.present)
        return true;

    // a window's RGB content reaches the DCP as is: in the output's description, or in one whose primaries alone differ,
    // which the output's CTM converts (render:non_shader_cm). The CTM takes the whole frame and the composite then skips
    // its CM, as for a fullscreen window: every other window shown must be in the same description
    const auto ATTRS = video->m_current.buffer ? video->m_current.buffer->dmabuf() : Aquamarine::SDMABUFAttrs{};
    if (ATTRS.success && !NFormatUtils::isFormatYUV(ATTRS.format)) {
        static auto PNONSHADER = CConfigValue<Config::INTEGER>("render:non_shader_cm");
        const auto  SOURCE     = underlayContentDescription(window, video);
        if (SOURCE == m_imageDescription)
            return false;

        if (*PNONSHADER == CM_NS_IGNORE || !canNoShaderCM(SOURCE, true))
            return true;

        for (auto const& w : Desktop::windowState()->windows()) {
            if (!w->mapped() || w->isHidden() || !w->wlSurface() || !w->wlSurface()->resource() || !w->presentation().visibleOnMonitor(m_self.lock()))
                continue;

            const auto SURF = w->wlSurface()->resource()->findWithCM();
            if ((SURF ? CImageDescription::from(SURF->m_colorManagement->imageDescription()) : getDefaultImageDescription()) != SOURCE)
                return true;
        }

        return false;
    }

    const auto& DESCRIPTION = HAS_DESCRIPTION ? video->m_colorManagement->imageDescription() : getDefaultImageDescription()->value();
    switch (DESCRIPTION.transferFunction) {
        case CM_TRANSFER_FUNCTION_SRGB:
        case CM_TRANSFER_FUNCTION_GAMMA22:
        case CM_TRANSFER_FUNCTION_BT1886: break;
        default: return true;
    }

    // which primaries the DCP takes for BT.601 (525 or 625 lines) is not known
    switch (placement.ycbcrEncoding) {
        case Aquamarine::AQ_OUTPUT_YCBCR_ENCODING_BT709: return DESCRIPTION.getPrimaries() != NColorManagement::getPrimaries(CM_PRIMARIES_SRGB);
        case Aquamarine::AQ_OUTPUT_YCBCR_ENCODING_BT2020: return DESCRIPTION.getPrimaries() != NColorManagement::getPrimaries(CM_PRIMARIES_BT2020);
        default: return true;
    }
}

// what a window's content is in: its own description, else its tree's (findWithCM, as for a fullscreen window), else the default
PImageDescription CMonitor::underlayContentDescription(PHLWINDOW window, SP<CWLSurfaceResource> content) {
    if (content->m_colorManagement.valid() && content->m_colorManagement->hasImageDescription())
        return CImageDescription::from(content->m_colorManagement->imageDescription());

    const auto SURF = window && window->wlSurface() && window->wlSurface()->resource() ? window->wlSurface()->resource()->findWithCM() : nullptr;
    return SURF ? CImageDescription::from(SURF->m_colorManagement->imageDescription()) : getDefaultImageDescription();
}

bool CMonitor::ensureUnderlaySwapchain() {
    if (!m_underlay.swapchain) {
        const auto BACKEND = m_output->getBackend();
        if (!BACKEND || !BACKEND->preferredAllocator())
            return false;

        const auto ALLOCATOR = BACKEND->preferredAllocator();

        const auto PRIMARY   = BACKEND->getPrimary();
        m_underlay.swapchain = Aquamarine::CSwapchain::create(ALLOCATOR, PRIMARY ? PRIMARY.lock() : BACKEND);
        if (!m_underlay.swapchain)
            return false;
    }

    const auto& OPTIONS = m_underlay.swapchain->currentOptions();
    if (OPTIONS.format == DRM_FORMAT_ARGB8888 && OPTIONS.scanout && OPTIONS.length == 3 && OPTIONS.size == m_pixelSize)
        return true;

    // ARGB8888 by name, for its 8-bit alpha, in a layout the output takes; the test checks it on the overlay plane
    return m_underlay.swapchain->reconfigure(Aquamarine::SSwapchainOptions{
        .length        = 3,
        .size          = m_pixelSize,
        .format        = DRM_FORMAT_ARGB8888,
        .scanout       = true,
        .scanoutOutput = m_output,
    });
}

bool CMonitor::prepareUnderlay(bool commit) {
    const auto LEAVE = [this] {
        const bool WAS_ON = m_underlay.entered;
        leaveUnderlay();

        // the fullscreen CTM may apply again, before the composite renders for it
        if (WAS_ON)
            g_pHyprRenderer->handleFullscreenSettings(m_self.lock());

        return false;
    };

    // a frame committed later (render:new_render_scheduling's) composites on the primary
    if (!commit || isUnderlayBlocked())
        return LEAVE();

    // isUnderlayBlocked() found it, with a surface to take
    const auto CANDIDATE  = underlayCandidate();
    const auto PCANDIDATE = CANDIDATE->window;
    const auto PVIDEO     = CANDIDATE->surface;
    const auto PLACEMENT  = underlayPlacement(PCANDIDATE, PVIDEO);
    const auto ATTRS      = PVIDEO->m_current.buffer->dmabuf();
    if (!PLACEMENT || !ensureUnderlaySwapchain()) {
        LOG(Log::ERR, "prepareUnderlay: no overlay swapchain for {}", m_name);
        m_underlayFailedOn = {.surface = PVIDEO, .format = ATTRS.format, .modifier = ATTRS.modifier, .placement = PLACEMENT.value_or(Aquamarine::COutputState::SBufferPlacement{})};
        return LEAVE();
    }

    const bool ENTERING = !m_underlay.entered;
    if (ENTERING) {
        // a direct scan-out left before this (handleDSleave): the monitor's format is the composite's
        m_prevDrmFormat    = m_drmFormat;
        m_underlay.entered = true;
    }

    // tested on entry, and when the video, its layout or its place changes
    const bool NEEDS_TEST =
        ENTERING || m_underlay.surface != PVIDEO || m_underlay.format != ATTRS.format || m_underlay.modifier != ATTRS.modifier || m_underlay.placement != *PLACEMENT;

    m_underlay.surface   = PVIDEO;
    m_underlay.window    = PCANDIDATE;
    m_underlay.video     = CANDIDATE->video;
    m_underlay.format    = ATTRS.format;
    m_underlay.modifier  = ATTRS.modifier;
    m_underlay.placement = *PLACEMENT;

    // a window's content: the hole's corners as the renderer rounds the surface (ElementRenderer: the window's, less one).
    // A window's video reaching its window's corners: the hole is the window's rounded shape, cut to the video
    const auto  SHAPE         = CANDIDATE->video && CANDIDATE->windowed ? underlayCorners(PCANDIDATE, PLACEMENT->destination) : CBox{};
    const bool  ROUNDED       = !CANDIDATE->video || !SHAPE.empty();
    const float ROUND         = ROUNDED ? std::max(PCANDIDATE->presentation().rounding() * sc<float>(m_scale) - 1.F, 0.F) : 0.F;
    const float ROUNDINGPOWER = ROUNDED ? PCANDIDATE->presentation().roundingPower() : 2.F;
    if (ROUND != m_underlay.round || ROUNDINGPOWER != m_underlay.roundingPower || SHAPE != m_underlay.shape) {
        m_underlay.round         = ROUND;
        m_underlay.roundingPower = ROUNDINGPOWER;
        m_underlay.shape         = SHAPE;
        m_damage.damageEntire();
    }

    if (NEEDS_TEST) {
        if (m_drmFormat != ATTRS.format) {
            m_output->state->setFormat(ATTRS.format);
            m_drmFormat = ATTRS.format;
        }

        m_output->state->setBuffer(PVIDEO->m_current.buffer.m_buffer);
        m_output->state->setBufferPlacement(*PLACEMENT);
        m_output->state->resetExplicitFences();

        // the buffer the frame renders into next: the test needs its layout, not its contents
        const auto OVERLAY = m_underlay.swapchain->next(nullptr);
        m_underlay.swapchain->rollback();
        m_output->state->setOverlayBuffer(OVERLAY);

        if (!OVERLAY || !m_state.test()) {
            LOG(Log::DEBUG, "prepareUnderlay: the test failed for {} (format {}, modifier {:x})", m_name, NFormatUtils::drmFormatName(ATTRS.format), ATTRS.modifier);
            underlayFailed();
            return LEAVE();
        }

        // the hole follows the video's place: the overlay renders again
        m_damage.damageEntire();
    }

    if (ENTERING) {
        LOG(Log::DEBUG, "Entered an underlay ({}) to {:x}: \"{}\"", CANDIDATE->video ? "video" : "window content", rc<uintptr_t>(PCANDIDATE.get()), PCANDIDATE->metadata().title());

        // a video's composite takes its CM from the shader, not a fullscreen CTM, which would also hit the video; a window's
        // content and the composite take the CTM if it needs one. Set before the render
        m_previousFSWindow.reset();
        m_previousCTMWindow.reset();
        g_pHyprRenderer->handleFullscreenSettings(m_self.lock());
        m_damage.damageEntire();
    }

    return true;
}

bool CMonitor::commitUnderlay(std::optional<CDamageRing::CTransaction> damage) {
    const auto PCANDIDATE = m_underlay.window.lock();
    const auto PVIDEO     = m_underlay.surface.lock();

    // prepareUnderlay(), the render and this run in one go, with no client event between
    if (!PCANDIDATE || !PVIDEO || !PVIDEO->m_current.buffer) {
        LOG(Log::ERR, "commitUnderlay: the video went away during the frame");
        if (damage)
            damage->rollback();
        leaveUnderlay();
        scheduleFrame(Aquamarine::IOutput::AQ_SCHEDULE_RENDER_MONITOR);
        return false;
    }

    auto       PBUFFER = PVIDEO->m_current.buffer.m_buffer;
    const auto ATTRS   = PBUFFER->dmabuf();

    // the video on the primary, under the overlay the frame went into (endRender() set it, with its fence)
    if (m_drmFormat != ATTRS.format) {
        m_output->state->setFormat(ATTRS.format);
        m_drmFormat = ATTRS.format;
    }

    m_output->state->setBuffer(PBUFFER);
    m_output->state->setBufferPlacement(m_underlay.placement);
    m_output->state->resetExplicitFences(); // a client's committed buffer is ready to read

    // a placed buffer is not in output pixels: all of it
    m_output->state->clearDamage();
    m_output->state->addDamage(CBox{{}, PVIDEO->m_current.bufferSize});

    g_pHyprRenderer->handleFullscreenSettings(m_self.lock());

    // the video is not drawn, so its frame and feedback go out here, as in a direct scan-out
    PVIDEO->presentFeedback(Time::steadyNow(), m_self.lock());

    COutputCommitCoordinator::SFrame frame{
        .kind             = COutputCommitCoordinator::FRAME_UNDERLAY,
        .damage           = std::move(damage),
        .scanoutCandidate = PCANDIDATE,
        .vrr              = m_vrrActive,
    };

    if (m_commitCoordinator->submit(std::move(frame)) == COutputCommitCoordinator::SUBMIT_FAILED) {
        LOG(Log::TRACE, "commitUnderlay: the commit failed");
        return false;
    }

    // keep the client's buffer while KMS scans it out, as attemptDirectScanout()
    if (!PBUFFER->lockedByBackend || PBUFFER->m_hlEvents.backendRelease)
        return true;

    PBUFFER->lock();
    PBUFFER->onBackendRelease([wb = WP<IHLBuffer>{PBUFFER}] {
        if (wb)
            wb->unlock();
    });

    return true;
}

// a skipped underlay frame flips nothing, so no presentation paces the clients: answered at once, Firefox commits again
// at once (about 5,500 skipped frames a second, build 4). Their frame events and the video's feedback go out a refresh
// after the last ones or the last presentation, as a flip would send them
void CMonitor::scheduleUnderlayFrameEvents() {
    if (m_underlayFrameTimer && m_underlayFrameTimer->armed())
        return;

    const auto PERIOD = std::chrono::duration_cast<Time::steady_dur>(std::chrono::duration<double>(1.0 / std::max(sc<double>(m_refreshRate), 1.0)));
    const auto NOW    = Time::steadyNow();
    const auto DUE    = m_underlay.lastFrameEvents + PERIOD;

    if (!m_underlayFrameTimer) {
        m_underlayFrameTimer = makeShared<CEventLoopTimer>(
            std::nullopt,
            [self = m_self](SP<CEventLoopTimer>, void*) {
                if (const auto MON = self.lock())
                    MON->sendUnderlayFrameEvents();
            },
            nullptr);
        g_pEventLoopManager->addTimer(m_underlayFrameTimer);
    }

    m_underlayFrameTimer->updateTimeout(DUE > NOW ? DUE - NOW : Time::steady_dur{0});
}

void CMonitor::sendUnderlayFrameEvents() {
    const auto NOW             = Time::steadyNow();
    m_underlay.lastFrameEvents = NOW;

    const auto PVIDEO = m_underlay.surface.lock();
    if (PVIDEO && m_underlay.entered)
        PVIDEO->presentFeedback(NOW, m_self.lock());

    if (m_activeWorkspace)
        g_pHyprRenderer->sendFrameEventsToWorkspace(m_self.lock(), m_activeWorkspace, NOW);
    if (m_activeSpecialWorkspace)
        g_pHyprRenderer->sendFrameEventsToWorkspace(m_self.lock(), m_activeSpecialWorkspace, NOW);
}

void CMonitor::leaveUnderlay() {
    const bool OVERLAY_PENDING = m_output && (m_output->state->state().committed & Aquamarine::COutputState::AQ_OUTPUT_STATE_OVERLAY_BUFFER);
    if (!m_underlay.entered && !m_underlay.active && !OVERLAY_PENDING)
        return;

    // off with the next commit, the composite's on the primary, which draws what the overlay showed
    if (m_underlay.active || OVERLAY_PENDING)
        m_output->state->setOverlayBuffer(nullptr);

    const bool ENTERED = m_underlay.entered;

    // aquamarine keeps the buffer the overlay scans out until the plane goes off
    m_underlay = {};

    if (!ENTERED)
        return;

    LOG(Log::DEBUG, "Left a video underlay.");

    // the composite's format back, as handleDSleave()
    if (m_output->state->state().drmFormat != m_prevDrmFormat)
        m_output->state->setFormat(m_prevDrmFormat);

    m_drmFormat = m_prevDrmFormat;
    m_previousFSWindow.reset(); // recalc fs settings
    m_previousCTMWindow.reset();
    m_blurFBDirty = true;
    m_damage.damageEntire();
    m_state.updateSwapchain();
}

void CMonitor::underlayFailed() {
    m_underlayFailedOn = {.surface = m_underlay.surface, .format = m_underlay.format, .modifier = m_underlay.modifier, .placement = m_underlay.placement};
}

bool CMonitor::attemptDirectScanout() {
    static const auto PSAME     = CConfigValue<Config::INTEGER>("debug:ds_handle_same_buffer");
    static const auto PSAMEFIFO = CConfigValue<Config::INTEGER>("debug:ds_handle_same_buffer_fifo");

    const auto        blockedReason = isDSBlocked();
    if (blockedReason)
        return false;

    // a direct scan-out takes over from an underlay: the overlay goes off with its commit
    const bool LEFT_UNDERLAY = m_underlay.entered;
    leaveUnderlay();

    const auto PCANDIDATE = m_solitaryClient.lock();
    const auto PSOLITARY  = PCANDIDATE->getSolitaryResource();
    const auto PSURFACE   = PSOLITARY ? PSOLITARY : PCANDIDATE->getVideoResource();
    const auto PLACEMENT  = PSOLITARY ? Aquamarine::COutputState::SBufferPlacement{} : videoPlacement(PCANDIDATE, PSURFACE).value_or(Aquamarine::COutputState::SBufferPlacement{});
    auto       PBUFFER    = PSURFACE->m_current.buffer.m_buffer;

    // #TODO this entire bit needs figuring out, vrr goes down the drain without it
    if (!LEFT_UNDERLAY && PBUFFER == m_output->state->state().buffer && PLACEMENT == m_output->state->state().bufferPlacement && *PSAME) {
        PSURFACE->presentFeedback(Time::steadyNow(), m_self.lock());
        if (!PSOLITARY) // a video client may wait for frames on its main surface
            PCANDIDATE->wlSurface()->resource()->frame(Time::steadyNow());
        PROTO::presentation->tagQueued(m_self.lock(), 0, m_tearingState.activelyTearing, m_vrrActive);

        if (m_scanoutNeedsCursorUpdate) {
            if (!m_state.test()) {
                LOG(Log::TRACE, "attemptDirectScanout: failed basic test on cursor update");
                return false;
            }

            if (!m_output->commit()) {
                LOG(Log::TRACE, "attemptDirectScanout: failed to commit cursor update");
                m_lastScanout.reset();
                return false;
            }

            m_scanoutNeedsCursorUpdate = false;
        }

        //#TODO this entire bit is bootleg deluxe, above bit is to not make vrr go down the drain, returning early here means fifo gets forever locked.
        if (PSURFACE->m_fifo && !m_tearingState.activelyTearing && *PSAMEFIFO)
            PSURFACE->m_stateQueue.unlockFirst(LOCK_REASON_FIFO);

        return true;
    }

    const auto params = PSURFACE->m_current.buffer->dmabuf();

    LOG(Log::TRACE, "attemptDirectScanout: surface {:x} passed, will attempt, buffer {} fmt: {} -> {} (mod {})", rc<uintptr_t>(PSURFACE.get()),
        rc<uintptr_t>(PSURFACE->m_current.buffer.m_buffer.get()), m_drmFormat, params.format, params.modifier);

    // FIXME: make sure the buffer comes from the appropriate device (its
    // layout is checked against the primary plane's). This may implode on multi-gpu!!

    // entering into scanout, so save monitor format
    if (m_lastScanout.expired())
        m_prevDrmFormat = m_drmFormat;

    const auto  previousFormat           = m_drmFormat;
    const auto  previousBuffer           = m_output->state->state().buffer;
    const auto  previousPlacement        = m_output->state->state().bufferPlacement;
    const auto  previousPresentationMode = m_output->state->state().presentationMode;
    bool        scanoutCommitted         = false;
    CScopeGuard rollbackState            = {[this, previousFormat, previousBuffer, previousPlacement, previousPresentationMode, &scanoutCommitted]() {
        if (scanoutCommitted)
            return;

        m_drmFormat = previousFormat;
        m_output->state->setFormat(previousFormat);
        m_output->state->setBuffer(previousBuffer);
        m_output->state->setBufferPlacement(previousPlacement);
        m_output->state->setPresentationMode(previousPresentationMode);
        m_output->state->resetExplicitFences();
    }};

    // do not retest while it's active, unless a video moved or scaled
    const bool NEEDS_TEST = !m_lastScanout || m_drmFormat != params.format || PLACEMENT != previousPlacement;
    if (m_drmFormat != params.format) {
        m_output->state->setFormat(params.format);
        m_drmFormat = params.format;
    }

    m_output->state->setBuffer(PBUFFER);
    m_output->state->setBufferPlacement(PLACEMENT);
    LOG(Log::TRACE, "attemptDirectScanout: setting presentation mode");
    m_output->state->setPresentationMode(m_tearingState.activelyTearing ? Aquamarine::eOutputPresentationMode::AQ_OUTPUT_PRESENTATION_IMMEDIATE :
                                                                          Aquamarine::eOutputPresentationMode::AQ_OUTPUT_PRESENTATION_VSYNC);

    if (NEEDS_TEST && !m_state.test()) {
        LOG(Log::TRACE, "attemptDirectScanout: failed basic test");
        m_dsFailedOn = {.surface = PSURFACE, .format = params.format, .modifier = params.modifier, .placement = PLACEMENT};
        return false;
    }

    PSURFACE->presentFeedback(Time::steadyNow(), m_self.lock());
    if (!PSOLITARY) // a video client may wait for frames on its main surface
        PCANDIDATE->wlSurface()->resource()->frame(Time::steadyNow());

    auto    damageTransaction = m_damage.beginTransaction();
    CRegion scanoutDamage     = damageTransaction.getBufferDamage(1);
    // the ring is in transformed space, the fb we hand to KMS is in pixel space
    scanoutDamage.transform(Math::wlTransformToHyprutils(Math::invertTransform(m_transform)), m_transformedSize.x, m_transformedSize.y);
    // expand to not miss pixels from rounding, being a bit over is safe, going below is stale pixels.
    scanoutDamage.expand(1).intersect(CBox{{}, m_pixelSize});
    // a placed buffer is not in output pixels: all of it
    if (PLACEMENT != Aquamarine::COutputState::SBufferPlacement{})
        scanoutDamage = CBox{{}, PSURFACE->m_current.bufferSize};
    m_output->state->addDamage(scanoutDamage);

    // multigpu needs a fence to trigger fence syncing blits and also committing with the recreated dgpu fence
    if (g_pHyprRenderer->explicitSyncSupported() && isMultiGPU()) {
        auto sync = g_pHyprRenderer->createSyncFDManager();

        if (sync && sync->isValid()) {
            m_inFence = sync->takeFd();
            m_output->state->setExplicitInFence(m_inFence.get());
        } else {
            m_inFence.reset();
            m_output->state->resetExplicitFences(); // good luck.
        }
    } else
        m_output->state->resetExplicitFences();

    // no need to do explicit sync here as surface current can only ever be ready to read

    COutputCommitCoordinator::SFrame frame{
        .kind              = COutputCommitCoordinator::FRAME_DIRECT_SCANOUT,
        .damage            = std::move(damageTransaction),
        .scanoutCandidate  = PCANDIDATE,
        .rollbackSwapchain = false,
        .tearing           = m_tearingState.activelyTearing,
        .vrr               = m_vrrActive,
        .previousFormat    = previousFormat,
    };

    const auto result = m_commitCoordinator->submit(std::move(frame));
    if (result == COutputCommitCoordinator::SUBMIT_FAILED) {
        LOG(Log::TRACE, "attemptDirectScanout: failed to scanout surface");
        m_lastScanout.reset();
        return false;
    }

    scanoutCommitted = true;

    m_scanoutNeedsCursorUpdate = false;

    if (!PBUFFER->lockedByBackend || PBUFFER->m_hlEvents.backendRelease)
        return true;

    // lock buffer while DRM/KMS is using it, then release it when page flip happens since DRM/KMS should be done by then
    // btw buffer's syncReleaser will take care of signaling release point, so we don't do that here
    PBUFFER->lock();
    PBUFFER->onBackendRelease([wb = WP<IHLBuffer>{PBUFFER}] {
        if (wb)
            wb->unlock();
    });

    return true;
}

void CMonitor::handleDSleave() {
    LOG(Log::DEBUG, "Left a direct scanout.");
    m_lastScanout.reset();
    m_previousFSWindow.reset(); // recalc fs settings
    m_previousCTMWindow.reset();
    m_directScanoutIsActive = false;

    // reset DRM format, but only if needed since it might modeset
    if (m_output->state->state().drmFormat != m_prevDrmFormat)
        m_output->state->setFormat(m_prevDrmFormat);

    m_drmFormat   = m_prevDrmFormat;
    m_blurFBDirty = true;
    m_damage.damageEntire();

    // if swapchain in DS is an 8-bit format whilst we are configured for 10-bit, we may see a visible flicker
    // when leaving DS. Update the swapchain format here before any further rendering to avoid that
    m_state.updateSwapchain();
}

bool CMonitor::primaryTakes(uint32_t format, uint64_t modifier) {
    if (m_primaryTakesCache.format == format && m_primaryTakesCache.modifier == modifier)
        return m_primaryTakesCache.takes;

    bool takes = false;
    for (const auto& f : m_output->getRenderFormats()) {
        if (f.drmFormat != format)
            continue;

        // aquamarine lists INVALID for a plane that takes implicit buffers, so an
        // implicit buffer matches it and an explicit one needs its own modifier
        takes = std::ranges::contains(f.modifiers, modifier);
        break;
    }

    m_primaryTakesCache = {.format = format, .modifier = modifier, .takes = takes};
    return takes;
}

bool CMonitor::canAttemptDirectScanoutFast() const {
    return !m_solitaryClient.expired() || !m_lastScanout.expired() || m_directScanoutIsActive;
}

bool CMonitor::isMultiGPU() {
    if (!m_output || !g_pCompositor)
        return false;

    const auto PREFERREDALLOCATOR = m_output->getBackend()->preferredAllocator();
    const int  allocatorFD        = PREFERREDALLOCATOR ? PREFERREDALLOCATOR->drmFD() : -1;
    const int  compositorFD       = g_pCompositor->m_drm.fd;

    if (allocatorFD < 0 || compositorFD < 0) {
        m_cachedAllocatorDRMDev.reset();
        m_cachedCompositorDRMDev.reset();
        m_cachedAllocatorDRMFD  = allocatorFD;
        m_cachedCompositorDRMFD = compositorFD;
        m_cachedSameGPU         = true;
        return false;
    }

    const auto allocatorDev  = DRM::devIDFromFD(allocatorFD);
    const auto compositorDev = DRM::devIDFromFD(compositorFD);

    // AQ can reopen DRM nodes for refcounting, so raw fd numbers are not a stable cache key.
    const bool useDeviceIDCache = allocatorDev.has_value() && compositorDev.has_value();
    const bool cacheStale       = !m_cachedSameGPU ||
        (useDeviceIDCache ? m_cachedAllocatorDRMDev != allocatorDev || m_cachedCompositorDRMDev != compositorDev :
                            m_cachedAllocatorDRMFD != allocatorFD || m_cachedCompositorDRMFD != compositorFD);

    if (cacheStale) {
        m_cachedAllocatorDRMDev  = allocatorDev;
        m_cachedCompositorDRMDev = compositorDev;
        m_cachedAllocatorDRMFD   = allocatorFD;
        m_cachedCompositorDRMFD  = compositorFD;
        m_cachedSameGPU          = DRM::sameGpu(allocatorFD, compositorFD);
    }

    return !*m_cachedSameGPU;
}

bool CMonitor::shouldUseSoftwareCursors() {
    static auto PNOHW      = CConfigValue<Config::INTEGER>("cursor:no_hardware_cursors");
    static auto PINVISIBLE = CConfigValue<Config::INTEGER>("cursor:invisible");

    if (m_tearingState.activelyTearing)
        return true;

    if (*PINVISIBLE != 0)
        return true;

    switch (*PNOHW) {
        case 0: return false;
        case 1: return true;
        case 2: return g_pHyprRenderer->isNvidia() && (g_pHyprRenderer->isMgpu() || State::monitorLayoutController()->isVRRActiveOnAnyMonitor());
        default: break;
    }

    return true;
}

void CMonitor::setDPMS(bool on) {
    // Don't trigger animation if the target state is the same
    if (m_dpmsStatus == on)
        return;

    m_dpmsStatus = on;
    m_events.dpmsChanged.emit();

    if (on) {
        // enable the monitor. Wait for the frame to be presented, then begin animation
        m_dpmsBlackOpacity->setCallbackOnEnd(nullptr);
        m_dpmsBlackOpacity->setValueAndWarp(1.F);
        m_pendingDpmsAnimation        = true;
        m_pendingDpmsAnimationCounter = 0;
        commitDPMSState(true);
    } else {
        // disable the monitor. Begin the animation, then do dpms on its end.
        m_dpmsBlackOpacity->setCallbackOnEnd(nullptr);
        m_dpmsBlackOpacity->setValueAndWarp(0.F);
        *m_dpmsBlackOpacity = 1.F;
        m_dpmsBlackOpacity->setCallbackOnEnd(
            [this, self = m_self](auto) {
                if (!self)
                    return;

                // commit DPMS to disable the monitor, it's fully black now
                commitDPMSState(false);
            },
            true);
    }
}

void CMonitor::commitDPMSState(bool state) {
    m_output->state->resetExplicitFences();
    m_output->state->setEnabled(state);

    if (!m_state.commit()) {
        LOG(Log::ERR, "Couldn't commit output {} for DPMS = {}, will retry.", m_name, state);

        // retry in 2 frames. This could happen when the DRM backend rejects our commit
        // because disable + enable were sent almost instantly

        m_dpmsRetryTimer = makeShared<CEventLoopTimer>(
            std::chrono::milliseconds(2000 / sc<int>(m_refreshRate)),
            [this, self = m_self](SP<CEventLoopTimer> s, void* d) {
                if (!self)
                    return;

                m_output->state->resetExplicitFences();
                m_output->state->setEnabled(m_dpmsStatus);

                if (!m_state.commit()) {
                    LOG(Log::ERR, "Couldn't retry committing output {} for DPMS = {}", m_name, m_dpmsStatus);
                    return;
                }

                m_dpmsRetryTimer.reset();
            },
            nullptr);
        g_pEventLoopManager->addTimer(m_dpmsRetryTimer);

        return;
    }

    if (state)
        g_pHyprRenderer->damageMonitor(m_self.lock());
}

void CMonitor::debugLastPresentation(const std::string& message) {
    LOG(Log::TRACE, "{} (last presentation {} - {} fps)", message, m_lastPresentationTimer.getMillis(),
        m_lastPresentationTimer.getMillis() > 0 ? 1000.0f / m_lastPresentationTimer.getMillis() : 0.0f);
}

void CMonitor::onCursorMovedOnMonitor() {
    if (!m_tearingState.activelyTearing || !m_solitaryClient || !g_pHyprRenderer->shouldRenderCursor())
        return;

    // submit a frame immediately. This will only update the cursor pos.
    // output->state->setBuffer(output->state->state().buffer);
    // output->state->addDamage(CRegion{});
    // output->state->setPresentationMode(Aquamarine::eOutputPresentationMode::AQ_OUTPUT_PRESENTATION_IMMEDIATE);
    // if (!output->commit())
    //     LOG(Log::ERR, "onCursorMovedOnMonitor: tearing and wanted to update cursor, failed.");

    // FIXME: try to do the above. We currently can't just render because drm is a fucking bitch
    // and throws a "nO pRoP cAn Be ChAnGeD dUrInG AsYnC fLiP" on crtc_x
    // this will throw too but fix it if we use sw cursors

    m_tearingState.frameScheduledWhileBusy = true;
}

bool CMonitor::supportsWideColor() {
    switch (m_supportsWideColor) {
        case -1: return false;
        case 1: return true;
        default: return m_output->parsedEDID.supportsBT2020;
    }
}

bool CMonitor::supportsHDR() {
    if (!supportsWideColor())
        return false;

    switch (m_supportsHDR) {
        case -1: return false;
        case 1: return true;
        default: return m_output->parsedEDID.hdrMetadata.has_value() ? m_output->parsedEDID.hdrMetadata->supportsPQ : false;
    }
}

float CMonitor::minLuminance(float defaultValue) {
    return m_minLuminance >= 0 ? m_minLuminance : (m_output->parsedEDID.hdrMetadata.has_value() ? m_output->parsedEDID.hdrMetadata->desiredContentMinLuminance : defaultValue);
}

int CMonitor::maxLuminance(int defaultValue) {
    return m_maxLuminance >= 0 ? m_maxLuminance : (m_output->parsedEDID.hdrMetadata.has_value() ? m_output->parsedEDID.hdrMetadata->desiredContentMaxLuminance : defaultValue);
}

int CMonitor::maxAvgLuminance(int defaultValue) {
    return m_maxAvgLuminance >= 0 ? m_maxAvgLuminance :
                                    (m_output->parsedEDID.hdrMetadata.has_value() ? m_output->parsedEDID.hdrMetadata->desiredMaxFrameAverageLuminance : defaultValue);
}

float CMonitor::maxFALL() {
    return m_maxAvgLuminance >= 0 ? m_maxAvgLuminance : (m_output->parsedEDID.hdrMetadata.has_value() ? m_output->parsedEDID.hdrMetadata->desiredMaxFrameAverageLuminance : 0);
}

float CMonitor::maxCLL() {
    return m_maxLuminance >= 0 ? m_maxLuminance : (m_output->parsedEDID.hdrMetadata.has_value() ? m_output->parsedEDID.hdrMetadata->desiredContentMaxLuminance : 0);
}

// a headroom change ramps over 2 s whatever its size, up or down, geometrically in headroom; a rise starts at once and
// cancels a pending drop, a drop waits 15 s, so content that comes and goes holds the headroom
static constexpr float HDR_HEADROOM_RAMP = 2.F;  // s
static constexpr float HDR_HEADROOM_HOLD = 15.F; // s
static constexpr float HDR_HEADROOM_MAX  = 16.F; // the panel's potential

bool                   CMonitor::hdrHeadroomApplies() {
    static auto PHEADROOM = CConfigValue<Config::INTEGER>("render:cm_hdr_headroom");
    return *PHEADROOM == 2 || (*PHEADROOM == 1 && m_name.starts_with("eDP"));
}

void CMonitor::hdrHeadroomRequest(const SImageDescription& desc) {
    const bool HDR = desc.transferFunction == CM_TRANSFER_FUNCTION_ST2084_PQ || desc.transferFunction == CM_TRANSFER_FUNCTION_HLG ||
        (desc.transferFunction == CM_TRANSFER_FUNCTION_EXT_LINEAR && desc.primariesNamed == CM_PRIMARIES_SRGB);
    if (!HDR)
        return;

    // the content's own peak, else its mastering display's, else all the panel has
    m_hdrHeadroom.request = std::max(m_hdrHeadroom.request, desc.getContentMaxLuminance(maxLuminance(HDR_MAX_LUMINANCE)));
}

// the request is the largest the HDR content shown on the monitor asks for, over its visible layers:
// from the surfaces of the windows that render here and of its layers, whether or not a frame redraws them, so content
// that stays still keeps its headroom and content gone lets it go with the next frame
void CMonitor::hdrHeadroomFrame() {
    m_hdrHeadroom.request = 0;
    if (!hdrHeadroomApplies() || !inHDR()) {
        m_hdrHeadroom = {};
        return;
    }

    const auto ASK = [this](SP<CWLSurfaceResource> s, const Vector2D&, void*) {
        if (s && s->m_current.texture && s->m_colorManagement.valid() && s->m_colorManagement->hasImageDescription())
            hdrHeadroomRequest(s->m_colorManagement->imageDescription());
    };

    const auto SELF = m_self.lock();
    for (auto const& w : Desktop::windowState()->windows()) {
        if (!w->mapped() || !g_pHyprRenderer->shouldRenderWindow(w, SELF))
            continue;
        if (const auto SURF = w->wlSurface(); SURF && SURF->resource())
            SURF->resource()->breadthfirst(ASK, nullptr);
    }

    for (auto const& layer : m_layerSurfaceLayers) {
        for (auto const& ref : layer) {
            const auto LS = ref.lock();
            if (LS && LS->mapped() && LS->wlSurface() && LS->wlSurface()->resource())
                LS->wlSurface()->resource()->breadthfirst(ASK, nullptr);
        }
    }

    hdrHeadroomStep();
}

bool CMonitor::hdrHeadroomPending() {
    return hdrHeadroomApplies() && inHDR() && m_hdrHeadroom.current != m_hdrHeadroom.target;
}

// the headroom's ramp, advanced at the frame's start: a rise, or the same, at once; a drop (the content gone, or asking
// less) after the hold, a timer waking the frame that starts it. A step damages the monitor, so the frame renders and
// sends its MaxCLL even with nothing else changed, and frames follow until the ramp ends
void CMonitor::hdrHeadroomStep() {
    const auto  NOW     = Time::steadyNow();
    const float WHITE   = std::max(m_sdrMaxLuminance, 1);
    const float REQUEST = m_hdrHeadroom.request;
    const float PANEL   = maxCLL() > 0 ? maxCLL() : maxLuminance(HDR_MAX_LUMINANCE);

    const auto  rampAt = [this](const Time::steady_tp& t) {
        const float ELAPSED = std::chrono::duration<float>(t - m_hdrHeadroom.start).count();
        if (m_hdrHeadroom.duration <= 0 || ELAPSED >= m_hdrHeadroom.duration)
            return m_hdrHeadroom.target;
        return m_hdrHeadroom.origin * std::pow(m_hdrHeadroom.target / m_hdrHeadroom.origin, std::max(ELAPSED, 0.F) / m_hdrHeadroom.duration);
    };

    const float WANT   = REQUEST > 0 ? std::clamp(std::min(REQUEST, PANEL) / WHITE, 1.F, HDR_HEADROOM_MAX) : 1.F;
    float       target = WANT;
    if (WANT >= m_hdrHeadroom.target)
        m_hdrHeadroom.lastSeen = NOW;
    else {
        const float SINCE = std::chrono::duration<float>(NOW - m_hdrHeadroom.lastSeen).count();
        if (SINCE < HDR_HEADROOM_HOLD) {
            target = m_hdrHeadroom.target;
            if (!m_hdrHeadroomTimer) {
                m_hdrHeadroomTimer = makeShared<CEventLoopTimer>(
                    std::nullopt,
                    [self = m_self](SP<CEventLoopTimer>, void*) {
                        if (const auto MON = self.lock())
                            MON->scheduleFrame(Aquamarine::IOutput::AQ_SCHEDULE_ANIMATION);
                    },
                    nullptr);
                g_pEventLoopManager->addTimer(m_hdrHeadroomTimer);
            }
            m_hdrHeadroomTimer->updateTimeout(std::chrono::duration_cast<Time::steady_dur>(std::chrono::duration<float>(HDR_HEADROOM_HOLD - SINCE)));
        }
    }

    const float NOW_HEADROOM = rampAt(NOW);
    if (target != m_hdrHeadroom.target) {
        m_hdrHeadroom.origin   = NOW_HEADROOM;
        m_hdrHeadroom.target   = target;
        m_hdrHeadroom.start    = NOW;
        m_hdrHeadroom.duration = target != NOW_HEADROOM ? HDR_HEADROOM_RAMP : 0.F;
    }

    // as an animation: every frame of the ramp, its first (still the old value) included, damages the monitor, so each
    // commits and its flip brings the next; a frame with nothing damaged commits nothing and the ramp would stop
    const float CURRENT = rampAt(NOW);
    if (CURRENT != m_hdrHeadroom.current || CURRENT != m_hdrHeadroom.target) {
        m_hdrHeadroom.current = CURRENT;
        m_damage.damageEntire();
    }

    if (m_hdrHeadroom.current != m_hdrHeadroom.target)
        scheduleFrame(Aquamarine::IOutput::AQ_SCHEDULE_ANIMATION);
}

uint16_t CMonitor::hdrHeadroomMaxCLL() {
    return sc<uint16_t>(std::round(std::min(m_hdrHeadroom.current * std::max(m_sdrMaxLuminance, 1), 65535.F)));
}

bool CMonitor::wantsWideColor() {
    return supportsWideColor() && (wantsHDR() || m_imageDescription->value().primariesNamed == CM_PRIMARIES_BT2020);
}

bool CMonitor::wantsHDR() {
    return supportsHDR() && inHDR();
}

bool CMonitor::inHDR() {
    return m_output->state->state().hdrMetadata.hdmi_metadata_type1.eotf == 2;
}

std::optional<NColorManagement::PImageDescription> CMonitor::getFSImageDescription() {

    const auto FS_WINDOW = Fullscreen::controller()->getFullscreenWindow(m_self.lock());

    // a window's content under the composite (render:underlay 2) reaches the DCP as is, as a fullscreen window's would
    if (!FS_WINDOW && m_underlay.entered && !m_underlay.video) {
        if (const auto CONTENT = m_underlay.surface.lock())
            return underlayContentDescription(m_underlay.window.lock(), CONTENT);
    }

    if (!FS_WINDOW)
        return {};

    const auto ROOT_SURF = FS_WINDOW->wlSurface()->resource();
    const auto SURF      = ROOT_SURF->findWithCM();
    return SURF ? NColorManagement::CImageDescription::from(SURF->m_colorManagement->imageDescription()) : getDefaultImageDescription();
}

// the HDR description auto HDR would switch to, offered to the fullscreen surface so it can trigger the switch
std::optional<NColorManagement::PImageDescription> CMonitor::getAutoHDRImageDescription(const SP<CWLSurfaceResource>& surface) {
    static const auto PAUTOHDR = CConfigValue<Config::INTEGER>("render:cm_auto_hdr");

    if (!*PAUTOHDR || !supportsHDR() || !m_activeMonitorRule.m_iccFile.empty() || m_cmType == NCMType::CM_HDR || m_cmType == NCMType::CM_HDR_EDID)
        return {};

    const auto FS_WINDOW = Fullscreen::controller()->getFullscreenWindow(m_self.lock());
    if (!FS_WINDOW || FS_WINDOW->wlSurface()->resource() != surface || FS_WINDOW->m_ruleApplicator->noAutoHDR().valueOrDefault())
        return {};

    return getHDRImageDescription(*PAUTOHDR == 2 ? NCMType::CM_HDR_EDID : NCMType::CM_HDR);
}

NColorManagement::SPCPRimaries CMonitor::getMasteringPrimaries() {
    return m_output->parsedEDID.chromaticityCoords.has_value() ?
        NColorManagement::SPCPRimaries{
            .red   = {.x = m_output->parsedEDID.chromaticityCoords->red.x, .y = m_output->parsedEDID.chromaticityCoords->red.y},
            .green = {.x = m_output->parsedEDID.chromaticityCoords->green.x, .y = m_output->parsedEDID.chromaticityCoords->green.y},
            .blue  = {.x = m_output->parsedEDID.chromaticityCoords->blue.x, .y = m_output->parsedEDID.chromaticityCoords->blue.y},
            .white = {.x = m_output->parsedEDID.chromaticityCoords->white.x, .y = m_output->parsedEDID.chromaticityCoords->white.y},
        } :
        NColorManagement::SPCPRimaries{};
}

NColorManagement::SImageDescription::SPCMasteringLuminances CMonitor::getMasteringLuminances() {
    return {
        .min = m_minLuminance >= 0 ? m_minLuminance : (m_output->parsedEDID.hdrMetadata.has_value() ? m_output->parsedEDID.hdrMetadata->desiredContentMinLuminance : 0),
        .max = m_maxLuminance >= 0 ? m_maxLuminance : (m_output->parsedEDID.hdrMetadata.has_value() ? m_output->parsedEDID.hdrMetadata->desiredContentMaxLuminance : 0),
    };
}

NColorManagement::PImageDescription CMonitor::getHDRImageDescription(NCMType::eCMType cmType) {
    const auto  masteringPrimaries = getMasteringPrimaries();
    const auto& defaultHDR         = DEFAULT_HDR_IMAGE_DESCRIPTION->value();
    const bool  edidPrimaries      = cmType == NCMType::CM_HDR_EDID;
    const auto  primaries =
        edidPrimaries && m_output->parsedEDID.chromaticityCoords.has_value() ? masteringPrimaries : NColorManagement::getPrimaries(NColorManagement::CM_PRIMARIES_BT2020);

    return CImageDescription::from({
        .transferFunction    = NColorManagement::CM_TRANSFER_FUNCTION_ST2084_PQ,
        .primariesNameSet    = !edidPrimaries,
        .primariesNamed      = NColorManagement::CM_PRIMARIES_BT2020,
        .primaries           = primaries,
        .masteringPrimaries  = masteringPrimaries,
        .luminances          = {.min       = minLuminance() >= 0 ? minLuminance() : defaultHDR.getTFMinLuminance(),
                                .max       = maxLuminance() > 0 ? sc<uint32_t>(maxLuminance()) : sc<uint32_t>(defaultHDR.getTFMaxLuminance()),
                                .reference = sc<uint32_t>(defaultHDR.getTFRefLuminance())},
        .masteringLuminances = getMasteringLuminances(),
        .maxCLL              = maxCLL(),
        .maxFALL             = maxFALL(),
    });
}

uint32_t CMonitor::getPreferredReadFormat() {
    static const auto PFORCE8BIT = CConfigValue<Config::INTEGER>("misc:screencopy_force_8b");

    auto              monFmt = m_output->state->state().drmFormat;

    if (*PFORCE8BIT)
        if (monFmt == DRM_FORMAT_BGRA1010102 || monFmt == DRM_FORMAT_ARGB2101010 || monFmt == DRM_FORMAT_XRGB2101010 || monFmt == DRM_FORMAT_BGRX1010102 ||
            monFmt == DRM_FORMAT_XBGR2101010)
            monFmt = DRM_FORMAT_XRGB8888;

    return monFmt;
}

bool CMonitor::needsCM() {
    const auto SRC_DESC = getFSImageDescription();
    return SRC_DESC.has_value() && SRC_DESC.value() != m_imageDescription;
}

static bool isCompatibleTF(eTransferFunction sourceTF, eTransferFunction targetTF) {
    static auto PNONSHADER = CConfigValue<Config::INTEGER>("render:non_shader_cm");
    const auto  sdrEOTF    = NTransferFunction::fromConfig();
    return sourceTF == targetTF                                                                                                         // same
        || (sdrEOTF == NTransferFunction::TF_FORCED_GAMMA22 && sourceTF == NColorManagement::CM_TRANSFER_FUNCTION_SRGB                  // forced source gamma22 to output gamma22
            && targetTF == NColorManagement::CM_TRANSFER_FUNCTION_GAMMA22)                                                              //
        || (*PNONSHADER == CM_NS_ONDEMAND                                                                                               // FIXME incorrect but good enough for DS
            && (sourceTF == NColorManagement::CM_TRANSFER_FUNCTION_GAMMA22 || sourceTF == NColorManagement::CM_TRANSFER_FUNCTION_SRGB)  //
            && (targetTF == NColorManagement::CM_TRANSFER_FUNCTION_GAMMA22 || targetTF == NColorManagement::CM_TRANSFER_FUNCTION_SRGB)) //
        ;
}

// TODO support more drm properties
bool CMonitor::canNoShaderCM(bool forDSmode) {
    const auto SRC_DESC = getFSImageDescription();
    return SRC_DESC.has_value() && canNoShaderCM(SRC_DESC.value(), forDSmode);
}

bool CMonitor::canNoShaderCM(const PImageDescription& source, bool forDSmode) {
    static auto PNONSHADER = CConfigValue<Config::INTEGER>("render:non_shader_cm");
    if (*PNONSHADER == CM_NS_DISABLE)
        return false;

    const auto& DST_DESC = forDSmode ? m_imageDescription : resources()->m_imageDescription;
    if (source == DST_DESC)
        return true; // no CM needed

    const auto& SRC_DESC_VALUE = source->value();

    if (m_imageDescription->value().icc.present)
        return false;

    LOG(Log::TRACE, "CM: can no shder compares src={} to output={}", SRC_DESC_VALUE, m_imageDescription->value());

    // only primaries differ
    return (
        isCompatibleTF(SRC_DESC_VALUE.transferFunction, DST_DESC->value().transferFunction) //
        && SRC_DESC_VALUE.transferFunctionPower == DST_DESC->value().transferFunctionPower  //
        && SRC_DESC_VALUE.getTFMinLuminance() == DST_DESC->value().getTFMinLuminance()      //
        && SRC_DESC_VALUE.getTFMaxLuminance() == DST_DESC->value().getTFMaxLuminance()      //
        && (!inHDR() || SRC_DESC_VALUE.luminances == DST_DESC->value().luminances)
        // not used by shaders atm
        // && SRC_DESC_VALUE.masteringLuminances == m_imageDescription->value().masteringLuminances && SRC_DESC_VALUE.maxCLL == m_imageDescription->value().maxCLL && SRC_DESC_VALUE.maxFALL == m_imageDescription->value().maxFALL
    );
}

bool CMonitor::doesNoShaderCM() {
    return m_noShaderCTM;
}

static std::vector<uint16_t> resampleInterleavedToKms(const SVCGTTable16& t, size_t gammaSize) {
    const size_t entries = sc<size_t>(t.entries);

    if (gammaSize == 0 || entries == 0)
        return {};

    if (t.ch[0].size() < entries || t.ch[1].size() < entries || t.ch[2].size() < entries)
        return {};

    std::vector<uint16_t> out(gammaSize * 3);

    // No resampling is needed when the VCGT already matches the KMS LUT size.
    if (gammaSize == entries) {
        const uint16_t* __restrict red   = t.ch[0].data();
        const uint16_t* __restrict green = t.ch[1].data();
        const uint16_t* __restrict blue  = t.ch[2].data();
        uint16_t* __restrict dst         = out.data();

        for (size_t i = 0; i < gammaSize; ++i) {
            const size_t dstIndex = i * 3;

            dst[dstIndex + 0] = red[i];
            dst[dstIndex + 1] = green[i];
            dst[dstIndex + 2] = blue[i];
        }

        return out;
    }

    auto sample = [&](int c, float x) -> uint16_t {
        const float maxX = t.entries - 1;
        x                = std::clamp(x, 0.F, maxX);

        const size_t i0 = sc<size_t>(std::floor(x));
        const size_t i1 = std::min(i0 + 1, sc<size_t>(t.entries - 1));
        const float  f  = x - sc<float>(i0);

        const float  v0 = sc<float>(t.ch[c][i0]);
        const float  v1 = sc<float>(t.ch[c][i1]);
        const float  v  = v0 + ((v1 - v0) * f);

        int64_t      vi = std::round(v);
        vi              = std::clamp(vi, sc<int64_t>(0), sc<int64_t>(65535));

        return sc<uint16_t>(vi);
    };

    for (size_t i = 0; i < gammaSize; ++i) {
        const float x = sc<float>(i) * sc<float>(t.entries - 1) / sc<float>(gammaSize - 1);

        out[i * 3 + 0] = sample(0, x);
        out[i * 3 + 1] = sample(1, x);
        out[i * 3 + 2] = sample(2, x);
    }

    return out;
}

void CMonitor::updateVCGTRamps() {
    auto gammaSize = m_output->getGammaSize();

    if (gammaSize <= 10) {
        LOG(Log::DEBUG, "CMonitor::updateVCGTRamps: skipping, no gamma ramp for output");
        return;
    }

    if (!m_imageDescription->value().icc.vcgt) {
        if (m_vcgtRampsSet)
            m_output->state->setGammaLut({});

        m_vcgtRampsSet = false;
        return;
    }

    // build table
    auto table = resampleInterleavedToKms(*m_imageDescription->value().icc.vcgt, gammaSize);

    m_output->state->setGammaLut(table);

    m_vcgtRampsSet = true;
}

bool CMonitor::gammaRampsInUse() {
    return m_vcgtRampsSet;
}

CMonitorState::CMonitorState(CMonitor* owner) : m_owner(owner) {
    ;
}

void CMonitorState::ensureBufferPresent() {
    const auto& STATE = m_owner->m_output->state->state();
    if (!STATE.enabled) {
        LOG(Log::TRACE, "CMonitorState::ensureBufferPresent: Ignoring, monitor is not enabled");
        return;
    }

    if (STATE.buffer) {
        if (const auto params = STATE.buffer->dmabuf(); params.success && params.format == m_owner->m_drmFormat)
            return;
    }

    // this is required for modesetting being possible and might be missing in case of first tests in the renderer
    // where we test modes and buffers
    LOG(Log::DEBUG, "CMonitorState::ensureBufferPresent: no buffer or mismatched format, attaching one from the swapchain for modeset being possible");
    m_owner->m_output->state->setBuffer(m_owner->m_output->swapchain->next(nullptr));
    m_owner->m_output->swapchain->rollback(); // restore the counter, don't advance the swapchain
}

bool CMonitorState::commit() {
    if (m_owner->m_commitCoordinator->deferStateCommit())
        return true;

    if (!updateSwapchain())
        return false;

    Event::bus()->m_events.monitor.preCommit.emit(m_owner->m_self.lock());

    ensureBufferPresent();

    bool ret = m_owner->m_output->commit();
    return ret;
}

bool CMonitorState::test() {
    if (!updateSwapchain())
        return false;

    ensureBufferPresent();

    return m_owner->m_output->test();
}

bool CMonitorState::updateSwapchain() {
    if (!m_owner->m_output)
        return false;

    const auto& OPTIONS = m_owner->m_output->swapchain->currentOptions();
    const auto& STATE   = m_owner->m_output->state->state();
    const auto& MODE    = STATE.mode ? STATE.mode : STATE.customMode;
    if (!MODE) {
        LOG(Log::WARN, "updateSwapchain: No mode?");
        return true;
    }

    // the swapchain is what the compositor renders into: a buffer scanned out in a YUV format (the monitor's format while
    // it is) leaves it in the format it renders in, which, unlike the YUV one, it can allocate at the mode's size
    const auto FORMAT = NFormatUtils::isFormatYUV(m_owner->m_drmFormat) && OPTIONS.format != DRM_FORMAT_INVALID ? OPTIONS.format : m_owner->m_drmFormat;
    if (OPTIONS.format == FORMAT && OPTIONS.scanout && OPTIONS.length == 3 && OPTIONS.size == MODE->pixelSize)
        return true;

    auto options    = OPTIONS;
    options.format  = FORMAT;
    options.scanout = true;
    options.length  = 3;
    options.size    = MODE->pixelSize;
    return m_owner->m_output->swapchain->reconfigure(options);
}
void CMonitorState::applyModeWithSwapchain(const SP<Aquamarine::SOutputMode>& mode) {
    m_owner->m_output->state->setMode(mode);
    updateSwapchain();
}

void CMonitorState::applyCustomModeWithSwapchain(const SP<Aquamarine::SOutputMode>& mode) {
    m_owner->m_output->state->setCustomMode(mode);
    updateSwapchain();
}

bool CMonitor::needsACopyFB() {
    return !m_mirrors.empty() || Screenshare::mgr()->outputNeedsCopyFB(m_self.lock());
}

bool CMonitor::needsUnmodifiedCopy() {
    static const auto PKEEP = CConfigValue<Config::INTEGER>("render:keep_unmodified_copy");
    if (*PKEEP == 1)
        return true;

    const bool HAS_MODS = m_sdrMinLuminance != SDR_MIN_LUMINANCE || m_sdrMaxLuminance != SDR_MAX_LUMINANCE || (m_sdrBrightness > 0 && m_sdrBrightness != 1.0) ||
        (m_sdrSaturation > 0 && m_sdrSaturation != 1.0);

    if (!HAS_MODS)
        return false;

    // TODO handle some FP16 cases
    if (m_imageDescription->value().transferFunction != CM_TRANSFER_FUNCTION_ST2084_PQ && m_imageDescription->value().transferFunction != CM_TRANSFER_FUNCTION_HLG)
        return false;

    return *PKEEP == 2 ? true : needsACopyFB();
}

bool CMonitor::useFP16() {
    static const auto PFP16 = CConfigValue<Config::INTEGER>("render:use_fp16");

    // Auto: FP16 where an 8-bit buffer loses something: an HDR transfer function (values past SDR white), an ICC profile or a 10-bit output.
    // A wide-gamut SDR output at 8 bits composites in its own description instead: each surface is converted into it as it is drawn, which
    // FP16's final pass does anyway, and an opaque pixel is rounded to 8 bits once either way. FP16 doubles the bytes of every composited pass.
    auto needsFP16 = [this] {
        const auto& value = m_imageDescription->value();

        if (value.icc.present || m_enabled10bit)
            return true;

        return value.transferFunction == CM_TRANSFER_FUNCTION_ST2084_PQ || value.transferFunction == CM_TRANSFER_FUNCTION_HLG ||
            value.transferFunction == CM_TRANSFER_FUNCTION_EXT_LINEAR;
    };

    return g_pHyprRenderer->fp16Supported() && (*PFP16 == 1 || (*PFP16 == 2 && needsFP16()));
}

PImageDescription CMonitor::workBufferImageDescription() {
    static const auto PFP16TF     = CConfigValue<Config::INTEGER>("render:fp16_sdr_tf");
    static const auto PBLENDSPACE = CConfigValue<Config::INTEGER>("render:hdr_blend_space");

    if (!useFP16() && !m_imageDescription->value().icc.present)
        return m_imageDescription;

    const auto& value = m_imageDescription->value();

    const bool  isHDRLikeTF =
        value.transferFunction == CM_TRANSFER_FUNCTION_ST2084_PQ || value.transferFunction == CM_TRANSFER_FUNCTION_HLG || value.transferFunction == CM_TRANSFER_FUNCTION_EXT_LINEAR;

    const auto& cached = m_cachedInternalDescription->value();

    // HDR in extended sRGB: translucent surfaces blend in a gamma space, as in SDR mode. The range is SDR content's own in HDR (getCMSettings'
    // srcTFRange with the SDR mod), so SDR content keeps its codes, its white at 1.0, and the blend matches SDR mode's; HDR content goes above 1.0 and
    // wide-gamut content below 0.0. The final pass decodes it to linear light before the output's transfer function.
    if (isHDRLikeTF && *PBLENDSPACE == 1 && !value.icc.present) {
        const SImageDescription             SDR_CONTENT = {.transferFunction = CM_TRANSFER_FUNCTION_SRGB};
        const SImageDescription::SPCTFRange RANGE       = {.min = SDR_CONTENT.getTFMinLuminance(m_sdrMinLuminance), .max = SDR_CONTENT.getTFMaxLuminance(m_sdrMaxLuminance)};

        if (cached.transferFunction != CM_TRANSFER_FUNCTION_EXT_SRGB || cached.luminances != value.luminances || cached.tfRange != RANGE)
            m_cachedInternalDescription = CImageDescription::from(SImageDescription{
                .transferFunction = CM_TRANSFER_FUNCTION_EXT_SRGB,
                .primariesNameSet = true,
                .primariesNamed   = NColorManagement::CM_PRIMARIES_SRGB,
                .primaries        = NColorPrimaries::BT709,
                .luminances       = value.luminances,
                .tfRange          = RANGE,
            });
        return m_cachedInternalDescription;
    }

    // HDR
    if (isHDRLikeTF || *PFP16TF != 0) {
        if (cached.transferFunction != LINEAR_IMAGE_DESCRIPTION->value().transferFunction || cached.luminances != value.luminances)
            m_cachedInternalDescription = LINEAR_IMAGE_DESCRIPTION->with(value.luminances);
        return m_cachedInternalDescription;
    }

    // SDR
    if (cached.transferFunction != chooseTF(m_sdrEotf))
        m_cachedInternalDescription = CImageDescription::from(SImageDescription{
            .transferFunction = chooseTF(m_sdrEotf),
            .primariesNameSet = true,
            // render:keep_unmodified_copy and other conditions that trigger MRT for screen sharing expect a work buffer with sRGB primaries
            .primariesNamed = NColorManagement::CM_PRIMARIES_SRGB,
            .primaries      = NColorPrimaries::BT709,
        });

    return m_cachedInternalDescription;
}

WP<CMonitorResources> CMonitor::resources() {
    // under an underlay's video, the frame goes to the overlay and keeps its alpha; during a direct scanout the output's
    // format is the client buffer's (a video's NV12, which GL cannot render to), so the composite's, saved on entry
    const auto OUTPUT_FORMAT = m_lastScanout.expired() ? m_output->state->state().drmFormat : m_prevDrmFormat;
    const auto DRM_FORMAT    = useFP16() ? DRM_FORMAT_ABGR16161616F : m_underlay.entered ? DRM_FORMAT_ARGB8888 : OUTPUT_FORMAT;
    const auto DESC          = workBufferImageDescription();

    if (!m_resources || m_resources->m_drmFormat != DRM_FORMAT || m_resources->m_size != m_transformedSize)
        m_resources = makeUnique<CMonitorResources>(m_self, DRM_FORMAT, m_transformedSize, DESC);

    if (m_resources->m_imageDescription != DESC)
        m_resources->setImageDescription(DESC);

    return m_resources;
}

WP<CMonitorResources> CMonitor::currentResources() {
    return m_resources;
}

bool CMonitor::trySetFormat(std::span<const uint32_t> formats) {
    for (auto fmt : formats) {
        m_output->state->setFormat(fmt);
        m_prevDrmFormat = m_drmFormat;
        m_drmFormat     = fmt;

        const auto fmtName = drmFormatToString(fmt);
        if (!m_state.test()) {
            LOG(Log::ERR, "output {} failed basic test on format {}", m_name, fmtName);
        } else {
            LOG(Log::DEBUG, "output {} succeeded basic test on format {}", m_name, fmtName);
            return true;
        }
    }
    return false;
}
