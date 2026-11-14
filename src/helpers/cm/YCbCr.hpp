#pragma once

#include <cstdint>
#include "../math/Math.hpp"

namespace NColorManagement {
    // the matrix a YCbCr buffer converts to RGB with
    enum eYCbCrEncoding : uint8_t {
        CM_YCBCR_BT601 = 0,
        CM_YCBCR_BT709,
        CM_YCBCR_BT2020,
    };

    struct SYCbCr {
        eYCbCrEncoding encoding  = CM_YCBCR_BT709;
        bool           fullRange = false;

        bool           operator==(const SYCbCr&) const = default;
    };

    // an unmarked YCbCr buffer: BT.601 at SD sizes and BT.709 above, limited range, as video players assume
    SYCbCr defaultYCbCr(const Vector2D& bufferSize);
}
