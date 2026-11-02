#pragma once

#include "../../../SharedDefs.hpp"
#include "../../../managers/fullscreen/FullscreenTypes.hpp"

#include <optional>

namespace Desktop::View {
    struct SFullscreenStackingContext {
        bool isFullscreenWindow    = false;
        bool pinned                = false;
        bool groupedWithFullscreen = false;
    };

    struct SFullscreenRequestSuppression {
        bool fullscreen       = false;
        bool maximize         = false;
        bool fullscreenOutput = false;
    };

    struct SPendingClientFullscreenRequest {
        std::optional<Fullscreen::eFullscreenMode> mode;
        std::optional<MONITORID>                   monitor;
    };

    class CWindowFullscreenPolicy {
      public:
        bool                                   allowedOverFullscreen() const;
        void                                   setAllowedOverFullscreen(bool allowed);
        bool                                   effectiveAllowedOverFullscreen(const SFullscreenStackingContext& context) const;

        const SFullscreenRequestSuppression&   requestSuppression() const;
        void                                   setRequestSuppression(const SFullscreenRequestSuppression& suppression);

        const SPendingClientFullscreenRequest& pendingClientRequest() const;
        void                                   setPendingClientRequest(Fullscreen::eFullscreenMode mode, std::optional<MONITORID> monitor = std::nullopt);
        void                                   clearPendingClientMode(Fullscreen::eFullscreenMode mode);
        SPendingClientFullscreenRequest        consumePendingClientRequest();
        Fullscreen::eFullscreenMode            toldPendingMode() const;
        void                                   setToldPendingMode(Fullscreen::eFullscreenMode mode);

        bool                                   pinFullscreened() const;
        void                                   setPinFullscreened(bool pinFullscreened);

        bool                                   restoreClientMaximized() const;
        void                                   setRestoreClientMaximized(bool restore);

      private:
        bool                            m_allowedOverFullscreen = true;
        SFullscreenRequestSuppression   m_requestSuppression;
        SPendingClientFullscreenRequest m_pendingClientRequest;
        bool                            m_pinFullscreened        = false;
        bool                            m_restoreClientMaximized = false;
        Fullscreen::eFullscreenMode     m_toldPendingMode        = Fullscreen::FSMODE_NONE; // the first configure told the pending ask; map takes a refusal back
    };
}
