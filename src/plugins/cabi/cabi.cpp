// cabi.cpp — the C ABI implementation. Every function: thread-check, then a
// try/catch so a throwing compositor call becomes HL_E_FAILED, never an
// exception across the boundary (crash class 2, neutralized here).
#include "cabi-int.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "../../event/EventBus.hpp"
#include "../../desktop/state/FocusState.hpp"
#include "../../desktop/state/ViewState.hpp"
#include "../../desktop/state/WindowState.hpp"
#include "../../state/MonitorState.hpp"
#include "../../state/WorkspaceState.hpp"
#include "../../managers/eventLoop/EventLoopManager.hpp"
#include "../../managers/SessionLockManager.hpp"
#include "../../managers/input/InputManager.hpp"
#include "../../managers/SeatManager.hpp"
#include "../../managers/fullscreen/FullscreenController.hpp"
#include "../../desktop/view/window/Window.hpp"
#include "../../desktop/view/window/WaylandBackend.hpp"
#include "../../protocols/XDGShell.hpp"
#include "../../layout/LayoutManager.hpp"
#include "../../layout/target/Target.hpp"
#include "../../layout/target/WindowTarget.hpp"
#include "../../output/Monitor.hpp"
#include "../../workspace/HLWorkspace.hpp"
#include "../../debug/log/Logger.hpp"
#include "../../helpers/math/Math.hpp"
#include "../../defines.hpp"
#include "../../config/shared/Types.hpp"
#include "../../config/ConfigManager.hpp"
#include "../../config/values/types/IntValue.hpp"
#include "../../config/values/types/BoolValue.hpp"
#include "../../config/values/types/FloatValue.hpp"
#include "../../config/values/types/StringValue.hpp"
#include "../../plugins/PluginAPI.hpp"
#include "../../render/Renderer.hpp"                   // g_pHyprRenderer, ITexture, addPassElement, damageBox, createTexture, renderText
#include "../../render/OpenGL.hpp"                      // Render::GL::g_pHyprOpenGL (renderRect/Border/Texture)
#include "../../config/shared/complex/ComplexDataTypes.hpp" // Config::CGradientValueData (renderBorder)

#include <hyprutils/os/FileDescriptor.hpp>

#include <libdrm/drm_fourcc.h> // DRM_FORMAT_XRGB8888 (software RGBA textures)

#include <algorithm>

// The ABI version the plugin was built against. Bump on any breaking cabi.h
// change; the plugin ejects on mismatch.
static constexpr uint32_t CABI_ABI_VERSION = 1;

namespace {
    // Every entry (except cabiAbiVersion) runs on the event-loop thread.
    inline bool cabiThreadOk(const CCabiCtx* ctx) {
        return std::this_thread::get_id() == ctx->m_thread;
    }

    // Build a plugin-owned handle (one ref) from a live strong ref.
    inline hl_window* makeWindow(PHLWINDOW w) {
        return new hl_window(PHLWINDOWREF(w));
    }
    inline hl_workspace* makeWorkspace(PHLWORKSPACE ws) {
        return new hl_workspace(PHLWORKSPACEREF(ws));
    }
    inline hl_monitor* makeMonitor(PHLMONITOR m) {
        return new hl_monitor(PHLMONITORREF(m));
    }

    // Map the fork's fullscreen mode to the ABI's 0/1/2 encoding
    // (FSMODE_NONE=0, FSMODE_MAXIMIZED=1, FSMODE_FULLSCREEN=2).
    inline uint32_t fsMode(const Fullscreen::eFullscreenMode m) {
        switch (m) {
            case Fullscreen::FSMODE_MAXIMIZED: return 1;
            case Fullscreen::FSMODE_FULLSCREEN: return 2;
            default: return 0;
        }
    }
}

// =======================================================================
// version + lifecycle
// =======================================================================

uint32_t cabiAbiVersion() {
    return CABI_ABI_VERSION;
}

SP<CCabiCtx> cabiCreateCtx() {
    auto sp = makeShared<CCabiCtx>();
    sp->m_weak = sp; // self-weak for jobs/listeners; expires when the loader resets the SP
    return sp;
}

hl_error_t hl_shutdown(hl_ctx* c) {
    try {
        auto* ctx = reinterpret_cast<CCabiCtx*>(c);
        if (!ctx)
            return HL_E_ARG;
        if (!cabiThreadOk(ctx))
            return HL_E_THREAD;
        ctx->shutdown();
        return HL_E_OK;
    } catch (const std::exception&) {
        return HL_E_FAILED;
    } catch (...) {
        return HL_E_FAILED;
    }
}

// =======================================================================
// context implementation
// =======================================================================

void CCabiCtx::cancelJobInternal(UP<SJob>& job) {
    if (!job || !job->active)
        return;
    job->active = false;
    switch (job->kind) {
        case KIND_DEFER:
            if (job->doLaterSeq)
                g_pEventLoopManager->removeDoLater(job->doLaterSeq);
            break;
        case KIND_TIMER:
            if (job->timer) {
                job->timer->cancel();
                g_pEventLoopManager->removeTimer(job->timer);
                job->timer.reset();
            }
            break;
        case KIND_FD:
            if (job->waiter) {
                g_pEventLoopManager->removeReadableWaiter(job->waiter);
                job->waiter.reset();
            }
            break;
    }
}

void CCabiCtx::shutdown() {
    if (m_shutdown)
        return;
    m_shutdown = true;

    // Jobs first: cancel every pending job so none can fire into an
    // unmapped .so (crash class 2 at the boundary).
    for (auto& [token, job] : m_jobs)
        cancelJobInternal(job);
    m_jobs.clear();

    // Then listeners: drop every subscription so no event reaches a torn-down
    // dispatcher (this also drops the render-stage listener, so no trampoline
    // element is added after teardown).
    m_listeners.clear();
    m_dispatch = nullptr;
    m_ud       = nullptr;
    m_mask     = 0;
    m_renders.clear();

    for (auto& s : m_scratch)
        s.clear();
    m_scratchIdx = 0;
}

// =======================================================================
// log
// =======================================================================

void hl_log(hl_ctx* c, uint32_t level, const char* fmt, ...) {
    auto* ctx = reinterpret_cast<CCabiCtx*>(c);
    if (!ctx)
        return;

    char    buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt ? fmt : "", ap);
    va_end(ap);

    std::string msg = std::format("[awesome] {}", buf);
    hl_log_str(c, level, msg.c_str());
}

void hl_log_str(hl_ctx* c, uint32_t level, const char* msg) {
    auto* ctx = reinterpret_cast<CCabiCtx*>(c);
    if (!ctx)
        return;
    std::string text = std::format("[awesome] {}", msg ? msg : "");
    try {
        switch (level) {
            case 1: Log::logger->log(Log::INFO, Log::logFnName(), text); break;
            case 2: Log::logger->log(Log::WARN, Log::logFnName(), text); break;
            case 3: Log::logger->log(Log::ERR, Log::logFnName(), text); break;
            default: Log::logger->log(Log::DEBUG, Log::logFnName(), text); break;
        }
    } catch (...) {
        // logging must never throw across the boundary
    }
}

