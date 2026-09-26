/*
 * cabi — the narrow C ABI that carries the compositor boundary for the
 * single Rust plugin (awesome). Owned by the fork; the only C++<->Rust
 * boundary in the system. One rule: no C++ type, no reference, no
 * exception, no template crosses this header. Everything is by-value C
 * structs, opaque refcounted handles, or `const char*` with explicit
 * lifetime rules.
 *
 * This file is parsed by bindgen (the awesome crate's build.rs) and is
 * also the installed header `hyprland/src/plugins/cabi/cabi.h`. It must
 * stay valid C99 (the C++-only internals live in cabi-int.hpp).
 *
 * Threading: every hl_* call (except cabiAbiVersion) must run on the
 * event-loop thread recorded at context creation. A mismatch returns
 * HL_E_THREAD (it turns misuse into a log line, not UB).
 *
 * Errors: every function returns hl_error_t (or a count/uint). A throwing
 * compositor call becomes HL_E_FAILED — an exception never crosses the
 * boundary.
 */
#ifndef HYPRLAND_CABI_H
/* CABI_ABI_VERSION covers the whole surface; bump it on any change above. */
#define HYPRLAND_CABI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 1. error model ---------------------------------------------------- */
typedef enum {
    HL_E_OK = 0,       /* success */
    HL_E_ARG,          /* invalid argument / bad handle / bad string */
    HL_E_NOT_FOUND,    /* no such object (expired window, no monitor, …) */
    HL_E_STATE,        /* illegal for current state (draw outside a frame,
                          texture build inside a frame, cancel a
                          non-cancellable event) */
    HL_E_THREAD,       /* called off the event-loop thread */
    HL_E_FULL,         /* a bounded structure is at its bound */
    HL_E_FAILED,       /* the compositor op failed (focus refused, …) */
    HL_E_UNAVAILABLE,  /* feature off (no lua, no config, renderer down) */
} hl_error_t;

/* ---- 2. opaque handles ------------------------------------------------- */
/*
 * A handle is a refcounted C++ object on the fork side that holds a WEAK
 * reference to the underlying compositor object. Every query locks that
 * weak reference; an expired object yields HL_E_NOT_FOUND, never a
 * dangling deref (the same guard that fixes the teardown-SEGV class).
 *
 * Ownership: every handle the fork hands to the plugin (via hl_windows,
 * hl_focus_*, an event payload) arrives with one reference the plugin
 * owns; the plugin must unref it when done. hl_*_ref / hl_*_unref manage
 * additional references.
 */
typedef struct hl_ctx        hl_ctx;
typedef struct hl_window     hl_window;
typedef struct hl_workspace  hl_workspace;
typedef struct hl_monitor    hl_monitor;

/* ---- by-value types ---------------------------------------------------- */
/*
 * String model (zero-alloc hot path): an hl_str_t is valid until the next
 * call on the same ctx (single-threaded, so no race). Use hl_strdup to
 * copy a value that must outlive the call; hl_free releases it.
 */
typedef struct { const char* d; uint32_t l; } hl_str_t;
typedef struct { double x, y, w, h; } hl_box_t;
typedef struct { float r, g, b, a; } hl_color_t;

