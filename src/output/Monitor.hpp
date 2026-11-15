#pragma once

#include "../defines.hpp"
#include <stack>
#include <vector>
#include "../SharedDefs.hpp"
#include "../helpers/MiscFunctions.hpp"
#include "../helpers/WLClasses.hpp"
#include <array>
#include <span>
#include "../helpers/AnimatedVariable.hpp"
#include "../helpers/CMType.hpp"
#include "IMonitorArrangeable.hpp"
#include "IMonitorQueryable.hpp"
#include "MonitorTypes.hpp"

#include <xf86drmMode.h>
#include "MonitorZoomController.hpp"
#include "../render/Texture.hpp"
#include "../render/Framebuffer.hpp"
#include "MonitorResources.hpp"
#include "../helpers/time/Timer.hpp"
#include "../helpers/math/Math.hpp"
#include "../desktop/reserved/ReservedArea.hpp"
#include <optional>
#include "../helpers/cm/ColorManagement.hpp"
#include "../helpers/signal/Signal.hpp"
#include "DamageRing.hpp"
#include <aquamarine/output/Output.hpp>
#include <aquamarine/allocator/Swapchain.hpp>
#include <hyprutils/os/FileDescriptor.hpp>
#include <sys/types.h>

#include "../helpers/TransferFunction.hpp"
#include "../config/shared/monitor/MonitorRule.hpp"

class CSyncTimeline;
class CEventLoopTimer;

namespace Monitor {
    class CMonitorResources;
    class CMonitorFrameScheduler;
    class COutputCommitCoordinator;
    class CMonitorWindowTransaction;
    class CMonitor;

    class CMonitorState {
      public:
        CMonitorState(CMonitor* owner);
        ~CMonitorState() = default;

        bool commit();
        bool test();
        bool updateSwapchain();
        void applyModeWithSwapchain(const SP<Aquamarine::SOutputMode>& mode);
        void applyCustomModeWithSwapchain(const SP<Aquamarine::SOutputMode>& mode);

      private:
        void      ensureBufferPresent();

        CMonitor* m_owner = nullptr;
    };

    class CMonitor : public IMonitorQueryable, public IMonitorArrangeable {
      public:
        CMonitor(SP<Aquamarine::IOutput> output);
        virtual ~CMonitor() override;

        Vector2D                    m_position         = Vector2D(-1, -1); // means unset
        Vector2D                    m_xwaylandPosition = Vector2D(-1, -1); // means unset
        Config::eAutoDirs           m_autoDir          = Config::DIR_AUTO_NONE;
        Vector2D                    m_size             = Vector2D(0, 0);
        Vector2D                    m_pixelSize        = Vector2D(0, 0);
        Vector2D                    m_transformedSize  = Vector2D(0, 0);

        MONITORID                   m_id                     = MONITOR_INVALID;
        PHLWORKSPACE                m_activeWorkspace        = nullptr;
        PHLWORKSPACE                m_activeSpecialWorkspace = nullptr;
        float                       m_setScale               = 1; // scale set by cfg
        float                       m_scale                  = 1; // real scale

        std::string                 m_name             = "";
        std::string                 m_description      = "";
        std::string                 m_shortDescription = "";

        drmModeModeInfo             m_customDrmMode = {};

        Desktop::CReservedArea      m_reservedArea;
        Desktop::CReservedArea      m_lastAnnouncedReservedArea;

        CMonitorState               m_state;
        CDamageRing                 m_damage;

        SP<Aquamarine::IOutput>     m_output;
        float                       m_refreshRate     = 60; // Hz
        int                         m_forceFullFrames = 0;
        bool                        m_scheduledRecalc = false;
        wl_output_transform         m_transform       = WL_OUTPUT_TRANSFORM_NORMAL;
        float                       m_xwaylandScale   = 1.f;

        std::optional<Vector2D>     m_forceSize;
        SP<Aquamarine::SOutputMode> m_currentMode;
        SP<Aquamarine::CSwapchain>  m_cursorSwapchain;
        uint32_t                    m_drmFormat     = DRM_FORMAT_INVALID;
        uint32_t                    m_prevDrmFormat = DRM_FORMAT_INVALID;

        CMonitorZoomController      m_zoomController;

