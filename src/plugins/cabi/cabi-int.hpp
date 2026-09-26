// cabi-int.hpp — C++-only internals for the cabi C ABI. Not installed; the
// installed/parsed header is cabi.h (valid C). This file names the handle
// struct layouts and the context the loader constructs.
#pragma once

#include "cabi.h"

#include <array>
#include <atomic>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "../../helpers/memory/Memory.hpp"
#include "../../desktop/DesktopTypes.hpp"
#include "../../helpers/signal/Signal.hpp"
#include "../../managers/eventLoop/EventLoopManager.hpp" // SReadableWaiter, CEventLoopTimer
#include "../../config/values/types/IValue.hpp"
#include "../../render/Texture.hpp"                        // Render::ITexture
#include "../../render/pass/PassElement.hpp"                // IPassElement, ePassElementType
#include "../../SharedDefs.hpp"                             // eRenderStage
#include "../../output/Monitor.hpp"                         // Monitor::CMonitor (m_scale, logicalBox, m_position)
#include "../../devices/IPointer.hpp"                         // IPointer (hl_pointer handle)

// The handle structs. Each holds a WEAK reference to the compositor object
// plus an atomic refcount of how many plugin-side references are live. The
// window/workspace/monitor itself is never kept alive by the handle — an
// expired object surfaces as HL_E_NOT_FOUND on query.
struct hl_window {
    PHLWINDOWREF    ref;
    std::atomic<int> rc{ 1 };
    explicit hl_window(PHLWINDOWREF r) : ref(r) {}
};

struct hl_workspace {
    PHLWORKSPACEREF    ref;
    std::atomic<int>   rc{ 1 };
    explicit hl_workspace(PHLWORKSPACEREF r) : ref(r) {}
};

struct hl_monitor {
    PHLMONITORREF    ref;
    std::atomic<int> rc{ 1 };
    explicit hl_monitor(PHLMONITORREF r) : ref(r) {}
};

// A pointer (mouse, touchpad, tablet pointer, virtual). Wraps a WEAK ref to
// the compositor's IPointer; an expired object surfaces as HL_E_NOT_FOUND.
struct hl_pointer {
    WP<IPointer>     ref;
    std::atomic<int> rc{ 1 };
    explicit hl_pointer(WP<IPointer> r) : ref(r) {}
};

// A refcounted GPU texture. Built in the warm pass (outside a frame) and
// painted by a later frame's draw (crash class 4). Wraps the compositor's
// SP<Render::ITexture>; the refcount is how many plugin-side references are live.
struct hl_texture {
    SP<Render::ITexture> tex;
    std::atomic<int>     rc{ 1 };
    explicit hl_texture(SP<Render::ITexture> t) : tex(std::move(t)) {}
};

// A paint canvas: the trampoline builds one per frame for the monitor being
// rendered and hands it to the plugin's draw callback. Coordinates are
// monitor-local LOGICAL px (0,0 = the monitor's top-left); the canvas scales
// to monitor-local physical for the renderer. `bounds` points at the element's
// bounding box (the plugin sets it so the pass can optimize).
struct hl_canvas {
    PHLMONITORREF mon;
    float         scale   = 1.0f;
    CBox          logical {};
    CBox*         bounds  = nullptr;
};

// The trampoline pass element: the fork adds one per frame (per render
// callback) at the selected stage. Its draw() builds a canvas and invokes the
// plugin's C callback, which paints imperatively through the canvas (the
// callback never returns elements — the C++ bar does the same).
class CCabiPassElement : public IPassElement {
  public:
    CCabiPassElement(PHLMONITOR mon, hl_draw_fn draw, void* ud) : m_mon(mon), m_draw(draw), m_ud(ud) {}
    virtual ~CCabiPassElement() = default;

