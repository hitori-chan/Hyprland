#pragma once

#include <cstdint>
#include "../math/Math.hpp"

namespace NColorManagement {
    enum eYCbCrCoefficients : uint8_t {
        CM_YCBCR_COEFFICIENTS_UNSET = 0,
        CM_YCBCR_COEFFICIENTS_BT601,
        CM_YCBCR_COEFFICIENTS_BT709,
        CM_YCBCR_COEFFICIENTS_BT2020,
    };

    enum eYCbCrRange : uint8_t {
        CM_YCBCR_RANGE_UNSET = 0,
        CM_YCBCR_RANGE_FULL,
        CM_YCBCR_RANGE_LIMITED,
    };

    // H.273 Chroma420SampleLocType
    enum eChromaLocation : uint8_t {
        CM_CHROMA_LOCATION_UNSET = 0,
        CM_CHROMA_LOCATION_TYPE_0,
        CM_CHROMA_LOCATION_TYPE_1,
        CM_CHROMA_LOCATION_TYPE_2,
        CM_CHROMA_LOCATION_TYPE_3,
        CM_CHROMA_LOCATION_TYPE_4,
        CM_CHROMA_LOCATION_TYPE_5,
    };

    // how to convert YCbCr to RGB, unset values are left to the driver
    struct SColorRepresentation {
        eYCbCrCoefficients coefficients   = CM_YCBCR_COEFFICIENTS_UNSET;
        eYCbCrRange        range          = CM_YCBCR_RANGE_UNSET;
        eChromaLocation    chromaLocation = CM_CHROMA_LOCATION_UNSET;

        bool               operator==(const SColorRepresentation&) const = default;

        // unset values as mpv and FFmpeg guess for an untagged picture: BT.601 at SD sizes, BT.709 above, limited range
        SColorRepresentation resolved(const Vector2D& bufferSize) const {
            SColorRepresentation r = *this;
            if (r.coefficients == CM_YCBCR_COEFFICIENTS_UNSET)
                r.coefficients = bufferSize.x >= 1280 || bufferSize.y > 576 ? CM_YCBCR_COEFFICIENTS_BT709 : CM_YCBCR_COEFFICIENTS_BT601;
            if (r.range == CM_YCBCR_RANGE_UNSET)
                r.range = CM_YCBCR_RANGE_LIMITED;
            return r;
        }
    };
}