        bool                        m_dpmsStatus       = true;
        bool                        m_vrrActive        = false; // this can be TRUE even if VRR is not active in the case that this display does not support it.
        bool                        m_enabled10bit     = false; // as above, this can be TRUE even if 10 bit failed.
        NCMType::eCMType            m_cmType           = NCMType::CM_SRGB;
        NTransferFunction::eTF      m_sdrEotf          = NTransferFunction::TF_DEFAULT;
        float                       m_sdrSaturation    = 1.0f;
        float                       m_sdrBrightness    = 1.0f;
        float                       m_sdrMinLuminance  = 0.2f;
        int                         m_sdrMaxLuminance  = 80;
        bool                        m_createdByUser    = false;
        bool                        m_isUnsafeFallback = false;

        SP<CEventLoopTimer>         m_dpmsRetryTimer;

        bool                        m_pendingFrame    = false; // if we schedule a frame during rendering, reschedule it after
        bool                        m_renderingActive = false;

        bool                        m_ratsScheduled = false;
        CTimer                      m_lastPresentationTimer;

        bool                        m_isBeingLeased = false;

        Config::CMonitorRule        m_activeMonitorRule;

        SP<Render::ITexture>        m_splash;
        SP<Render::ITexture>        m_background;

        // explicit sync
        Hyprutils::OS::CFileDescriptor m_inFence; // TODO: remove when aq uses CFileDescriptor

        PHLMONITORREF                  m_self;

        UP<CMonitorFrameScheduler>     m_frameScheduler;
        UP<COutputCommitCoordinator>   m_commitCoordinator;
        UP<CMonitorWindowTransaction>  m_windowTransaction;

        // mirroring
        PHLMONITORREF              m_mirrorOf;
        std::vector<PHLMONITORREF> m_mirrors;

        // ctm
        Mat3x3 m_ctm        = Mat3x3::identity();
        bool   m_ctmUpdated = false;

        // for tearing
        PHLWINDOWREF m_solitaryClient;

        // for direct scanout
        PHLWINDOWREF m_lastScanout;
        bool m_directScanoutIsActive    = false; // for cleanup logic. m_lastScanout.expired() can become true before the DS cleanup if client crashes/exits while DS is active.
        bool m_scanoutNeedsCursorUpdate = false;

        // for the underlay: a fullscreen window's video, or with render:underlay 2 a window's content, on the primary plane,
        // the rest of the frame composited over it on an overlay plane, with a hole where it shows
        struct {
            WP<CWLSurfaceResource>                     surface;              // the video, or the window's content
            PHLWINDOWREF                               window;               // its window
            bool                                       video         = true; // a fullscreen window's video, not a window's content
            float                                      round         = 0.F;  // the hole's corners, in output pixels, as the surface's
            float                                      roundingPower = 2.F;
            Aquamarine::COutputState::SBufferPlacement placement;                     // its place on the primary, as last tested
            uint32_t                                   format   = DRM_FORMAT_INVALID; // its buffer's layout, as last tested
            uint64_t                                   modifier = DRM_FORMAT_MOD_INVALID;
            SP<Aquamarine::CSwapchain>                 swapchain;            // the overlay's buffers, which the frame renders into
            bool                                       entered      = false; // from the first underlay frame to the leave: the primary is in the video's format
            bool                                       active       = false; // the overlay plane is on
            bool                                       videoDamaged = false; // a new video buffer since the last commit
            Time::steady_tp                            lastFrameEvents;      // the clients' last frame events, or the last presentation
        } m_underlay;
        SP<CEventLoopTimer> m_underlayFrameTimer; // a skipped underlay frame's frame events, one refresh after the last

        // underlay frames since the start: the overlay rendered, or the primary flipped alone (the video's damage only)
        struct {
            uint64_t rendered  = 0;
            uint64_t videoOnly = 0;
            uint64_t skipped   = 0; // a video commit with no new buffer: no flip
        } m_underlayFrames;