    virtual std::vector<UP<IPassElement>> draw() override {
        auto M = m_mon.lock();
        if (!M || !m_draw)
            return {};
        hl_canvas cv{};
        cv.mon     = m_mon;
        cv.scale   = M->m_scale;
        cv.logical = M->logicalBox();
        cv.bounds  = &m_bounds;
        m_draw(&cv, m_ud);
        return {};
    }
    virtual bool                needsLiveBlur()       override { return false; }
    virtual bool                needsPrecomputeBlur() override { return false; }
    virtual const char*         passName()            override { return "CCabiPassElement"; }
    virtual ePassElementType    type()                override { return EK_CUSTOM; }
    virtual std::optional<CBox> boundingBox()         override {
        if (m_bounds.w > 0 && m_bounds.h > 0)
            return m_bounds;
        auto M = m_mon.lock();
        if (!M)
            return std::nullopt;
        const auto B = M->logicalBox();
        return CBox{0, 0, B.size().x, B.size().y};
    }

  private:
    PHLMONITORREF m_mon;
    hl_draw_fn    m_draw;
    void*         m_ud;
    CBox          m_bounds{ 0, 0, 0, 0 }; // monitor-local logical; set by the plugin
};

class CCabiCtx {
  public:
    CCabiCtx() { m_thread = std::this_thread::get_id(); }
    ~CCabiCtx() { shutdown(); }

    // The event-loop thread every hl_* call must run on (captured at
    // creation, which happens on that thread from the loader).
    std::thread::id m_thread;
    WP<CCabiCtx>    m_weak; // set by cabiCreateCtx; jobs/listeners hold this

    bool m_shutdown = false;

    // The single dispatcher (the plugin subscribes once per ctx).
    hl_dispatch_fn  m_dispatch = nullptr;
    void*           m_ud       = nullptr;
    hl_event_mask_t m_mask     = 0;
    std::vector<CHyprSignalListener> m_listeners; // kept alive = subscribed

    // The plugin handle (set by the loader): needed to register config values.
    void* m_handle = nullptr;

    // Registered config values, keyed by the opaque handle we hand out.
    struct SConfigVal {
        uint32_t                 type = 0;
        SP<Config::Values::IValue> val;
    };
    std::unordered_map<void*, SConfigVal> m_config;

    // Render callbacks. A single render-stage listener (added on the first
    // hl_render_listen, kept in m_listeners) walks this list each frame and
    // adds a trampoline element for every callback whose stage matches. `stage`
    // stores the compositor's eRenderStage value (mapped from the compact cabi
    // index in hl_render_listen). SPs so the object (the opaque handle we hand
    // out) stays put across reallocation.
    struct SRenderListener {
        uint32_t   stage = 0;
        hl_draw_fn draw  = nullptr;
        void*      ud    = nullptr;
    };
    std::vector<SP<SRenderListener>> m_renders;

    // String scratch: a small rotating pool so a single query can return
    // several distinct strings (e.g. window appID + title). Each hl_str_t
    // points into one buffer and stays valid until that buffer is reused by
    // a later scratch call — the plugin reads/copies them before its next
    // string-producing query.
    static constexpr size_t SCRATCH_N = 4;
    std::array<std::string, SCRATCH_N> m_scratch{};
    size_t                             m_scratchIdx = 0;
    hl_str_t                           scratch(const std::string& s) {
        auto& buf = m_scratch[m_scratchIdx % SCRATCH_N];
        buf       = s;
        m_scratchIdx += 1;
        return hl_str_t{ buf.data(), (uint32_t) buf.size() };
    }

    // Jobs.
    enum eJobKind : uint8_t { KIND_DEFER = 0, KIND_TIMER, KIND_FD };
    struct SJob {
        uint64_t        token = 0;
        eJobKind        kind  = KIND_DEFER;
        bool            active = false;
        uint64_t        doLaterSeq = 0;      // KIND_DEFER
        SP<CEventLoopTimer> timer;           // KIND_TIMER
        WP<SReadableWaiter> waiter;          // KIND_FD
        hl_job_fn       fn    = nullptr;
        void*           ud    = nullptr;
    };
    std::unordered_map<uint64_t, UP<SJob>> m_jobs;
    uint64_t m_nextJobToken = 1;

    void shutdown();

    void cancelJobInternal(UP<SJob>& job);

    friend class CPluginSystem;
};

// Loader-facing: creates the ctx and returns the OWNING SP (the loader keeps
// it in CPlugin::m_cabiCtx for the load lifetime). The single strong owner is
// that SP; jobs/listeners hold the self-weak m_weak, which expires on reset.
SP<CCabiCtx> cabiCreateCtx();
