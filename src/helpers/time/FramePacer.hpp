#pragma once

#include "Time.hpp"

#include <cstdint>
#include <optional>

namespace Time {

    // Paces a client's frames to an output's refreshes: a frame goes out on a refresh, at least divisor refreshes
    // after the last one, so a frame rate cap stays on the output's vblank grid.
    class CFramePacer {
      public:
        struct SGrid {
            steady_dur               period = steady_dur::zero(); // the output's refresh period
            std::optional<steady_tp> anchor;                      // a time the output refreshed at, e.g. its last presentation
            uint32_t                 divisor = 1;                 // the refreshes a frame takes
        };

        // the refreshes a frame of a client capped at maxFps takes on an output at refreshHz: 1 is every refresh, no cap
        static uint32_t divisor(float refreshHz, int64_t maxFps);

        // whether a frame may go out at now. If it may, it is taken: the next one waits for divisor refreshes
        bool take(const steady_tp& now, const SGrid& grid);

        // when the next frame may go out
        steady_tp next(const steady_tp& now, const SGrid& grid) const;

      private:
        // the refresh nearest to t
        steady_tp                snap(const steady_tp& t, const SGrid& grid) const;

        std::optional<steady_tp> m_last;
    };
};