        // render:cm_hdr_headroom: in HDR the metadata's MaxCLL follows the HDR content shown, as macOS's EDR headroom
        // does: SDR white with none, so a panel that lights highlights by it (Apple's) keeps its backlight at the SDR
        // level; the content's peak while some is drawn, reached by a geometric ramp. In headroom over SDR white (1: none)
        struct {
            float           request = 0; // the largest peak the HDR content shown asks for at this frame, nits (0: none)
            float           origin = 1.F, target = 1.F, current = 1.F;
            float           duration = 0; // the ramp's, in seconds
            Time::steady_tp start;
            Time::steady_tp lastSeen; // the last frame with HDR content
        } m_hdrHeadroom;
        SP<CEventLoopTimer> m_hdrHeadroomTimer; // the ramp down's start, after the hold
        bool                hdrHeadroomApplies();
        void                hdrHeadroomRequest(const NColorManagement::SImageDescription& desc);
        void                hdrHeadroomFrame();   // a frame begins: the request from the content shown, the ramp advanced
        void                hdrHeadroomStep();    // the ramp advanced; damages the monitor on a step, schedules its frames
        uint16_t            hdrHeadroomMaxCLL();  // the MaxCLL to send: SDR white times the headroom now
        bool                hdrHeadroomPending(); // a ramp step not yet sent

        // for special fade/blur
        PHLANIMVAR<float> m_specialFade;
        PHLANIMVAR<float> m_specialDim;
        PHLANIMVAR<float> m_specialBlur;

        // for dpms off anim
        PHLANIMVAR<float> m_dpmsBlackOpacity;
        bool              m_pendingDpmsAnimation        = false;
        int               m_pendingDpmsAnimationCounter = 0;

        PHLANIMVAR<float> m_cursorZoom;

        // for fading in the wallpaper because it doesn't happen instantly (it's loaded async)
        PHLANIMVAR<float> m_backgroundOpacity;

        // for initial zoom anim
        PHLANIMVAR<float> m_zoomAnimProgress;
        CTimer            m_newMonitorAnimTimer;
        int               m_zoomAnimFrameCounter = 0;

        struct {
            bool canTear         = false;
            bool nextRenderTorn  = false;
            bool activelyTearing = false;

            bool busy                    = false;
            bool frameScheduledWhileBusy = false;
        } m_tearingState;

        struct {
            CSignalT<>                commit;
            CSignalT<>                destroy;
            CSignalT<>                connect;
            CSignalT<>                disconnect;
            CSignalT<>                dpmsChanged;
            CSignalT<>                modeChanged;
            CSignalT<Time::steady_tp> presented;
        } m_events;

        std::array<std::vector<PHLLSREF>, 4> m_layerSurfaceLayers;

        // keep in sync with socket1 output
        enum eDSBlockReason : uint16_t {
            DS_OK = 0,

            DS_BLOCK_UNKNOWN   = (1 << 0),
            DS_BLOCK_USER      = (1 << 1),
            DS_BLOCK_WINDOWED  = (1 << 2),
            DS_BLOCK_CONTENT   = (1 << 3),
            DS_BLOCK_MIRROR    = (1 << 4),
            DS_BLOCK_RECORD    = (1 << 5),
            DS_BLOCK_SW        = (1 << 6),
            DS_BLOCK_CANDIDATE = (1 << 7),
            DS_BLOCK_SURFACE   = (1 << 8),
            DS_BLOCK_TRANSFORM = (1 << 9),
            DS_BLOCK_DMA       = (1 << 10),
            DS_BLOCK_FAILED    = (1 << 11),
            DS_BLOCK_CM        = (1 << 12),
            DS_BLOCK_FORMAT    = (1 << 13),

            DS_CHECKS_COUNT = 14,
        };

        // keep in sync with socket1 output
        enum eUnderlayBlockReason : uint16_t {
            UL_OK = 0,

            UL_BLOCK_UNKNOWN   = (1 << 0),
            UL_BLOCK_USER      = (1 << 1),
            UL_BLOCK_PLANE     = (1 << 2),
            UL_BLOCK_MIRROR    = (1 << 3),
            UL_BLOCK_RECORD    = (1 << 4),
            UL_BLOCK_SW        = (1 << 5),
            UL_BLOCK_SHADER    = (1 << 6),
            UL_BLOCK_TEARING   = (1 << 7),
            UL_BLOCK_CANDIDATE = (1 << 8),
            UL_BLOCK_SURFACE   = (1 << 9),
            UL_BLOCK_TRANSFORM = (1 << 10),
            UL_BLOCK_FORMAT    = (1 << 11),
            UL_BLOCK_FAILED    = (1 << 12),
            UL_BLOCK_CM        = (1 << 13),
            UL_BLOCK_BLUR      = (1 << 14),