// =======================================================================
// handle ref/unref
// =======================================================================

void hl_window_ref(hl_window* w) {
    if (w)
        w->rc.fetch_add(1, std::memory_order_relaxed);
}
void hl_window_unref(hl_window* w) {
    if (w && w->rc.fetch_sub(1, std::memory_order_acq_rel) == 1)
        delete w;
}
void hl_workspace_ref(hl_workspace* w) {
    if (w)
        w->rc.fetch_add(1, std::memory_order_relaxed);
}
void hl_workspace_unref(hl_workspace* w) {
    if (w && w->rc.fetch_sub(1, std::memory_order_acq_rel) == 1)
        delete w;
}
void hl_monitor_ref(hl_monitor* m) {
    if (m)
        m->rc.fetch_add(1, std::memory_order_relaxed);
}
void hl_monitor_unref(hl_monitor* m) {
    if (m && m->rc.fetch_sub(1, std::memory_order_acq_rel) == 1)
        delete m;
}

// =======================================================================
// string model
// =======================================================================

hl_error_t hl_strdup(hl_ctx* c, hl_str_t src, hl_str_t* out) {
    try {
        auto* ctx = reinterpret_cast<CCabiCtx*>(c);
        if (!ctx)
            return HL_E_ARG;
        if (!cabiThreadOk(ctx))
            return HL_E_THREAD;
        if (!out)
            return HL_E_ARG;
        // The strdup result is heap-owned and outlives the call.
        std::string s = src.d ? std::string(src.d, src.l) : std::string();
        char*       p = new char[s.size() + 1];
        if (!s.empty())
            memcpy(p, s.data(), s.size());
        p[s.size()] = '\0';
        out->d = p;
        out->l = (uint32_t) s.size();
        return HL_E_OK;
    } catch (const std::exception&) {
        return HL_E_FAILED;
    } catch (...) {
        return HL_E_FAILED;
    }
}

void hl_free(hl_ctx* c, void* p) {
    auto* ctx = reinterpret_cast<CCabiCtx*>(c);
    if (!ctx || !p)
        return;
    delete[] static_cast<char*>(p);
}

// =======================================================================
// config
// =======================================================================

hl_error_t hl_config_register(hl_ctx* c, const char* key, const char* desc,
    uint32_t type, double num_default, const char* str_default, void** out) {
    try {
        auto* ctx = reinterpret_cast<CCabiCtx*>(c);
        if (!ctx)
            return HL_E_ARG;
        if (!cabiThreadOk(ctx))
            return HL_E_THREAD;
        if (ctx->m_shutdown)
            return HL_E_STATE;
        if (!key || !*key || !out)
            return HL_E_ARG;

        auto makeVal = [&]() -> SP<Config::Values::IValue> {
            const char* D = desc ? desc : "";
            switch (type) {
                case HL_CFG_INT:
                    return makeShared<Config::Values::CIntValue>(key, D, (Config::INTEGER) num_default);
                case HL_CFG_BOOL:
                    return makeShared<Config::Values::CBoolValue>(key, D, num_default != 0.0);
                case HL_CFG_FLOAT:
                    return makeShared<Config::Values::CFloatValue>(key, D, (Config::FLOAT) num_default);
                case HL_CFG_STRING:
                    return makeShared<Config::Values::CStringValue>(key, D, std::string(str_default ? str_default : ""));
                default:
                    return SP<Config::Values::IValue>();
            }
        };

        auto v = makeVal();
        if (!v)
            return HL_E_ARG;

        if (!HyprlandAPI::addConfigValueV2(ctx->m_handle, v))
            return HL_E_FAILED;

        void* h = v.get();
        ctx->m_config.emplace(h, CCabiCtx::SConfigVal{ type, std::move(v) });
        *out    = h;
        return HL_E_OK;
    } catch (const std::exception&) {
        return HL_E_FAILED;
    } catch (...) {
        return HL_E_FAILED;
    }
}

hl_error_t hl_config_get(hl_ctx* c, void* h, uint32_t* type, double* num, hl_str_t* str) {
    try {
        auto* ctx = reinterpret_cast<CCabiCtx*>(c);
        if (!ctx)
            return HL_E_ARG;
        if (!cabiThreadOk(ctx))
            return HL_E_THREAD;
        if (!h)
            return HL_E_ARG;
        auto it = ctx->m_config.find(h);
        if (it == ctx->m_config.end())
            return HL_E_NOT_FOUND;
        const auto& cv = it->second;
        if (type)
            *type = cv.type;
        switch (cv.type) {
            case HL_CFG_INT: {
                auto v = dynamicPointerCast<Config::Values::CIntValue>(cv.val);
                if (num && v)
                    *num = (double) v->value();
                break;
            }
            case HL_CFG_BOOL: {
                auto v = dynamicPointerCast<Config::Values::CBoolValue>(cv.val);
                if (num && v)
                    *num = v->value() ? 1.0 : 0.0;
                break;
            }
            case HL_CFG_FLOAT: {
                auto v = dynamicPointerCast<Config::Values::CFloatValue>(cv.val);
                if (num && v)
                    *num = (double) v->value();
                break;
            }
            case HL_CFG_STRING: {
                auto v = dynamicPointerCast<Config::Values::CStringValue>(cv.val);
                if (str && v)
                    *str = ctx->scratch(v->value());
                break;
            }
            default:
                return HL_E_ARG;
        }
        return HL_E_OK;
    } catch (const std::exception&) {
        return HL_E_FAILED;
    } catch (...) {
        return HL_E_FAILED;
    }
}

// =======================================================================
// queries
// =======================================================================

hl_error_t hl_window_get(hl_ctx* c, hl_window* wh,
    hl_str_t* app_id, hl_str_t* title, hl_box_t* at, hl_box_t* size,
    uint32_t* fullscreen, uint32_t* fullscreen_client,
    uint32_t* floating, uint32_t* pinned, uint32_t* visible,
    uint32_t* allowed_over_fullscreen, uint32_t* urgent) {
    try {
        auto* ctx = reinterpret_cast<CCabiCtx*>(c);
        if (!ctx)
            return HL_E_ARG;
        if (!cabiThreadOk(ctx))
            return HL_E_THREAD;
        if (!wh)
            return HL_E_ARG;
        auto W = wh->ref.lock();
        if (!W)
            return HL_E_NOT_FOUND;

        const auto BOX = W->layoutBox();
        if (app_id)
            *app_id = ctx->scratch(W->metadata().appID());
        if (title)
            *title = ctx->scratch(W->metadata().title());
        if (at) {
            at->x = BOX.pos().x;
            at->y = BOX.pos().y;
            at->w = BOX.size().x;
            at->h = BOX.size().y;
        }
        if (size) {
            size->x = BOX.pos().x;
            size->y = BOX.pos().y;
            size->w = BOX.size().x;
            size->h = BOX.size().y;
        }
        const auto FS = Fullscreen::controller()->getFullscreenModes(W);
        if (fullscreen)
            *fullscreen = fsMode(FS.internal);
        if (fullscreen_client)
            *fullscreen_client = fsMode(FS.client);
        if (floating)
            *floating = W->isFloating() ? 1 : 0;
        if (pinned)
            *pinned = (W->m_state & Desktop::View::WINDOW_STATE_PINNED) ? 1 : 0;
        if (visible)
            *visible = (W->mapped() && !W->isHidden()) ? 1 : 0;
        if (allowed_over_fullscreen)
            *allowed_over_fullscreen = W->isAllowedOverFullscreen() ? 1 : 0;
        if (urgent)
            *urgent = (W->m_hints & Desktop::View::WINDOW_HINT_URGENT) ? 1 : 0;
        return HL_E_OK;
    } catch (const std::exception&) {
        return HL_E_FAILED;
    } catch (...) {
        return HL_E_FAILED;
    }
}

