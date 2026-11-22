#include "KeptFrame.hpp"

bool Render::frameKeepsBuffer(const SKeptFrameState& state) {
    if (!state.commits || state.damaged || state.fullFrame || !state.frontComposited)
        return false;

    // a copy takes the frame as rendered, a solitary redraw draws without damage, and an immediate flip may change nothing
    // but the primary's buffer (not the cursor's position)
    return !state.copied && !state.mirror && !state.solitaryRedraw && !state.tearing;
}