/* ---- 3. events --------------------------------------------------------- */
typedef uint32_t hl_event_mask_t;
#define HL_EV_TICK              (1u << 0)
#define HL_EV_KEY               (1u << 1)   /* cancellable */
#define HL_EV_MOUSE_BUTTON      (1u << 2)   /* cancellable */
#define HL_EV_MOUSE_MOVE        (1u << 3)   /* cancellable */
#define HL_EV_MOUSE_AXIS        (1u << 4)   /* cancellable */
#define HL_EV_WINDOW_ACTIVE     (1u << 5)
#define HL_EV_WINDOW_OPEN       (1u << 6)   /* mapped & first configure done */
#define HL_EV_WINDOW_OPEN_EARLY (1u << 7)   /* openEarly: predictSize usable */
#define HL_EV_WINDOW_DESTROY    (1u << 8)
#define HL_EV_WINDOW_CLOSE      (1u << 9)
#define HL_EV_WINDOW_FULL       (1u << 10)
#define HL_EV_WINDOW_FLOAT      (1u << 11)
#define HL_EV_WINDOW_TITLE      (1u << 12)
#define HL_EV_WINDOW_CLASS      (1u << 13)
#define HL_EV_WINDOW_PIN        (1u << 14)
#define HL_EV_WINDOW_MIN        (1u << 15)
#define HL_EV_WINDOW_WS         (1u << 16)  /* moveToWorkspace */
#define HL_EV_WINDOW_BELL       (1u << 17)  /* cancellable */
#define HL_EV_WS_ACTIVE         (1u << 18)
#define HL_EV_WS_MOVE_MON       (1u << 19)
#define HL_EV_WS_CREATED        (1u << 20)
#define HL_EV_WS_REMOVED        (1u << 21)
#define HL_EV_WS_RENAMED        (1u << 22)
#define HL_EV_MON_ADDED         (1u << 23)
#define HL_EV_MON_REMOVED       (1u << 24)
#define HL_EV_MON_PRECOMMIT     (1u << 25)
#define HL_EV_MON_FOCUSED       (1u << 26)
#define HL_EV_MON_RESERVED      (1u << 27)
#define HL_EV_CONFIG_RELOAD     (1u << 28)
#define HL_EV_EXIT              (1u << 29)

typedef struct hl_event {
    uint32_t kind;         /* an HL_EV_* bit (as a value, not a mask) */
    uint32_t cancellable;  /* 1 if hl_event_cancel applies to this event */
    /* by-value payloads — only the fields of the active kind are set */
    double   x, y;         /* mouse: position */
    uint32_t button;       /* mouse button: BTN_* */
    uint32_t state;        /* press/release; key: state */
    uint32_t keycode;      /* key */
    uint32_t axis;         /* axis enum */
    double   delta, delta_discrete;
    hl_window*    window;    /* window events (a ref is attached) */
    hl_workspace* workspace; /* a ref is attached */
    hl_monitor*   monitor;   /* a ref is attached */
    hl_str_t     string;     /* title/class/etc. */
    void*        _cancel_slot; /* fork-owned; opaque to the plugin */
} hl_event_t;

typedef void (*hl_dispatch_fn)(hl_event_t* ev, void* ud);

/* ---- 4. jobs ----------------------------------------------------------- */
typedef uint64_t hl_job_t;
typedef void (*hl_job_fn)(void* ud);

/*
 * Lifetime rule (crash class 2 at the boundary): a job holds a fork-side
 * weak reference to the ctx. The ctx is destroyed by hl_shutdown (which the
 * plugin calls from its exit entry BEFORE the loader unloads the .so). A
 * job whose ctx has expired becomes a silent no-op — it can never fire
 * into an unmapped .so. Cancellation is idempotent; tokens are never
 * reused while a job is pending.
 */

/* ======================================================================= */
/* functions                                                               */
/* ======================================================================= */

/* Version handshake. Exported by the fork (no ctx). The plugin calls this
 * during init and ejects if it does not match the version it was built
 * against. */
uint32_t cabiAbiVersion(void);

/* Destroy the context: cancels every job, unsubscribes every event, drops
 * every listener. Called from the plugin's exit entry (hyprPluginExitC).
 * Idempotent. */
hl_error_t hl_shutdown(hl_ctx* ctx);

/* Log to Hyprland's LOG at `level` (0=DEBUG,1=INFO,2=WARN,3=ERR). `fmt` is
 * a printf-style format (C callers). */
void hl_log(hl_ctx* ctx, uint32_t level, const char* fmt, ...);

/* Log a pre-formatted message (no format interpretation). The path a
 * non-C caller (Rust) uses: it formats its own string and passes it whole,
 * so a literal '%' in the text is never misread as a specifier. */
void hl_log_str(hl_ctx* ctx, uint32_t level, const char* msg);

/* ---- handle ref/unref (the plugin owns the ref the fork handed it) ---- */
void hl_window_ref(hl_window* w);
void hl_window_unref(hl_window* w);
void hl_workspace_ref(hl_workspace* w);
void hl_workspace_unref(hl_workspace* w);
void hl_monitor_ref(hl_monitor* m);
void hl_monitor_unref(hl_monitor* m);

