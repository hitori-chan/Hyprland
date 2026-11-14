#include "YCbCr.hpp"

NColorManagement::SYCbCr NColorManagement::defaultYCbCr(const Vector2D& bufferSize) {
    // mpv's and FFmpeg's guess for an untagged picture
    const bool HD = bufferSize.x >= 1280 || bufferSize.y > 576;
    return {.encoding = HD ? CM_YCBCR_BT709 : CM_YCBCR_BT601, .fullRange = false};
}
