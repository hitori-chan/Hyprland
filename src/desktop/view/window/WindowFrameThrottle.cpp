#include "WindowFrameThrottle.hpp"

#include "Window.hpp"
#include "../Popup.hpp"
#include "../../rule/windowRule/WindowRuleApplicator.hpp"
#include "../../../output/Monitor.hpp"
#include "../../../protocols/core/Compositor.hpp"
#include "../../../managers/eventLoop/EventLoopManager.hpp"
#include "../../../managers/eventLoop/EventLoopTimer.hpp"

#include <chrono>

using namespace Desktop;
using namespace Desktop::View;

CWindowFrameThrottle::CWindowFrameThrottle(CWindow& window) : m_window(window) {
    ;
}

bool CWindowFrameThrottle::limits() const {
    const auto MAX_FPS = m_window.m_ruleApplicator->maxFps().valueOrDefault();
    if (MAX_FPS <= 0)
        return false;

    const auto MONITOR = m_window.m_monitor.lock();
    return MONITOR && Time::CFramePacer::divisor(MONITOR->m_refreshRate, MAX_FPS) > 1;
}

bool CWindowFrameThrottle::frame(const Time::steady_tp& now) {
    const auto GRID = grid();

    if (m_pacer.take(now, GRID)) {
        sendAll(now);
        return true;
    }

    // held until the next refresh the cap allows, if no frame comes before it
    if (!m_timer) {
        m_timer = makeShared<CEventLoopTimer>(std::nullopt, [this](SP<CEventLoopTimer>, void*) { frame(Time::steadyNow()); }, nullptr);
        g_pEventLoopManager->addTimer(m_timer);
    }

    // now may be the start of a render pass, a while ago: the timeout counts from the clock
    if (!m_timer->armed()) {
        const auto NEXT  = m_pacer.next(now, GRID);
        const auto CLOCK = Time::steadyNow();
        m_timer->updateTimeout(NEXT > CLOCK ? NEXT - CLOCK : Time::steady_dur::zero());
    }

    return false;
}

Time::CFramePacer::SGrid CWindowFrameThrottle::grid() {
    const auto MONITOR = m_window.m_monitor.lock();
    if (!MONITOR || MONITOR->m_refreshRate <= 0.F)
        return {};

    follow(MONITOR);

    return {
        .period  = std::chrono::duration_cast<Time::steady_dur>(std::chrono::duration<double>(1.0 / MONITOR->m_refreshRate)),
        .anchor  = m_lastPresentation,
        .divisor = Time::CFramePacer::divisor(MONITOR->m_refreshRate, m_window.m_ruleApplicator->maxFps().valueOrDefault()),
    };
}

void CWindowFrameThrottle::follow(PHLMONITOR monitor) {
    if (m_monitor == monitor)
        return;

    m_monitor = monitor;
    m_lastPresentation.reset();
    m_presented = monitor->m_events.presented.listen([this](const Time::steady_tp& when) { m_lastPresentation = when; });
}

void CWindowFrameThrottle::sendAll(const Time::steady_tp& now) {
    if (m_timer)
        m_timer->updateTimeout(std::nullopt);

    const auto SEND = [&now](SP<CWLSurfaceResource> surface, const Vector2D& offset, void* data) { surface->sendFrameCallbacks(now); };

    if (const auto SURFACE = m_window.resource(); SURFACE)
        SURFACE->breadthfirst(SEND, nullptr);

    if (const auto& HEAD = m_window.popupHead(); HEAD) {
        HEAD->breadthfirst(
            [&SEND](SP<CPopup> popup, void* data) {
                if (const auto SURFACE = popup->resource(); SURFACE)
                    SURFACE->breadthfirst(SEND, nullptr);
            },
            nullptr);
    }
}