/* ---- string model ------------------------------------------------------ */
hl_error_t hl_strdup(hl_ctx* ctx, hl_str_t src, hl_str_t* out);
void       hl_free(hl_ctx* ctx, void* p);

/* ---- queries (const, always safe inside any callback) ----------------- */
/* Window geometry is the layout box (what the client is sized to). */
hl_error_t hl_window_get(hl_ctx* ctx, hl_window* w,
    hl_str_t* app_id, hl_str_t* title, hl_box_t* at, hl_box_t* size,
    uint32_t* fullscreen, uint32_t* fullscreen_client,
    uint32_t* floating, uint32_t* pinned, uint32_t* visible,
    uint32_t* allowed_over_fullscreen, uint32_t* urgent);
hl_error_t hl_window_workspace(hl_ctx* ctx, hl_window* w, hl_workspace** out);
/* The window's stable identity (its address, valid while the window lives;
 * 0 if the handle's weak ref has expired). Keys a plugin's per-window map —
 * entries must be dropped on HL_EV_WINDOW_DESTROY so a reused address cannot
 * alias a fresh window. */
uint64_t hl_window_id(hl_ctx* ctx, hl_window* w);

hl_error_t hl_workspace_get(hl_ctx* ctx, hl_workspace* ws,
    hl_str_t* name, uint32_t* id, uint32_t* focused);

hl_error_t hl_monitor_get(hl_ctx* ctx, hl_monitor* m,
    hl_str_t* name, hl_box_t* box, float* scale, uint32_t* focused);
hl_error_t hl_monitor_at(hl_ctx* ctx, double x, double y, hl_monitor** out);

hl_error_t hl_focus_window(hl_ctx* ctx, hl_window** out);
hl_error_t hl_focus_monitor(hl_ctx* ctx, hl_monitor** out);
hl_error_t hl_window_at(hl_ctx* ctx, double x, double y, hl_window** out);

/* Bounded iteration: fills up to `cap` handles (each with one ref the
 * caller owns) and returns the total count, which may exceed cap. */
uint32_t hl_windows(hl_ctx* ctx, hl_window** out, uint32_t cap);
uint32_t hl_monitors(hl_ctx* ctx, hl_monitor** out, uint32_t cap);
uint32_t hl_workspaces(hl_ctx* ctx, hl_workspace** out, uint32_t cap);

/* ---- window writes (geometry / maximize / focus; event-loop thread) ---- */
/* Set the window's layout box (logical px, GLOBAL — like a monitor box).
 * Combines setTargetGeom + warpPositionSize (immediate). Floats only; a
 * tiled window must go through hl_window_set_fs_mode instead. */
hl_error_t hl_window_set_geom(hl_ctx* ctx, hl_window* w, double x, double y, double pw, double ph);
/* The monitor's workarea (logical box minus reserved areas), in global px. */
hl_error_t hl_monitor_workarea(hl_ctx* ctx, hl_monitor* m, hl_box_t* out);
/* The toplevel's min/max size (logical px); a pinned axis has min == max. */
hl_error_t hl_window_min_max_size(hl_ctx* ctx, hl_window* w, hl_box_t* min, hl_box_t* max);
/* Set the compositor fullscreen modes; pass -1 (0xFFFFFFFF) to leave one
 * unchanged. Values mirror eFullscreenMode (0 none, 1 maximized, 2 full). */
hl_error_t hl_window_set_fs_mode(hl_ctx* ctx, hl_window* w, uint32_t internal, uint32_t client);
/* Set the xdg toplevel's client-facing maximized bit (told-state only; this
 * never enters compositor fullscreen). */
hl_error_t hl_window_set_toplevel_maximized(hl_ctx* ctx, hl_window* w, uint32_t on);
/* Read back the toplevel's client-facing maximized bit (last told state). */
uint32_t   hl_window_told_maximized(hl_ctx* ctx, hl_window* w);
/* Ask the client to report its size (the 0x0 grant; it answers with its
 * normal size on the next commit). */