            UL_CHECKS_COUNT = 15,
        };

        // keep in sync with socket1 output
        enum eSolitaryCheck : uint32_t {
            SC_OK = 0,

            SC_UNKNOWN      = (1 << 0),
            SC_NOTIFICATION = (1 << 1),
            SC_LOCK         = (1 << 2),
            SC_WORKSPACE    = (1 << 3),
            SC_WINDOWED     = (1 << 4),
            SC_DND          = (1 << 5),
            SC_SPECIAL      = (1 << 6),
            SC_ALPHA        = (1 << 7),
            SC_OFFSET       = (1 << 8),
            SC_CANDIDATE    = (1 << 9),
            SC_OPAQUE       = (1 << 10),
            SC_TRANSFORM    = (1 << 11),
            SC_OVERLAYS     = (1 << 12),
            SC_FLOAT        = (1 << 13),
            SC_WORKSPACES   = (1 << 14),
            SC_SURFACES     = (1 << 15),
            SC_ERRORBAR     = (1 << 16),
            SC_FADEOUT      = (1 << 17),

            SC_CHECKS_COUNT = 18,
        };

        // keep in sync with socket1 output
        enum eTearingCheck : uint8_t {
            TC_OK = 0,

            TC_UNKNOWN   = (1 << 0),
            TC_NOT_TORN  = (1 << 1),
            TC_USER      = (1 << 2),
            TC_ZOOM      = (1 << 3),
            TC_SUPPORT   = (1 << 4),
            TC_CANDIDATE = (1 << 5),
            TC_WINDOW    = (1 << 6),
            TC_HW_CURSOR = (1 << 7),

            TC_CHECKS_COUNT = 8,
        };

        // methods
        void         onConnect(bool noRule);
        void         onDisconnect(bool destroy = false);
        void         emitReservedChangedIfNeeded();
        void         applyCMType(NCMType::eCMType cmType, NTransferFunction::eTF cmSdrEotf);
        void         addDamage(const pixman_region32_t* rg);
        void         addDamage(const CRegion& rg);
        void         addDamage(const CBox& box);
        void         scheduleFrame(Aquamarine::IOutput::scheduleFrameReason reason = Aquamarine::IOutput::AQ_SCHEDULE_CLIENT_UNKNOWN);
        bool         shouldSkipScheduleFrameOnMouseEvent();
        void         setMirror(const std::string&);
        bool         isMirror();
        float        getDefaultScale();
        void         changeWorkspace(const PHLWORKSPACE& pWorkspace, bool internal = false, bool noMouseMove = false, bool noFocus = false);
        void         setSpecialWorkspace(const PHLWORKSPACE& pWorkspace, bool noFocus = false);
        PHLWORKSPACE getCurrentWorkspace();
        void         scheduleDone();
        uint32_t     isSolitaryBlocked(bool full = false);
        void         recheckSolitary();
        uint8_t      isTearingBlocked(bool full = false);
        void         updateSurfaceScaleTransformDetails();
        bool         updateTearing();
        uint16_t     isDSBlocked(bool full = false);
        bool         attemptDirectScanout();
        void         handleDSleave();
        bool         canAttemptDirectScanoutFast() const;
        uint16_t     isUnderlayBlocked(bool full = false);
        bool         prepareUnderlay(bool commit);                                    // before a frame: true if it renders into the overlay
        bool         commitUnderlay(std::optional<CDamageRing::CTransaction> damage); // after it: the video on the primary, the frame over it
        void         leaveUnderlay();                                                 // the overlay goes off with the next commit
        void         underlayFailed();
        void         scheduleUnderlayFrameEvents(); // a skipped underlay frame: the clients' frame events at the refresh
        void         sendUnderlayFrameEvents();
        bool         isMultiGPU();
        void         setCTM(const Mat3x3& ctm);
        void         onCursorMovedOnMonitor();
        void         setDPMS(bool on);
        bool         shouldUseSoftwareCursors();

