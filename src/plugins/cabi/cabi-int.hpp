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