hl_error_t hl_window_request_client_size(hl_ctx* ctx, hl_window* w);
/* Force the window-size configure out (an unforced send dedups against the
 * pending reported size and can stay silent). */
hl_error_t hl_window_send_window_size(hl_ctx* ctx, hl_window* w, uint32_t force);
/* Raise the window in its workspace (stacking; does not change focus). */
hl_error_t hl_window_raise(hl_ctx* ctx, hl_window* w);
/* Reset the in-flight client-size grant (adopt/restore: the box is ours, so
 * a later client size answer must not re-impose itself). */
hl_error_t hl_window_reset_client_size_grant(hl_ctx* ctx, hl_window* w);
/* Set the born-fullscreen bit (false dissolves the one-shot re-grant that
 * would re-arm the 0x0 client-size request on the next floating recalc). */
hl_error_t hl_window_set_born_fullscreen(hl_ctx* ctx, hl_window* w, uint32_t on);

/* ---- Lua --------------------------------------------------------------- */
/* A Lua function body: called with the lua_State* when invoked; returns the
 * number of Lua values pushed as results. Args are read through the Lua C
 * API (the plugin binds lua.h itself). */
typedef int (*hl_lua_fn)(void* lua_state);
/* Register a Lua function `ns`.`name` (e.g. ns="hyprmax", name="toggle" ->
 * hyprmax.toggle). The original per-plugin namespace is preserved so the
 * user's existing binds keep working after the cutover. */
hl_error_t hl_lua_register(hl_ctx* ctx, const char* ns, const char* name, hl_lua_fn fn);

/* ---- native input state (compositor-integration rules) ---------------- */
uint32_t hl_session_locked(hl_ctx* ctx);
uint32_t hl_input_capture_active(hl_ctx* ctx);   /* EIS owns input */
uint32_t hl_native_pointer_grab(hl_ctx* ctx);    /* seat grab / held btns */
/* The pointer (at its current position — the event position for input
 * events) is over a native layer surface: pass the event through. */
uint32_t hl_native_layer_at(hl_ctx* ctx);

/* ---- events ------------------------------------------------------------ */
hl_error_t hl_subscribe(hl_ctx* ctx, hl_event_mask_t mask,
    hl_dispatch_fn dispatch, void* ud);
/* Cancels the event (cancellable kinds only). HL_E_STATE semantics are
 * surfaced by simply not cancelling; the plugin checks ev->cancellable. */
void hl_event_cancel(hl_event_t* ev);

/* ---- jobs -------------------------------------------------------------- */
hl_job_t hl_defer(hl_ctx* ctx, hl_job_fn fn, void* ud);        /* next idle */
hl_job_t hl_timer(hl_ctx* ctx, uint32_t ms, uint32_t repeat,
    hl_job_fn fn, void* ud);
hl_job_t hl_watch_fd(hl_ctx* ctx, int fd, hl_job_fn fn, void* ud); /* readable */
void     hl_job_cancel(hl_ctx* ctx, hl_job_t job);

/* ---- config (register at init, read live) ------------------------------ */
typedef enum {
    HL_CFG_INT = 0,
    HL_CFG_BOOL,
    HL_CFG_FLOAT,
    HL_CFG_STRING,
} hl_cfg_type_t;

/*
 * Register a config value under `key` (the `plugin:` namespace). Returns an
 * opaque handle kept alive for the ctx lifetime. num_default carries the
 * default for INT/BOOL/FLOAT; str_default for STRING. Must run on the
 * event-loop thread during init (same window as the C++ plugins).
 */
hl_error_t hl_config_register(hl_ctx* ctx, const char* key, const char* desc,
    uint32_t type, double num_default, const char* str_default, void** out);

/* Read the live value. Fills `type` and either `num` (INT/BOOL/FLOAT) or
 * `str` (STRING). */
hl_error_t hl_config_get(hl_ctx* ctx, void* h, uint32_t* type, double* num, hl_str_t* str);

