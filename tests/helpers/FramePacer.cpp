#include <helpers/time/FramePacer.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <vector>

using namespace Time;
using namespace std::chrono_literals;

static constexpr steady_dur PERIOD_120 = std::chrono::duration_cast<steady_dur>(std::chrono::duration<double>(1.0 / 120.0));

// the output's nth refresh after anchor
static steady_tp refresh(const steady_tp& anchor, int64_t n) {
    return anchor + n * PERIOD_120;
}

TEST(FramePacer, divisorKeepsWholeRefreshes) {
    EXPECT_EQ(CFramePacer::divisor(120.F, 60), 2U);
    EXPECT_EQ(CFramePacer::divisor(119.88F, 60), 2U);
    EXPECT_EQ(CFramePacer::divisor(120.01F, 60), 2U);
    EXPECT_EQ(CFramePacer::divisor(120.F, 30), 4U);
    EXPECT_EQ(CFramePacer::divisor(144.F, 60), 3U);
    EXPECT_EQ(CFramePacer::divisor(90.F, 60), 2U);
    EXPECT_EQ(CFramePacer::divisor(60.F, 60), 1U);
    EXPECT_EQ(CFramePacer::divisor(59.94F, 60), 1U);
    EXPECT_EQ(CFramePacer::divisor(120.F, 240), 1U);
}

TEST(FramePacer, divisorWithoutCap) {
    EXPECT_EQ(CFramePacer::divisor(120.F, 0), 1U);
    EXPECT_EQ(CFramePacer::divisor(120.F, -1), 1U);
    EXPECT_EQ(CFramePacer::divisor(0.F, 60), 1U);
}

TEST(FramePacer, noCapTakesEveryFrame) {
    CFramePacer pacer;
    const auto  T0   = steady_tp{} + 10s;
    const auto  GRID = CFramePacer::SGrid{.period = PERIOD_120, .anchor = T0, .divisor = 1};

    for (int i = 0; i < 10; ++i) {
        EXPECT_TRUE(pacer.take(T0 + i * 1ms, GRID));
    }
}

// frame events just after each refresh, as on a busy output: every second one goes out, never two in a row
TEST(FramePacer, halvesFrameEventsOnTheGrid) {
    CFramePacer       pacer;
    const auto        T0   = steady_tp{} + 10s;
    const auto        GRID = CFramePacer::SGrid{.period = PERIOD_120, .anchor = T0, .divisor = 2};

    std::vector<bool> taken;
    for (int64_t i = 0; i < 120; ++i) {
        taken.emplace_back(pacer.take(refresh(T0, i) + 300us, GRID));
    }

    int count = 0;
    for (size_t i = 0; i < taken.size(); ++i) {
        count += taken[i];
        EXPECT_EQ(taken[i], i % 2 == 0) << "refresh " << i;
    }

    EXPECT_EQ(count, 60);
}

// a render on the client's commit comes between two refreshes: it must not send a second batch for the same frame
TEST(FramePacer, noBurstAfterATakenFrame) {
    CFramePacer pacer;
    const auto  T0   = steady_tp{} + 10s;
    const auto  GRID = CFramePacer::SGrid{.period = PERIOD_120, .anchor = T0, .divisor = 2};

    EXPECT_TRUE(pacer.take(T0 + 50us, GRID));
    EXPECT_FALSE(pacer.take(T0 + 500us, GRID));
    EXPECT_FALSE(pacer.take(T0 + 3ms, GRID));
    EXPECT_FALSE(pacer.take(T0 + 6ms, GRID));
    EXPECT_FALSE(pacer.take(refresh(T0, 1) + 300us, GRID));
    EXPECT_FALSE(pacer.take(refresh(T0, 1) + 3ms, GRID));

    EXPECT_EQ(pacer.next(T0 + 6ms, GRID), refresh(T0, 2));
    EXPECT_TRUE(pacer.take(refresh(T0, 2) + 20us, GRID));
    EXPECT_FALSE(pacer.take(refresh(T0, 2) + 2ms, GRID));
}

// a frame event a little before the refresh the grid predicts still counts as that refresh
TEST(FramePacer, toleratesTimestampJitter) {
    CFramePacer pacer;
    const auto  T0   = steady_tp{} + 10s;
    const auto  GRID = CFramePacer::SGrid{.period = PERIOD_120, .anchor = T0, .divisor = 2};

    EXPECT_TRUE(pacer.take(T0 + 100us, GRID));
    EXPECT_TRUE(pacer.take(refresh(T0, 2) - 200us, GRID));
    EXPECT_FALSE(pacer.take(refresh(T0, 3) - 200us, GRID));
    EXPECT_TRUE(pacer.take(refresh(T0, 4) + 200us, GRID));
}

// an idle output: only the timer at next() sends, late by its latency each time, and the frames stay on the grid
TEST(FramePacer, timerDoesNotDrift) {
    CFramePacer pacer;
    const auto  T0   = steady_tp{} + 10s;
    const auto  GRID = CFramePacer::SGrid{.period = PERIOD_120, .anchor = T0, .divisor = 2};

    auto        now = T0 + 100us;
    EXPECT_TRUE(pacer.take(now, GRID));

    for (int64_t i = 1; i <= 1000; ++i) {
        // the client commits 3 ms after its callback, the render on it is held
        EXPECT_FALSE(pacer.take(now + 3ms, GRID));

        const auto NEXT = pacer.next(now + 3ms, GRID);
        EXPECT_EQ(NEXT, refresh(T0, 2 * i));

        now = NEXT + 400us;
        EXPECT_TRUE(pacer.take(now, GRID));
    }
}

// before the output presents, the grid starts at the first frame
TEST(FramePacer, gridFromTheFirstFrame) {
    CFramePacer pacer;
    const auto  T0   = steady_tp{} + 10s;
    const auto  GRID = CFramePacer::SGrid{.period = PERIOD_120, .anchor = std::nullopt, .divisor = 2};

    EXPECT_EQ(pacer.next(T0, GRID), T0);
    EXPECT_TRUE(pacer.take(T0, GRID));
    EXPECT_FALSE(pacer.take(T0 + 9ms, GRID));
    EXPECT_EQ(pacer.next(T0 + 9ms, GRID), T0 + 2 * PERIOD_120);
    EXPECT_TRUE(pacer.take(T0 + 2 * PERIOD_120 + 100us, GRID));
}

// the grid follows the output's presentations: a new anchor a refresh on keeps the frames whole refreshes apart
TEST(FramePacer, followsAMovingAnchor) {
    CFramePacer pacer;
    const auto  T0 = steady_tp{} + 10s;

    for (int64_t i = 0; i < 20; ++i) {
        const auto GRID = CFramePacer::SGrid{.period = PERIOD_120, .anchor = refresh(T0, i) + 30us, .divisor = 2};
        EXPECT_EQ(pacer.take(refresh(T0, i) + 300us, GRID), i % 2 == 0) << "refresh " << i;
    }
}