uint64_t hl_window_id(hl_ctx* c, hl_window* wh) {
    try {
        auto* ctx = reinterpret_cast<CCabiCtx*>(c);
        if (!ctx || !cabiThreadOk(ctx) || !wh)
            return 0;
        auto W = wh->ref.lock();
        if (!W)
            return 0;
        return reinterpret_cast<uint64_t>(W.get());
    } catch (...) {
        return 0;
    }
}

hl_error_t hl_window_workspace(hl_ctx* c, hl_window* wh, hl_workspace** out) {
    try {
        auto* ctx = reinterpret_cast<CCabiCtx*>(c);
        if (!ctx)
            return HL_E_ARG;
        if (!cabiThreadOk(ctx))
            return HL_E_THREAD;
        if (!wh || !out)
            return HL_E_ARG;
        auto W = wh->ref.lock();
        if (!W)
            return HL_E_NOT_FOUND;
        auto ws = W->m_workspace; // strong ref already
        if (!ws)
            return HL_E_NOT_FOUND;
        *out = makeWorkspace(ws);
        return HL_E_OK;
    } catch (const std::exception&) {
        return HL_E_FAILED;
    } catch (...) {
        return HL_E_FAILED;
    }
}

hl_error_t hl_workspace_get(hl_ctx* c, hl_workspace* whs, hl_str_t* name, uint32_t* id, uint32_t* focused) {
    try {
        auto* ctx = reinterpret_cast<CCabiCtx*>(c);
        if (!ctx)
            return HL_E_ARG;
        if (!cabiThreadOk(ctx))
            return HL_E_THREAD;
        if (!whs)
            return HL_E_ARG;
        auto ws = whs->ref.lock();
        if (!ws)
            return HL_E_NOT_FOUND;
        if (name)
            *name = ctx->scratch(ws->displayName());
        if (id) {
            auto nid = ws->numberedID();
            *id      = nid ? *nid : 0;
        }
        if (focused) {
            auto mon = ws->m_monitor.lock();
            *focused = (mon && (mon->m_activeWorkspace == ws || mon->m_activeSpecialWorkspace == ws)) ? 1 : 0;
        }
        return HL_E_OK;
    } catch (const std::exception&) {
        return HL_E_FAILED;
    } catch (...) {
        return HL_E_FAILED;
    }
}

hl_error_t hl_monitor_get(hl_ctx* c, hl_monitor* wh, hl_str_t* name, hl_box_t* box, float* scale, uint32_t* focused) {
    try {
        auto* ctx = reinterpret_cast<CCabiCtx*>(c);
        if (!ctx)
            return HL_E_ARG;
        if (!cabiThreadOk(ctx))
            return HL_E_THREAD;
        if (!wh)
            return HL_E_ARG;
        auto M = wh->ref.lock();
        if (!M)
            return HL_E_NOT_FOUND;
        if (name)
            *name = ctx->scratch(M->m_name);
        if (box) {
            const auto B = M->logicalBox();
            box->x       = B.pos().x;
            box->y       = B.pos().y;
            box->w       = B.size().x;
            box->h       = B.size().y;
        }
        if (scale)
            *scale = M->m_scale;
        if (focused) {
            auto fm = Desktop::focusState()->monitor();
            *focused = (fm == M) ? 1 : 0;
        }
        return HL_E_OK;
    } catch (const std::exception&) {
        return HL_E_FAILED;
    } catch (...) {
        return HL_E_FAILED;
    }
}

hl_error_t hl_monitor_at(hl_ctx* c, double x, double y, hl_monitor** out) {
    try {
        auto* ctx = reinterpret_cast<CCabiCtx*>(c);
        if (!ctx)
            return HL_E_ARG;
        if (!cabiThreadOk(ctx))
            return HL_E_THREAD;
        if (!out)
            return HL_E_ARG;
        const Vector2D pos{x, y};
        auto           distToBox = [](const Vector2D& p, const CBox& b) {
            const double dx = p.x < b.pos().x ? b.pos().x - p.x : (p.x > b.pos().x + b.size().x ? p.x - (b.pos().x + b.size().x) : 0.0);
            const double dy = p.y < b.pos().y ? b.pos().y - p.y : (p.y > b.pos().y + b.size().y ? p.y - (b.pos().y + b.size().y) : 0.0);
            return dx * dx + dy * dy;
        };
        PHLMONITOR best;
        double     bestDist = 0.0;
        for (const auto& M : State::monitorState()->monitors()) {
            const auto BOX = M->logicalBox();
            if (BOX.containsPoint(pos)) {
                *out = makeMonitor(M);
                return HL_E_OK;
            }
            const double D = distToBox(pos, BOX);
            if (!best || D < bestDist) {
                best     = M;
                bestDist = D;
            }
        }
        if (!best)
            return HL_E_NOT_FOUND;
        *out = makeMonitor(best);
        return HL_E_OK;
    } catch (const std::exception&) {
        return HL_E_FAILED;
    } catch (...) {
        return HL_E_FAILED;
    }
}

hl_error_t hl_focus_window(hl_ctx* c, hl_window** out) {
    try {
        auto* ctx = reinterpret_cast<CCabiCtx*>(c);
        if (!ctx)
            return HL_E_ARG;
        if (!cabiThreadOk(ctx))
            return HL_E_THREAD;
        if (!out)
            return HL_E_ARG;
        auto w = Desktop::focusState()->window();
        if (!w)
            return HL_E_NOT_FOUND;
        *out = makeWindow(w);
        return HL_E_OK;
    } catch (const std::exception&) {
        return HL_E_FAILED;
    } catch (...) {
        return HL_E_FAILED;
    }
}

hl_error_t hl_focus_monitor(hl_ctx* c, hl_monitor** out) {
    try {
        auto* ctx = reinterpret_cast<CCabiCtx*>(c);
        if (!ctx)
            return HL_E_ARG;
        if (!cabiThreadOk(ctx))
            return HL_E_THREAD;
        if (!out)
            return HL_E_ARG;
        auto m = Desktop::focusState()->monitor();
        if (!m)
            return HL_E_NOT_FOUND;
        *out = makeMonitor(m);
        return HL_E_OK;
    } catch (const std::exception&) {
        return HL_E_FAILED;
    } catch (...) {
        return HL_E_FAILED;
    }
}

