#pragma once

namespace Render {
    // a monitor's frame as it begins: what would have it draw, and what its primary plane shows
    struct SKeptFrameState {
        bool commits         = false; // committed at once (render:new_render_scheduling renders some ahead of their commit)
        bool damaged         = false; // the monitor has damage since its last frame
        bool fullFrame       = false; // a full frame is due: forced frames, damage tracking off or per monitor, a damage blink, a zoom
        bool frontComposited = false; // the primary shows the monitor's last composited frame, in the monitor's format
        bool copied          = false; // a mirror or a capture copies the frame
        bool mirror          = false; // the monitor mirrors another
        bool solitaryRedraw  = false; // debug:render_solitary_wo_damage draws a solitary client without damage
        bool tearing         = false; // an immediate flip
    };

    // a frame with nothing new to draw (a hardware cursor moved or changed, a client asked for a frame alone) keeps the
    // primary's buffer: the buffer shown is already that frame, so it is committed again with no render and no new buffer
    bool frameKeepsBuffer(const SKeptFrameState& state);
}
