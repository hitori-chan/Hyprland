#pragma once

#include "../../DesktopTypes.hpp"
#include "../../../helpers/signal/Signal.hpp"
#include "../../../helpers/time/FramePacer.hpp"

#include <optional>

class CEventLoopTimer;

namespace Desktop::View {
    class CWindow;

    // Holds a window's frame callbacks to its max_fps rule, on its monitor's refreshes. The callbacks of all its
    // surfaces, subsurfaces and popups included, go out together on a refresh the cap allows, or on the next one.
    class CWindowFrameThrottle {
      public:
        explicit CWindowFrameThrottle(CWindow& window);

        // whether max_fps holds the window's frame callbacks: a cap below its monitor's refresh rate
        bool limits() const;

        // a surface of the window has frame callbacks for now: whether the refresh is one the cap allows, the window's
        // callbacks sent. If not, they are held for the next one
        bool frame(const Time::steady_tp& now);

      private:
        Time::CFramePacer::SGrid grid();
        void                     follow(PHLMONITOR monitor);
        void                     sendAll(const Time::steady_tp& now);

        CWindow&                 m_window;
        Time::CFramePacer        m_pacer;
        SP<CEventLoopTimer>      m_timer;

        // the monitor whose refreshes the grid is on, and its last presentation
        PHLMONITORREF                  m_monitor;
        std::optional<Time::steady_tp> m_lastPresentation;
        CHyprSignalListener            m_presented;
    };
}