hl_error_t hl_window_at(hl_ctx* c, double x, double y, hl_window** out) {
    try {
        auto* ctx = reinterpret_cast<CCabiCtx*>(c);
        if (!ctx)
            return HL_E_ARG;
        if (!cabiThreadOk(ctx))
            return HL_E_THREAD;
        if (!out)
            return HL_E_ARG;
        const Vector2D pos{x, y};
        auto           w = Desktop::viewState()->hitTest().windowAt(pos, Desktop::View::FOCUS_PRIORITY);
        if (!w)
            return HL_E_NOT_FOUND;
        *out = makeWindow(w);
        return HL_E_OK;
    } catch (const std::exception&) {
        return HL_E_FAILED;
    } catch (...) {
        return HL_E_FAILED;
    }
}

uint32_t hl_windows(hl_ctx* c, hl_window** out, uint32_t cap) {
    try {
        auto* ctx = reinterpret_cast<CCabiCtx*>(c);
        if (!ctx || !cabiThreadOk(ctx))
            return 0;
        uint32_t n = 0;
        for (const auto& w : Desktop::windowState()->windows()) {
            if (out && n < cap)
                out[n] = makeWindow(w);
            n++;
        }
        return n;
    } catch (const std::exception&) {
        return 0;
    } catch (...) {
        return 0;
    }
}

uint32_t hl_monitors(hl_ctx* c, hl_monitor** out, uint32_t cap) {
    try {
        auto* ctx = reinterpret_cast<CCabiCtx*>(c);
        if (!ctx || !cabiThreadOk(ctx))
            return 0;
        uint32_t n = 0;
        for (const auto& m : State::monitorState()->monitors()) {
            if (out && n < cap)
                out[n] = makeMonitor(m);
            n++;
        }
        return n;
    } catch (const std::exception&) {
        return 0;
    } catch (...) {
        return 0;
    }
}

uint32_t hl_workspaces(hl_ctx* c, hl_workspace** out, uint32_t cap) {
    try {
        auto* ctx = reinterpret_cast<CCabiCtx*>(c);
        if (!ctx || !cabiThreadOk(ctx))
            return 0;
        uint32_t n = 0;
        for (const auto& ws : State::workspaceState()->workspaces()) {
            auto live = ws.lock();
            if (!live)
                continue;
            if (out && n < cap)
                out[n] = makeWorkspace(live);
            n++;
        }
        return n;
    } catch (const std::exception&) {
        return 0;
    } catch (...) {
        return 0;
    }
}

// =======================================================================
// window writes (geometry / maximize / focus)
// =======================================================================

// The client-facing xdg toplevel role resource (nullptr for X11 / unmapped /
// destroyed). Mirrors the plugin common's xdgToplevel(); the fork exposes
// CWaylandBackend::m_resource publicly for exactly this.
static SP<CXDGToplevelResource> cabiXdgToplevel(const PHLWINDOW& w) {
    if (!w || w->backend().isX11())
        return nullptr;
    const auto* wl = dynamic_cast<const Desktop::View::CWaylandBackend*>(&w->backend());
    if (!wl)
        return nullptr;
    const auto res = wl->m_resource.lock();
    return res ? res->m_toplevel.lock() : nullptr;
}

hl_error_t hl_window_set_geom(hl_ctx* c, hl_window* wh, double x, double y, double pw, double ph) {
    try {
        auto* ctx = reinterpret_cast<CCabiCtx*>(c);
        if (!ctx)
            return HL_E_ARG;
        if (!cabiThreadOk(ctx))
            return HL_E_THREAD;
        auto W = wh ? wh->ref.lock() : nullptr;
        if (!W || !W->windowTarget())
            return HL_E_NOT_FOUND;
        const CBox BOX{Vector2D{x, y}, Vector2D{pw, ph}};
        g_layoutManager->setTargetGeom(BOX, W->windowTarget());
        W->windowTarget()->warpPositionSize();
        return HL_E_OK;
    } catch (const std::exception&) {
        return HL_E_FAILED;
    } catch (...) {
        return HL_E_FAILED;
    }
}

hl_error_t hl_monitor_workarea(hl_ctx* c, hl_monitor* mh, hl_box_t* out) {
    try {
        auto* ctx = reinterpret_cast<CCabiCtx*>(c);
        if (!ctx || !out)
            return HL_E_ARG;
        if (!cabiThreadOk(ctx))
            return HL_E_THREAD;
        auto M = mh ? mh->ref.lock() : nullptr;
        if (!M)
            return HL_E_NOT_FOUND;
        const auto B = M->logicalBoxMinusReserved();
        out->x = B.pos().x;
        out->y = B.pos().y;
        out->w = B.size().x;
        out->h = B.size().y;
        return HL_E_OK;
    } catch (const std::exception&) {
        return HL_E_FAILED;
    } catch (...) {
        return HL_E_FAILED;
    }
}

hl_error_t hl_window_min_max_size(hl_ctx* c, hl_window* wh, hl_box_t* min, hl_box_t* max) {
    try {
        auto* ctx = reinterpret_cast<CCabiCtx*>(c);
        if (!ctx)
            return HL_E_ARG;
        if (!cabiThreadOk(ctx))
            return HL_E_THREAD;
        auto W = wh ? wh->ref.lock() : nullptr;
        if (!W || !W->windowTarget())
            return HL_E_NOT_FOUND;
        const double UNLIMITED = 1e9;
        if (min) {
            const auto mn = W->windowTarget()->minSize();
            min->x = 0; min->y = 0;
            min->w = mn ? mn->x : 0.0;
            min->h = mn ? mn->y : 0.0;
        }
        if (max) {
            const auto mx = W->windowTarget()->maxSize();
            max->x = 0; max->y = 0;
            max->w = mx ? mx->x : UNLIMITED;
            max->h = mx ? mx->y : UNLIMITED;
        }
        return HL_E_OK;
    } catch (const std::exception&) {
        return HL_E_FAILED;
    } catch (...) {
        return HL_E_FAILED;
    }
}

hl_error_t hl_window_set_fs_mode(hl_ctx* c, hl_window* wh, uint32_t internal, uint32_t client) {
    try {
        auto* ctx = reinterpret_cast<CCabiCtx*>(c);
        if (!ctx)
            return HL_E_ARG;
        if (!cabiThreadOk(ctx))
            return HL_E_THREAD;
        auto W = wh ? wh->ref.lock() : nullptr;
        if (!W)
            return HL_E_NOT_FOUND;
        const auto MAP = [](uint32_t v) -> std::optional<Fullscreen::eFullscreenMode> {
            if (v == 0xFFFFFFFFu)
                return std::nullopt; // unchanged
            switch (v) {
                case 1: return Fullscreen::FSMODE_MAXIMIZED;
                case 2: return Fullscreen::FSMODE_FULLSCREEN;
                default: return Fullscreen::FSMODE_NONE;
            }
        };
        Fullscreen::controller()->setFullscreenMode(W, MAP(internal), MAP(client));
        return HL_E_OK;
    } catch (const std::exception&) {
        return HL_E_FAILED;
    } catch (...) {
        return HL_E_FAILED;
    }
}