        // IMonitorQueryable / IMonitorArrangeable
        virtual MONITORID                   id() const override;
        virtual std::string_view            name() const override;
        virtual std::string_view            description() const override;
        virtual std::string_view            shortDescription() const override;
        virtual bool                        matchesStaticSelector(std::string_view selector) const override;
        virtual Vector2D                    position() const override;
        virtual Vector2D                    size() const override;
        virtual Vector2D                    pixelSize() const override;
        virtual Vector2D                    transformedSize() const override;
        virtual float                       scale() const override;
        virtual Hyprutils::Math::eTransform transform() const override;
        virtual CBox                        logicalBox() const override;
        virtual CBox                        logicalBoxMinusReserved() const override;
        virtual Vector2D                    middle() const override;
        virtual void                        moveTo(const Vector2D& pos) override;
        virtual bool                        enabled() const override;
        virtual bool                        hasOutput() const override;
        virtual SP<Aquamarine::IOutput>     output() const override;
        virtual std::optional<Vector2D>     explicitPosition() const override;
        virtual Config::eAutoDirs           autoDirection() const override;
        virtual Vector2D                    xwaylandPosition() const override;
        virtual float                       xwaylandScale() const override;
        virtual void                        setXWaylandPosition(const Vector2D& pos) override;
        virtual void                        setXWaylandScale(float scale) override;

        //
        bool applyMonitorRule(Config::CMonitorRule&& pMonitorRule);
        bool applyMonitorRuleSoft(Config::CMonitorRule&& pMonitorRule);

        //
        const Mat3x3&                                               getTransformMatrix();
        const Mat3x3&                                               getScaleMatrix();

        void                                                        debugLastPresentation(const std::string& message);

        bool                                                        supportsWideColor();
        bool                                                        supportsHDR();
        float                                                       minLuminance(float defaultValue = 0);
        int                                                         maxLuminance(int defaultValue = 80);
        int                                                         maxAvgLuminance(int defaultValue = 80);
        float                                                       maxFALL();
        float                                                       maxCLL();

        bool                                                        wantsWideColor();
        bool                                                        wantsHDR();

        bool                                                        inHDR();
        bool                                                        gammaRampsInUse();

        std::optional<NColorManagement::PImageDescription>          getFSImageDescription();
        std::optional<NColorManagement::PImageDescription>          getAutoHDRImageDescription(const SP<CWLSurfaceResource>& surface);

        NColorManagement::SPCPRimaries                              getMasteringPrimaries();
        NColorManagement::SImageDescription::SPCMasteringLuminances getMasteringLuminances();
        NColorManagement::PImageDescription                         getHDRImageDescription(NCMType::eCMType cmType);

        uint32_t                                                    getPreferredReadFormat();

        bool                                                        needsCM();
        /// Can do CM without shader (forDSmode ? check output image description : check workbuffer image description)
        bool                                canNoShaderCM(bool forDSmode = false);
        bool                                canNoShaderCM(const NColorManagement::PImageDescription& source, bool forDSmode);
        bool                                doesNoShaderCM();

        bool                                m_enabled             = false;
        bool                                m_renderingInitPassed = false;

        PHLWINDOWREF                        m_previousFSWindow;
        PHLWINDOWREF                        m_previousCTMWindow; // the window the CTM was last set for: the fullscreen one, or an underlay's
        bool                                m_previousFSVideo = false;
        PHLWINDOWREF                        m_autoHDRWindow;
        bool                                m_needsHDRupdate         = false;
        bool                                m_hdrMetadataFromSurface = false;

        std::optional<dev_t>                m_cachedAllocatorDRMDev;
        std::optional<dev_t>                m_cachedCompositorDRMDev;
        int                                 m_cachedAllocatorDRMFD  = -1;
        int                                 m_cachedCompositorDRMFD = -1;
        std::optional<bool>                 m_cachedSameGPU;

        NColorManagement::PImageDescription m_imageDescription = NColorManagement::CImageDescription::from(NColorManagement::SImageDescription{});
        bool                                m_noShaderCTM      = false; // sets drm CTM, restore needed

        bool                                m_blurFBDirty        = true;
        bool                                m_blurFBShouldRender = false;

        // For the list lookup

        bool operator==(const CMonitor& rhs) {
            return m_position == rhs.m_position && m_size == rhs.m_size && m_name == rhs.m_name;
        }

