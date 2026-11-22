#include <render/KeptFrame.hpp>

#include <gtest/gtest.h>

using namespace Render;

// a frame with nothing new to draw over the last composite: a hardware cursor's move on an idle desktop
static SKeptFrameState idle() {
    return {
        .commits         = true,
        .frontComposited = true,
    };
}

TEST(KeptFrame, nothingToDrawKeepsTheBuffer) {
    EXPECT_TRUE(frameKeepsBuffer(idle()));
}

TEST(KeptFrame, damageRenders) {
    auto state    = idle();
    state.damaged = true;
    EXPECT_FALSE(frameKeepsBuffer(state));
}

TEST(KeptFrame, aFullFrameRenders) {
    auto state      = idle();
    state.fullFrame = true;
    EXPECT_FALSE(frameKeepsBuffer(state));
}

TEST(KeptFrame, anotherBufferShownRenders) {
    // a direct scan-out's, a fresh swapchain buffer, one in another format
    auto state            = idle();
    state.frontComposited = false;
    EXPECT_FALSE(frameKeepsBuffer(state));
}

TEST(KeptFrame, aFrameRenderedAheadRenders) {
    auto state    = idle();
    state.commits = false;
    EXPECT_FALSE(frameKeepsBuffer(state));
}

TEST(KeptFrame, copiesMirrorsSolitaryRedrawsAndTearingRender) {
    for (bool SKeptFrameState::* flag : {&SKeptFrameState::copied, &SKeptFrameState::mirror, &SKeptFrameState::solitaryRedraw, &SKeptFrameState::tearing}) {
        auto state  = idle();
        state.*flag = true;
        EXPECT_FALSE(frameKeepsBuffer(state));
    }
}

TEST(KeptFrame, keepsOnlyWithNothingAgainstIt) {
    // every combination: kept exactly when committed now over the last composite, with nothing else set
    for (int bits = 0; bits < (1 << 8); bits++) {
        const SKeptFrameState STATE{
            .commits         = (bits & 1) != 0,
            .damaged         = (bits & 2) != 0,
            .fullFrame       = (bits & 4) != 0,
            .frontComposited = (bits & 8) != 0,
            .copied          = (bits & 16) != 0,
            .mirror          = (bits & 32) != 0,
            .solitaryRedraw  = (bits & 64) != 0,
            .tearing         = (bits & 128) != 0,
        };
        EXPECT_EQ(frameKeepsBuffer(STATE), bits == (1 | 8)) << "bits=" << bits;
    }
}