hl_error_t hl_window_set_toplevel_maximized(hl_ctx* c, hl_window* wh, uint32_t on) {
    try {
        auto* ctx = reinterpret_cast<CCabiCtx*>(c);
        if (!ctx)
            return HL_E_ARG;
        if (!cabiThreadOk(ctx))
            return HL_E_THREAD;
        auto W = wh ? wh->ref.lock() : nullptr;
        if (!W)
            return HL_E_NOT_FOUND;
        auto TOP = cabiXdgToplevel(W);
        if (!TOP)
            return HL_E_STATE;
        TOP->setMaximized(on != 0);
        return HL_E_OK;
    } catch (const std::exception&) {
        return HL_E_FAILED;
    } catch (...) {
        return HL_E_FAILED;
    }
}

uint32_t hl_window_told_maximized(hl_ctx* c, hl_window* wh) {
    try {
        auto* ctx = reinterpret_cast<CCabiCtx*>(c);
        if (!ctx || !cabiThreadOk(ctx))
            return 0;
        auto W = wh ? wh->ref.lock() : nullptr;
        if (!W)
            return 0;
        auto TOP = cabiXdgToplevel(W);
        if (!TOP)
            return 0;
        return std::ranges::contains(TOP->m_pendingApply.states, XDG_TOPLEVEL_STATE_MAXIMIZED) ? 1u : 0u;
    } catch (...) {
        return 0;
    }
}

hl_error_t hl_window_request_client_size(hl_ctx* c, hl_window* wh) {
    try {
        auto* ctx = reinterpret_cast<CCabiCtx*>(c);
        if (!ctx)
            return HL_E_ARG;
        if (!cabiThreadOk(ctx))
            return HL_E_THREAD;
        auto W = wh ? wh->ref.lock() : nullptr;
        if (!W)
            return HL_E_NOT_FOUND;
        W->requestClientSize();
        return HL_E_OK;
    } catch (const std::exception&) {
        return HL_E_FAILED;
    } catch (...) {
        return HL_E_FAILED;
    }
}

hl_error_t hl_window_send_window_size(hl_ctx* c, hl_window* wh, uint32_t force) {
    try {
        auto* ctx = reinterpret_cast<CCabiCtx*>(c);
        if (!ctx)
            return HL_E_ARG;
        if (!cabiThreadOk(ctx))
            return HL_E_THREAD;
        auto W = wh ? wh->ref.lock() : nullptr;
        if (!W)
            return HL_E_NOT_FOUND;
        W->sendWindowSize(force != 0);
        return HL_E_OK;
    } catch (const std::exception&) {
        return HL_E_FAILED;
    } catch (...) {
        return HL_E_FAILED;
    }
}

hl_error_t hl_window_raise(hl_ctx* c, hl_window* wh) {
    try {
        auto* ctx = reinterpret_cast<CCabiCtx*>(c);
        if (!ctx)
            return HL_E_ARG;
        if (!cabiThreadOk(ctx))
            return HL_E_THREAD;
        auto W = wh ? wh->ref.lock() : nullptr;
        if (!W)
            return HL_E_NOT_FOUND;
        Desktop::windowState()->raise(W);
        return HL_E_OK;
    } catch (const std::exception&) {
        return HL_E_FAILED;
    } catch (...) {
        return HL_E_FAILED;
    }
}

hl_error_t hl_window_reset_client_size_grant(hl_ctx* c, hl_window* wh) {
    try {
        auto* ctx = reinterpret_cast<CCabiCtx*>(c);
        if (!ctx)
            return HL_E_ARG;
        if (!cabiThreadOk(ctx))
            return HL_E_THREAD;
        auto W = wh ? wh->ref.lock() : nullptr;
        if (!W)
            return HL_E_NOT_FOUND;
        W->m_sizeFromClientSerial = 0;
        return HL_E_OK;
    } catch (const std::exception&) {
        return HL_E_FAILED;
    } catch (...) {
        return HL_E_FAILED;
    }
}

hl_error_t hl_window_set_born_fullscreen(hl_ctx* c, hl_window* wh, uint32_t on) {
    try {
        auto* ctx = reinterpret_cast<CCabiCtx*>(c);
        if (!ctx)
            return HL_E_ARG;
        if (!cabiThreadOk(ctx))
            return HL_E_THREAD;
        auto W = wh ? wh->ref.lock() : nullptr;
        if (!W)
            return HL_E_NOT_FOUND;
        W->m_bornFullscreen = (on != 0);
        return HL_E_OK;
    } catch (const std::exception&) {
        return HL_E_FAILED;
    } catch (...) {
        return HL_E_FAILED;
    }
}

// =======================================================================
// Lua
// =======================================================================

hl_error_t hl_lua_register(hl_ctx* c, const char* ns, const char* name, hl_lua_fn fn) {
    try {
        auto* ctx = reinterpret_cast<CCabiCtx*>(c);
        if (!ctx || !ns || !name || !fn)
            return HL_E_ARG;
        if (!cabiThreadOk(ctx))
            return HL_E_THREAD;
        const bool OK = HyprlandAPI::addLuaFunction(ctx->m_handle, ns, name,
            reinterpret_cast<PLUGIN_LUA_FN>(fn));
        return OK ? HL_E_OK : HL_E_FAILED;
    } catch (const std::exception&) {
        return HL_E_FAILED;
    } catch (...) {
        return HL_E_FAILED;
    }
}

// =======================================================================
// native input state
// =======================================================================

uint32_t hl_session_locked(hl_ctx* c) {
    auto* ctx = reinterpret_cast<CCabiCtx*>(c);
    if (!ctx || !cabiThreadOk(ctx))
        return 0;
    return (g_pSessionLockManager && g_pSessionLockManager->isSessionLocked()) ? 1 : 0;
}

uint32_t hl_input_capture_active(hl_ctx* c) {
    auto* ctx = reinterpret_cast<CCabiCtx*>(c);
    if (!ctx || !cabiThreadOk(ctx))
        return 0;
    return (g_pInputManager && g_pInputManager->inputCaptureActive()) ? 1 : 0;
}

uint32_t hl_native_pointer_grab(hl_ctx* c) {
    auto* ctx = reinterpret_cast<CCabiCtx*>(c);
    if (!ctx || !cabiThreadOk(ctx))
        return 0;
    const bool held  = g_pInputManager && g_pInputManager->hasHeldButtons();
    const bool grab  = g_pSeatManager && g_pSeatManager->m_seatGrab && g_pSeatManager->m_seatGrab->m_pointer;
    return (held || grab) ? 1 : 0;
}

