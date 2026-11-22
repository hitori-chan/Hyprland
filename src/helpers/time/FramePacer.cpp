#include "FramePacer.hpp"
#include "../memory/Memory.hpp"

#include <algorithm>
#include <cmath>

using namespace Time;

uint32_t CFramePacer::divisor(float refreshHz, int64_t maxFps) {
    // a mode a little over its nominal rate (120.01 Hz) still halves to a cap of 60
    constexpr double REFRESH_TOLERANCE = 1.01;

    if (maxFps <= 0 || refreshHz <= 0.F)
        return 1;

    return sc<uint32_t>(std::max(1.0, std::ceil(sc<double>(refreshHz) / (sc<double>(maxFps) * REFRESH_TOLERANCE))));
}

steady_tp CFramePacer::snap(const steady_tp& t, const SGrid& grid) const {
    const auto ANCHOR    = grid.anchor.value_or(m_last.value_or(t));
    const auto REFRESHES = std::llround(sc<double>((t - ANCHOR).count()) / sc<double>(grid.period.count()));

    return ANCHOR + REFRESHES * grid.period;
}

bool CFramePacer::take(const steady_tp& now, const SGrid& grid) {
    if (grid.divisor <= 1 || grid.period <= steady_dur::zero())
        return true;

    // the refresh this frame is for: a frame event comes just after one, a timer at one, a render on a commit between two
    const auto REFRESH = snap(now, grid);

    if (m_last && std::llround(sc<double>((REFRESH - *m_last).count()) / sc<double>(grid.period.count())) < sc<int64_t>(grid.divisor))
        return false;

    m_last = REFRESH;
    return true;
}

steady_tp CFramePacer::next(const steady_tp& now, const SGrid& grid) const {
    if (!m_last || grid.divisor <= 1 || grid.period <= steady_dur::zero())
        return now;

    return std::max(now, snap(*m_last + grid.divisor * grid.period, grid));
}