/* ---- 5. render (canvas & textures) ------------------------------------ */
/*
 * The render model mirrors the fork's pass-element design: the plugin
 * registers a draw callback for a render stage. The fork's cabi layer
 * listens to the compositor's render-stage event and, at the selected stage,
 * adds a trampoline pass element that builds a canvas for the monitor being
 * rendered and calls the plugin's callback. The callback paints
 * IMPERATIVELY through the canvas (rect/glass/border/texture) — it does not
 * return elements.
 *
 * Textures are created OUTSIDE the frame (the "warm" pass) and referenced by
 * a later frame's draw. A texture cannot be painted in the frame that created
 * it (crash class 4); the warm/draw gate is the plugin's discipline, and the
 * canvas draw calls simply no-op for a not-yet-ready texture.
 */

/* Render stages the callback can fire at (a mirror of the fork's
 * eRenderStage; only the ones a plugin UI needs are exposed). */
typedef enum {
    HL_RND_POST_WINDOWS = 0, /* after windows, before top layers — bars */
    HL_RND_PRE_WINDOWS = 1,  /* before windows, after bottom/overlay */
    HL_RND_POST        = 2,  /* final stage (after all layers) */
} hl_render_stage_t;

typedef struct hl_canvas  hl_canvas;
typedef struct hl_texture hl_texture;

/* A draw callback: paint this frame for the active monitor. Runs on the
 * event-loop thread inside the render pass; keep it short (no D-Bus, no
 * blocking). `ud` is the plugin's pointer from hl_render_listen. */
typedef void (*hl_draw_fn)(hl_canvas* cv, void* ud);

/* Register a draw callback for `stage`. `out` receives an opaque handle
 * (hl_shutdown clears it). Invoked once per frame for each rendered monitor. */
hl_error_t hl_render_listen(hl_ctx* ctx, uint32_t stage, hl_draw_fn draw, void* ud, void** out);

/* ---- canvas queries (monitor-local LOGICAL px; the fork scales) -------- */
/* The monitor this frame renders (a ref is attached). */
void hl_canvas_monitor(hl_canvas* cv, hl_monitor** out);
/* The monitor's logical box + output scale (for layout). */
void hl_canvas_extent(hl_canvas* cv, hl_box_t* logical, float* scale);

/* ---- canvas draw (all monitor-local logical px) ------------------------ */
/* A filled rect; round=0 for square corners, rounding_power tunes the curve. */
void hl_canvas_rect(hl_canvas* cv, hl_box_t box, hl_color_t color, uint32_t round, float rounding_power);
/* Opaque fast path, or translucent blur when `blur` is set (the "glass"). */
void hl_canvas_glass(hl_canvas* cv, hl_box_t box, hl_color_t color, uint32_t round, float rounding_power, uint32_t blur);
/* A border ring of `size_px` thickness. */
void hl_canvas_border(hl_canvas* cv, hl_box_t box, hl_color_t color, uint32_t round, float rounding_power, uint32_t size_px);
/* Blit a (ready) texture into a logical box. No-ops if the texture is not
 * ready this frame. */
void hl_canvas_texture(hl_canvas* cv, hl_texture* tex, hl_box_t box);

/* ---- textures (refcounted; build in the warm pass, draw in a later frame) */
/* A text texture. `pt` is the font size in logical px; `max_width` 0 = no
 * wrap; `font` "" = the compositor default. Returns a ref the caller owns. */
hl_error_t hl_text_texture(hl_ctx* ctx, const char* text, hl_color_t color,
    uint32_t pt, uint32_t max_width, const char* font, hl_texture** out);
/* A texture from raw RGBA8 image data (top-down, straight alpha). */
hl_error_t hl_texture_from_rgba(hl_ctx* ctx, const uint8_t* data,
    uint32_t w, uint32_t h, uint32_t stride, hl_texture** out);
/* Pixel size (the texture's native, physical extent). */
void hl_texture_size(hl_texture* t, uint32_t* w, uint32_t* h);
void hl_texture_ref(hl_texture* t);
void hl_texture_unref(hl_texture* t);

/* ---- damage ------------------------------------------------------------- */
/* Mark a monitor-local logical box dirty (schedules a repaint of `m`). */
void hl_damage(hl_ctx* ctx, hl_monitor* m, hl_box_t box);

#ifdef __cplusplus
}
#endif

#endif /* HYPRLAND_CABI_H */