uint32_t hl_native_layer_at(hl_ctx* c) {
    auto* ctx = reinterpret_cast<CCabiCtx*>(c);
    if (!ctx || !cabiThreadOk(ctx))
        return 0;
    return (g_pInputManager && g_pInputManager->pointerHitIsNativeSurface()) ? 1 : 0;
}

uint32_t hl_super_held(hl_ctx* c) {
    auto* ctx = reinterpret_cast<CCabiCtx*>(c);
    if (!ctx || !cabiThreadOk(ctx))
        return 0;
    const auto KB = g_pSeatManager ? g_pSeatManager->m_keyboard.lock() : nullptr;
    return (KB && (KB->getModifiers() & Input::HL_MODIFIER_META) != Input::HL_MODIFIER_NONE) ? 1 : 0;
}

// =======================================================================
// events
// =======================================================================

hl_error_t hl_subscribe(hl_ctx* c, hl_event_mask_t mask, hl_dispatch_fn dispatch, void* ud) {
    try {
        auto* ctx = reinterpret_cast<CCabiCtx*>(c);
        if (!ctx)
            return HL_E_ARG;
        if (!cabiThreadOk(ctx))
            return HL_E_THREAD;
        if (ctx->m_mask != 0)
            return HL_E_STATE; // one dispatcher per ctx
        if (!dispatch)
            return HL_E_ARG;
        if (mask == 0)
            return HL_E_ARG;

        ctx->m_dispatch = dispatch;
        ctx->m_ud       = ud;
        ctx->m_mask     = mask;

        auto&    bus  = Event::bus();
        auto         weak = ctx->m_weak;

        // Build one hl_event_t, attach the relevant handles, and dispatch.
        // Each lambda captures the weak ctx: an expired ctx is a no-op.
        auto emit = [weak](auto&& build) {
            auto ctxp = weak.lock();
            if (!ctxp || ctxp->m_shutdown)
                return;
            hl_event_t ev{};
            build(ev);
            ctxp->m_dispatch(&ev, ctxp->m_ud);
        };

        auto& E = bus->m_events;

        if (mask & HL_EV_TICK)
            ctx->m_listeners.emplace_back(E.tick.listen([emit]() {
                emit([](hl_event_t& e) { e.kind = HL_EV_TICK; e.cancellable = 0; });
            }));

        if (mask & HL_EV_KEY)
            ctx->m_listeners.emplace_back(E.input.keyboard.key.listen([emit](IKeyboard::SKeyEvent k, Event::SCallbackInfo& info) {
                emit([&](hl_event_t& e) {
                    e.kind        = HL_EV_KEY;
                    e.cancellable = 1;
                    e.keycode     = k.keycode;
                    e.state       = (k.state == WL_KEYBOARD_KEY_STATE_PRESSED) ? 1 : 0;
                    e._cancel_slot = &info;
                });
            }));

        if (mask & HL_EV_MOUSE_BUTTON)
            ctx->m_listeners.emplace_back(E.input.mouse.button.listen([emit](IPointer::SButtonEvent b, Event::SCallbackInfo& info) {
                emit([&](hl_event_t& e) {
                    e.kind         = HL_EV_MOUSE_BUTTON;
                    e.cancellable  = 1;
                    e.button       = b.button;
                    e.state        = (b.state == WL_POINTER_BUTTON_STATE_PRESSED) ? 1 : 0;
                    e.x            = g_pInputManager->getMouseCoordsInternal().x;
                    e.y            = g_pInputManager->getMouseCoordsInternal().y;
                    e._cancel_slot = &info;
                });
            }));

        if (mask & HL_EV_MOUSE_MOVE)
            ctx->m_listeners.emplace_back(E.input.mouse.move.listen([emit](Vector2D pos, Event::SCallbackInfo& info) {
                emit([&](hl_event_t& e) {
                    e.kind         = HL_EV_MOUSE_MOVE;
                    e.cancellable  = 1;
                    e.x            = pos.x;
                    e.y            = pos.y;
                    e._cancel_slot = &info;
                });
            }));

        if (mask & HL_EV_MOUSE_AXIS)
            ctx->m_listeners.emplace_back(E.input.mouse.axis.listen([emit](IPointer::SAxisEvent a, Event::SCallbackInfo& info) {
                emit([&](hl_event_t& e) {
                    e.kind            = HL_EV_MOUSE_AXIS;
                    e.cancellable     = 1;
                    e.axis            = (uint32_t) a.axis;
                    e.delta           = a.delta;
                    e.delta_discrete  = (double) a.deltaDiscrete;
                    e.x               = g_pInputManager->getMouseCoordsInternal().x;
                    e.y               = g_pInputManager->getMouseCoordsInternal().y;
                    e._cancel_slot    = &info;
                });
            }));

        if (mask & HL_EV_WINDOW_ACTIVE)
            ctx->m_listeners.emplace_back(E.window.active.listen([emit](PHLWINDOW w, Desktop::eFocusReason) {
                emit([&](hl_event_t& e) {
                    e.kind = HL_EV_WINDOW_ACTIVE;
                    e.window = w ? makeWindow(w) : nullptr;
                });
            }));

        if (mask & HL_EV_WINDOW_OPEN)
            ctx->m_listeners.emplace_back(E.window.open.listen([emit](PHLWINDOW w) {
                emit([&](hl_event_t& e) {
                    e.kind = HL_EV_WINDOW_OPEN;
                    e.window = w ? makeWindow(w) : nullptr;
                });
            }));

        if (mask & HL_EV_WINDOW_OPEN_EARLY)
            ctx->m_listeners.emplace_back(E.window.openEarly.listen([emit](PHLWINDOW w) {
                emit([&](hl_event_t& e) {
                    e.kind = HL_EV_WINDOW_OPEN_EARLY;
                    e.window = w ? makeWindow(w) : nullptr;
                });
            }));

        if (mask & HL_EV_WINDOW_DESTROY)
            ctx->m_listeners.emplace_back(E.window.destroy.listen([emit](PHLWINDOWREF ref) {
                emit([&](hl_event_t& e) {
                    e.kind = HL_EV_WINDOW_DESTROY;
                    auto   w = ref.lock();
                    e.window = w ? makeWindow(w) : nullptr;
                });
            }));

        return HL_E_OK;
    } catch (const std::exception&) {
        return HL_E_FAILED;
    } catch (...) {
        return HL_E_FAILED;
    }
}

void hl_event_cancel(hl_event_t* ev) {
    if (ev && ev->cancellable && ev->_cancel_slot) {
        auto* info = static_cast<Event::SCallbackInfo*>(ev->_cancel_slot);
        info->cancelled = true;
    }
}

// =======================================================================
// jobs
// =======================================================================