        bool                                needsACopyFB();
        bool                                needsUnmodifiedCopy();
        bool                                useFP16();
        NColorManagement::PImageDescription workBufferImageDescription();
        WP<Monitor::CMonitorResources>      resources();

      private:
        void                updateMatrix();
        Mat3x3              m_projMatrix;
        Mat3x3              m_projOutputMatrix;

        void                setSpecialWorkspaceVisualState(bool active);
        void                commitDPMSState(bool state);
        void                scheduleModeRetry();
        void                clearModeRetry();
        void                updateVCGTRamps();
        bool                trySetFormat(std::span<const uint32_t> formats);
        bool                primaryTakes(uint32_t format, uint64_t modifier);

        bool                m_doneScheduled  = false;
        bool                m_vcgtRampsSet   = false;
        int                 m_modeRetryCount = 0;
        SP<CEventLoopTimer> m_modeRetryTimer;

        // primaryTakes()'s last answer: the primary plane's formats are fixed
        // for an output, a client's buffer layout changes rarely
        struct {
            uint32_t format   = DRM_FORMAT_INVALID;
            uint64_t modifier = DRM_FORMAT_MOD_INVALID;
            bool     takes    = false;
        } m_primaryTakesCache;

        // a surface's buffer layout whose direct-scanout or underlay test failed: not tried
        // again, as each try moves the swapchain to its format and back
        struct SScanoutFailure {
            WP<CWLSurfaceResource>                     surface;
            uint32_t                                   format   = DRM_FORMAT_INVALID;
            uint64_t                                   modifier = DRM_FORMAT_MOD_INVALID;
            Aquamarine::COutputState::SBufferPlacement placement;
        };
        SScanoutFailure m_dsFailedOn;
        SScanoutFailure m_underlayFailedOn;

        // a video subsurface over a black fullscreen main surface, scanned out on the primary plane
        std::optional<Aquamarine::COutputState::SBufferPlacement> videoPlacement(PHLWINDOW window, SP<CWLSurfaceResource> video);
        bool                                                      videoBlockedByCM(SP<CWLSurfaceResource> video, const Aquamarine::COutputState::SBufferPlacement& placement);
        // a video at a global logical box, placed on the primary plane
        std::optional<Aquamarine::COutputState::SBufferPlacement> placementAt(SP<CWLSurfaceResource> video, const CBox& box);
        // an underlay's video, anywhere in its window's tree, within the plane's scaling
        struct SUnderlayCandidate {
            PHLWINDOW              window;
            SP<CWLSurfaceResource> surface; // null: the window has none to take
            bool                   video = true;
        };
        std::optional<SUnderlayCandidate>                         underlayCandidate();
        bool                                                      underlayBlurredOver(PHLWINDOW window, const CBox& hole);
        NColorManagement::PImageDescription                       underlayContentDescription(PHLWINDOW window, SP<CWLSurfaceResource> content);
        std::optional<Aquamarine::COutputState::SBufferPlacement> underlayPlacement(PHLWINDOW window, SP<CWLSurfaceResource> video);
        bool underlayBlockedByCM(PHLWINDOW window, SP<CWLSurfaceResource> video, const Aquamarine::COutputState::SBufferPlacement& placement);
        bool ensureUnderlaySwapchain();

        // Resources
        UP<Monitor::CMonitorResources> m_resources;
        // cached should contain one of predefined descriptions for FP16: sRGB primaries with either linear TF by default and in HDR mode with render:hdr_blend_space = linear,
        // extended sRGB with SDR content's range in HDR mode with render:hdr_blend_space = srgb, or monitor's TF in SDR with render:fp16_sdr_tf = 0
        // avoids lookup for an id when ::from is used
        NColorManagement::PImageDescription m_cachedInternalDescription = NColorManagement::CImageDescription::from(NColorManagement::SImageDescription{});

        struct {
            CHyprSignalListener frame;
            CHyprSignalListener destroy;
            CHyprSignalListener state;
            CHyprSignalListener needsFrame;
            CHyprSignalListener presented;
            CHyprSignalListener commit;
            CHyprSignalListener commitResult;
        } m_listeners;

        int   m_supportsWideColor = 0;
        int   m_supportsHDR       = 0;
        float m_minLuminance      = -1.0f;
        int   m_maxLuminance      = -1;
        int   m_maxAvgLuminance   = -1;
    };
}
