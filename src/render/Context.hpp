#pragma once

#include "types.hpp"
#include "pass/Pass.hpp"
#include "SurfaceBufferUse.hpp"

namespace Aquamarine {
    class IBuffer;
}

struct SBackdropScope;

namespace Render {
    class IRenderbuffer;
    class CRenderContext;
    class CSceneResources;

    class CRenderDataScope {
      public:
        explicit CRenderDataScope(CRenderContext& ctx);
        ~CRenderDataScope();
        CRenderDataScope(const CRenderDataScope&)            = delete;
        CRenderDataScope& operator=(const CRenderDataScope&) = delete;

      private:
        CRenderContext& m_ctx;
        SRenderData     m_data;
        size_t          m_backdropDepth    = 0;
        bool            m_blurShouldRender = false;
    };

    // One reusable render session. Only m_data may be copied for nested draws.
    class CRenderContext {
      public:
        CRenderContext();
        ~CRenderContext();
        CRenderContext(const CRenderContext&)                  = delete;
        CRenderContext& operator=(const CRenderContext&)       = delete;
        CRenderContext(CRenderContext&&)                       = delete;
        CRenderContext&            operator=(CRenderContext&&) = delete;

        bool                       begin(SP<CSceneResources> resources = nullptr);
        bool                       active() const;
        void                       reset();
        const SP<CSceneResources>& sceneResources() const;
        bool                       readOnlyEffects() const;
        bool                       shouldRenderContent(bool noScreenShare) const;
        Time::steady_tp            effectTime() const;

        // Nested draws retain session routing, source buffers and persistent caches.
        [[nodiscard]] CRenderDataScope saveDrawState();

        SRenderData                    m_data;
        CRenderPass                    m_pass;
        CRenderPass*                   m_currentPass = nullptr;
        eRenderMode                    m_mode        = RENDER_MODE_NORMAL;
        SP<Aquamarine::IBuffer>        m_currentBuffer;
        SP<IRenderbuffer>              m_currentRenderbuffer;

        // Transfer to the backend's pending batch or submission before reset.
        std::vector<SSurfaceBufferUse> m_usedAsyncBuffers;

        struct SBackdropCapture {
            SP<SBackdropScope> scope;
            SP<IFramebuffer>   framebuffer;
        };
        std::vector<SBackdropCapture> m_backdropCaptures;

        struct SCMSettingsCacheEntry {
            uint64_t    srcDescId = 0, dstDescId = 0;
            void*       surfacePtr      = nullptr; // read-only!!
            bool        modifySDR       = false;
            float       sdrMinLuminance = -1.F;
            int         sdrMaxLuminance = -1;
            SCMSettings settings;
        };
        std::vector<SCMSettingsCacheEntry> m_cmSettingsCache;

        bool                               m_blockSurfaceFeedback = false;
        bool                               m_renderingSnapshot    = false;
        bool                               m_renderingCapture     = false;
        bool                               m_swapchainAcquired    = false;

        // a frame for the overlay plane over an underlay's video: the video's surface is a hole the plane shows through,
        // and the output keeps the frame's alpha
        struct {
            bool                   active = false;
            WP<CWLSurfaceResource> surface;
            CBox                   hole;                // in output pixels: the video's place on the primary plane
            float                  round         = 0.F; // a window's content: its corners, as the renderer rounds the surface
            float                  roundingPower = 2.F;
            CRegion                composited; // in output pixels: the content's part not declared opaque, drawn as the composite would
        } m_underlay;
        bool underlay() const;

        struct {
            bool fakeFrame            = false;
            bool offloadedFramebuffer = false;
            bool applyFinalShader     = false;
        } m_gl;

      private:
        bool                m_active = false;
        SP<CSceneResources> m_sceneResources;
        Time::steady_tp     m_frameTime = {};
    };
}