static CCabiCtx::SJob* cabiMakeJob(CCabiCtx* ctx, CCabiCtx::eJobKind kind, hl_job_fn fn, void* ud) {
    auto job = makeUnique<CCabiCtx::SJob>();
    job->token  = ctx->m_nextJobToken++;
    job->kind   = kind;
    job->fn     = fn;
    job->ud     = ud;
    auto [it, ok] = ctx->m_jobs.emplace(job->token, std::move(job));
    return ok ? it->second.get() : nullptr;
}

hl_job_t hl_defer(hl_ctx* c, hl_job_fn fn, void* ud) {
    try {
        auto* ctx = reinterpret_cast<CCabiCtx*>(c);
        if (!ctx)
            return 0;
        if (!cabiThreadOk(ctx))
            return 0;
        if (ctx->m_shutdown || !fn)
            return 0;

        auto* job = cabiMakeJob(ctx, CCabiCtx::KIND_DEFER, fn, ud);
        if (!job)
            return 0;
        auto weak  = ctx->m_weak;
        const auto token = job->token;
        job->doLaterSeq  = g_pEventLoopManager->doLater([weak, token] {
            auto ctxp = weak.lock();
            if (!ctxp || ctxp->m_shutdown)
                return;
            auto it = ctxp->m_jobs.find(token);
            if (it == ctxp->m_jobs.end() || !it->second->active)
                return;
            it->second->active = false; // one-shot
            hl_job_fn jfn = it->second->fn;
            void*     jud = it->second->ud;
            ctxp->m_jobs.erase(it);
            jfn(jud);
        });
        job->active = true;
        return job->token;
    } catch (const std::exception&) {
        return 0;
    } catch (...) {
        return 0;
    }
}

hl_job_t hl_timer(hl_ctx* c, uint32_t ms, uint32_t repeat, hl_job_fn fn, void* ud) {
    try {
        auto* ctx = reinterpret_cast<CCabiCtx*>(c);
        if (!ctx)
            return 0;
        if (!cabiThreadOk(ctx))
            return 0;
        if (ctx->m_shutdown || !fn || ms == 0)
            return 0;

        auto* job = cabiMakeJob(ctx, CCabiCtx::KIND_TIMER, fn, ud);
        if (!job)
            return 0;
        auto            weak  = ctx->m_weak;
        const auto      token = job->token;
        const auto      dur   = Time::steady_dur(std::chrono::milliseconds(ms));
        job->timer = makeShared<CEventLoopTimer>(std::optional(dur), [weak, token, repeat, dur](SP<CEventLoopTimer> self, void*) {
            auto ctxp = weak.lock();
            if (!ctxp || ctxp->m_shutdown) {
                self->updateTimeout(std::nullopt);
                return;
            }
            auto it = ctxp->m_jobs.find(token);
            if (it == ctxp->m_jobs.end() || !it->second->active) {
                self->updateTimeout(std::nullopt);
                return;
            }
            hl_job_fn jfn = it->second->fn;
            void*     jud = it->second->ud;
            jfn(jud);
            if (repeat)
                self->updateTimeout(dur);
            else {
                it->second->active = false;
                ctxp->m_jobs.erase(it);
                self->updateTimeout(std::nullopt);
            }
        }, nullptr);
        g_pEventLoopManager->addTimer(job->timer);
        job->active = true;
        return job->token;
    } catch (const std::exception&) {
        return 0;
    } catch (...) {
        return 0;
    }
}

hl_job_t hl_watch_fd(hl_ctx* c, int fd, hl_job_fn fn, void* ud) {
    try {
        auto* ctx = reinterpret_cast<CCabiCtx*>(c);
        if (!ctx)
            return 0;
        if (!cabiThreadOk(ctx))
            return 0;
        if (ctx->m_shutdown || !fn || fd < 0)
            return 0;

        auto* job = cabiMakeJob(ctx, CCabiCtx::KIND_FD, fn, ud);
        if (!job)
            return 0;
        auto           weak  = ctx->m_weak;
        const auto     token = job->token;
        // doOnReadable takes ownership of the fd (closes it when the waiter
        // is dropped). If the fd is already readable it delivers once and
        // returns a null waiter; the plugin can re-register to keep watching.
        job->waiter = g_pEventLoopManager->doOnReadable(Hyprutils::OS::CFileDescriptor(fd), [weak, token] {
            auto ctxp = weak.lock();
            if (!ctxp || ctxp->m_shutdown)
                return;
            auto it = ctxp->m_jobs.find(token);
            if (it == ctxp->m_jobs.end() || !it->second->active)
                return;
            hl_job_fn jfn = it->second->fn;
            void*     jud = it->second->ud;
            jfn(jud);
        });
        job->active = true;
        return job->token;
    } catch (const std::exception&) {
        return 0;
    } catch (...) {
        return 0;
    }
}

void hl_job_cancel(hl_ctx* c, hl_job_t job) {
    try {
        auto* ctx = reinterpret_cast<CCabiCtx*>(c);
        if (!ctx)
            return;
        if (!cabiThreadOk(ctx))
            return;
        auto it = ctx->m_jobs.find(job);
        if (it != ctx->m_jobs.end()) {
            auto& j = it->second;
            ctx->cancelJobInternal(j);
            ctx->m_jobs.erase(it);
        }
    } catch (const std::exception&) {
    } catch (...) {
    }
}

// =======================================================================
// render (canvas & textures)
// =======================================================================

hl_error_t hl_render_listen(hl_ctx* c, uint32_t stage, hl_draw_fn draw, void* ud, void** out) {
    try {
        auto* ctx = reinterpret_cast<CCabiCtx*>(c);
        if (!ctx)
            return HL_E_ARG;
        if (!cabiThreadOk(ctx))
            return HL_E_THREAD;
        if (ctx->m_shutdown || !draw)
            return HL_E_ARG;
        if (stage > (uint32_t) HL_RND_POST)
            return HL_E_ARG;
        if (!out)
            return HL_E_ARG;

        // The cabi stage indices are a compact public set (0,1,2) and do NOT
        // equal the compositor's eRenderStage values — map them explicitly.
        // (RENDER_POST is post-GL, so HL_RND_POST maps to the last renderable
        // stage, RENDER_LAST_MOMENT.)
        eRenderStage target;
        switch (stage) {
            case 0:
                target = RENDER_POST_WINDOWS;
                break;
            case 1:
                target = RENDER_PRE_WINDOWS;
                break;
            default:
                target = RENDER_LAST_MOMENT;
                break;
        }

        // Register the render-stage listener once; it walks m_renders each
        // frame and adds a trampoline for every callback matching the stage.
        if (ctx->m_renders.empty()) {
            auto weak = ctx->m_weak;
            ctx->m_listeners.emplace_back(Event::bus()->m_events.render.stage.listen([weak](eRenderStage st) {
                auto ctxp = weak.lock();
                if (!ctxp || ctxp->m_shutdown)
                    return;
                auto mon = g_pHyprRenderer->m_renderData.pMonitor.lock();
                if (!mon)
                    return;
                for (const auto& r : ctxp->m_renders)
                    if (r->stage == (uint32_t) st)
                        g_pHyprRenderer->addPassElement(makeUnique<CCabiPassElement>(mon, r->draw, r->ud));
            }));
        }

        auto r     = makeShared<CCabiCtx::SRenderListener>();
        r->stage   = (uint32_t) target;
        r->draw    = draw;
        r->ud      = ud;
        void*    h = r.get();
        ctx->m_renders.push_back(std::move(r));
        *out = h;
        return HL_E_OK;
    } catch (const std::exception&) {
        return HL_E_FAILED;
    } catch (...) {
        return HL_E_FAILED;
    }
}

