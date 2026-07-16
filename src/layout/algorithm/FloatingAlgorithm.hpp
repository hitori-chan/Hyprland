#pragma once

#include "../../helpers/math/Math.hpp"
#include "../../helpers/memory/Memory.hpp"

#include "ModeAlgorithm.hpp"

namespace Layout {

    class ITarget;
    class CAlgorithm;

    constexpr Vector2D FLOATING_DEFAULT_SIZE = {640, 400};

    class IFloatingAlgorithm : public IModeAlgorithm {
      public:
        virtual ~IFloatingAlgorithm() = default;

        // a target is being moved by a delta
        virtual void moveTarget(const Vector2D& Δ, SP<ITarget> target) = 0;

        // a target is moved to a pos x size
        virtual void setTargetGeom(const CBox& geom, SP<ITarget> target) = 0;

        virtual void recenter(SP<ITarget> t);

        virtual void recalculate(eRecalculateReason reason = RECALCULATE_REASON_UNKNOWN);

      protected:
        IFloatingAlgorithm() = default;

        // a window mapped straight into fullscreen has no windowed geometry to
        // restore to: place it like a fresh spawn instead. Returns true if handled.
        bool respawnIfBornFullscreen(SP<ITarget> t);

        friend class Layout::CAlgorithm;
    };
}