// ---- canvas queries ----

void hl_canvas_monitor(hl_canvas* cv, hl_monitor** out) {
    if (!cv || !out)
        return;
    auto M = cv->mon.lock();
    if (!M)
        return;
    *out = makeMonitor(M);
}

void hl_canvas_extent(hl_canvas* cv, hl_box_t* logical, float* scale) {
    if (!cv)
        return;
    auto M = cv->mon.lock();
    if (!M)
        return;
    if (logical) {
        const auto B = M->logicalBox();
        logical->x = 0;
        logical->y = 0;
        logical->w = B.size().x;
        logical->h = B.size().y;
    }
    if (scale)
        *scale = M->m_scale;
}

// monitor-local logical px -> monitor-local physical px (the renderer's space):
// the monitor's top-left is (0,0) in both, so only the scale differs.
static CBox cabiToPhys(const hl_canvas* cv, const hl_box_t& box) {
    return CBox{box.x, box.y, box.w, box.h}.scale(cv->scale).round();
}

// ---- canvas draw (the plugin paints imperatively through these) ----

void hl_canvas_rect(hl_canvas* cv, hl_box_t box, hl_color_t color, uint32_t round, float rp) {
    if (!cv || !Render::GL::g_pHyprOpenGL)
        return;
    if (!cv->mon.lock())
        return;
    try {
        const CHyprColor c{color.r, color.g, color.b, color.a};
        Render::GL::g_pHyprOpenGL->renderRect(cabiToPhys(cv, box), c, {.round = (int) round, .roundingPower = rp});
    } catch (...) {
    }
}

void hl_canvas_glass(hl_canvas* cv, hl_box_t box, hl_color_t color, uint32_t round, float rp, uint32_t blur) {
    if (!cv || !Render::GL::g_pHyprOpenGL)
        return;
    if (!cv->mon.lock())
        return;
    try {
        const CHyprColor c{color.r, color.g, color.b, color.a};
        Render::GL::g_pHyprOpenGL->renderRect(cabiToPhys(cv, box), c, {.round = (int) round, .roundingPower = rp, .blur = (bool) blur});
    } catch (...) {
    }
}

void hl_canvas_border(hl_canvas* cv, hl_box_t box, hl_color_t color, uint32_t round, float rp, uint32_t size_px) {
    if (!cv || !Render::GL::g_pHyprOpenGL)
        return;
    if (!cv->mon.lock())
        return;
    try {
        const CHyprColor c{color.r, color.g, color.b, color.a};
        Render::GL::g_pHyprOpenGL->renderBorder(cabiToPhys(cv, box), Config::CGradientValueData{c}, {.round = (int) round, .roundingPower = rp, .borderSize = (int) size_px});
    } catch (...) {
    }
}

void hl_canvas_texture(hl_canvas* cv, hl_texture* tex, hl_box_t box) {
    if (!cv || !tex || !tex->tex || !Render::GL::g_pHyprOpenGL)
        return;
    if (!cv->mon.lock())
        return;
    if (tex->tex->m_texID == 0)
        return; // warm/draw gate: never paint a texture in the frame it was created in
    try {
        Render::GL::g_pHyprOpenGL->renderTexture(tex->tex, cabiToPhys(cv, box), {});
    } catch (...) {
    }
}

// ---- textures ----

hl_error_t hl_text_texture(hl_ctx* c, const char* text, hl_color_t color, uint32_t pt, uint32_t max_width, const char* font, hl_texture** out) {
    try {
        auto* ctx = reinterpret_cast<CCabiCtx*>(c);
        if (!ctx)
            return HL_E_ARG;
        if (!cabiThreadOk(ctx))
            return HL_E_THREAD;
        if (ctx->m_shutdown || !text || !out)
            return HL_E_ARG;
        const CHyprColor col{color.r, color.g, color.b, color.a};
        auto t = g_pHyprRenderer->renderText(text, col, (int) pt, false, font ? font : "", (int) max_width);
        if (!t)
            return HL_E_FAILED;
        *out = new hl_texture(std::move(t));
        return HL_E_OK;
    } catch (const std::exception&) {
        return HL_E_FAILED;
    } catch (...) {
        return HL_E_FAILED;
    }
}

hl_error_t hl_texture_from_rgba(hl_ctx* c, const uint8_t* data, uint32_t w, uint32_t h, uint32_t stride, hl_texture** out) {
    try {
        auto* ctx = reinterpret_cast<CCabiCtx*>(c);
        if (!ctx)
            return HL_E_ARG;
        if (!cabiThreadOk(ctx))
            return HL_E_THREAD;
        if (ctx->m_shutdown || !data || !out || w == 0 || h == 0)
            return HL_E_ARG;
        // XRGB8888 (0x34325258): the software-texture format Hyprland uses for
        // CPU-provided pixels (the 'X' is an opaque alpha placeholder).
        auto t = g_pHyprRenderer->createTexture(DRM_FORMAT_XRGB8888, const_cast<uint8_t*>(data), stride, { (double) w, (double) h }, false, false);
        if (!t)
            return HL_E_FAILED;
        *out = new hl_texture(std::move(t));
        return HL_E_OK;
    } catch (const std::exception&) {
        return HL_E_FAILED;
    } catch (...) {
        return HL_E_FAILED;
    }
}

void hl_texture_size(hl_texture* t, uint32_t* w, uint32_t* h) {
    if (!t || !t->tex)
        return;
    if (w)
        *w = (uint32_t) t->tex->m_size.x;
    if (h)
        *h = (uint32_t) t->tex->m_size.y;
}

void hl_texture_ref(hl_texture* t) {
    if (t)
        t->rc.fetch_add(1, std::memory_order_relaxed);
}
void hl_texture_unref(hl_texture* t) {
    if (t && t->rc.fetch_sub(1, std::memory_order_acq_rel) == 1)
        delete t;
}

// ---- damage ----

void hl_damage(hl_ctx* c, hl_monitor* mh, hl_box_t box) {
    try {
        auto* ctx = reinterpret_cast<CCabiCtx*>(c);
        if (!ctx || !cabiThreadOk(ctx) || !mh)
            return;
        auto M = mh->ref.lock();
        if (!M)
            return;
        // damageBox takes GLOBAL coords; the box is monitor-local logical.
        const CBox global{box.x + M->m_position.x, box.y + M->m_position.y, box.w, box.h};
        g_pHyprRenderer->damageBox(global);
    } catch (const std::exception&) {
    } catch (...) {
    }
}
