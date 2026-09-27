// cabi-notify.cpp — the cabi surface for the notification port: markup
// text, focused-keyboard state, the cursor override, X11 pid/activation,
// pointer-grab state, and the async file-icon decode. Its own TU because
// the notification rasterizer (pango + cairo) and the decode worker are
// self-contained; cabi.cpp stays the generic surface.
//
// Same contract as the rest of the cabi: every entry is thread-checked and
// wrapped so a throwing compositor call becomes an error code, never an
// exception across the boundary (crash class 2, neutralized here).

#include "cabi-int.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
#include <thread>

#include "../../desktop/state/WindowState.hpp"
#include "../../desktop/view/window/Window.hpp"
#include "../../desktop/view/window/X11Backend.hpp"
#include "../../devices/IKeyboard.hpp"
#include "../../managers/SeatManager.hpp"
#include "../../protocols/XDGActivation.hpp"
#include "../../pointer/cursor/CursorShapeOverrideController.hpp"
#include "../../render/Renderer.hpp"
#include "../../Compositor.hpp"
#include "../../config/values/types/StringValue.hpp"
#include "../../helpers/math/Math.hpp"
#include "../../defines.hpp"

#include <pango/pangocairo.h>
#include <hyprgraphics/image/Image.hpp>
#include <xkbcommon/xkbcommon.h>

// =======================================================================
// markup text (the notification rasterizer)
// =======================================================================
//
// Ported from the hyprnotify rasterizer: a whitelisted-Pango-markup block
// rendered to a texture, with <a href> hit rectangles. The plugin owns any
// cache (its keys); one call = one texture.

namespace {

    int cabiCpToUtf8(uint32_t c, char buf[4]) {
        if (c < 0x80) {
            buf[0] = (char)c;
            return 1;
        }
        if (c < 0x800) {
            buf[0] = (char)(0xC0 | (c >> 6));
            buf[1] = (char)(0x80 | (c & 0x3F));
            return 2;
        }
        if (c < 0x10000) {
            buf[0] = (char)(0xE0 | (c >> 12));
            buf[1] = (char)(0x80 | ((c >> 6) & 0x3F));
            buf[2] = (char)(0x80 | (c & 0x3F));
            return 3;
        }
        buf[0] = (char)(0xF0 | (c >> 18));
        buf[1] = (char)(0x80 | ((c >> 12) & 0x3F));
        buf[2] = (char)(0x80 | ((c >> 6) & 0x3F));
        buf[3] = (char)(0x80 | (c & 0x3F));
        return 4;
    }

    uint32_t cabiParseCp(const std::string& e) { // "#960" or "#x3C0"
        if (e.size() < 2 || e[0] != '#')
            return 0;
        return e[1] == 'x' || e[1] == 'X' ? (uint32_t)std::strtol(e.c_str() + 2, nullptr, 16) : (uint32_t)std::strtol(e.c_str() + 1, nullptr, 10);
    }

    // Byte length an entity decodes to — must match Pango's stripping so link
    // offsets into the stripped text stay aligned.
    int cabiEntityBytes(const std::string& e) {
        if (e == "amp" || e == "lt" || e == "gt" || e == "quot" || e == "apos")
            return 1;
        if (e.size() > 1 && e[0] == '#') {
            const uint32_t C = cabiParseCp(e);
            char           b[4];
            if (C == 0 || C > 0x10FFFF || (C >= 0xD800 && C <= 0xDFFF))
                return 0;
            return cabiCpToUtf8(C, b);
        }
        return 0;
    }

    // Decode the entities of the href handed to the opener.
    std::string cabiDecodeEntities(const std::string& s) {
        std::string out;
        out.reserve(s.size());
        for (size_t i = 0; i < s.size();) {
            if (s[i] == '&') {
                const auto END = s.find(';', i);
                if (END != std::string::npos && END - i <= 10) {
                    const auto E = s.substr(i + 1, END - i - 1);
                    if (E == "amp" || E == "lt" || E == "gt" || E == "quot" || E == "apos") {
                        out += E == "amp" ? '&' : E == "lt" ? '<' : E == "gt" ? '>' : E == "quot" ? '"' : '\'';
                        i = END + 1;
                        continue;
                    }
                    if (E.size() > 1 && E[0] == '#') {
                        char           b[4];
                        const uint32_t C = cabiParseCp(E);
                        if (C > 0 && C <= 0x10FFFF && !(C >= 0xD800 && C <= 0xDFFF)) {
                            out.append(b, cabiCpToUtf8(C, b));
                            i = END + 1;
                            continue;
                        }
                    }
                }
            }
            out += s[i];
            i++;
        }
        return out;
    }

    // The clean fallback when markup won't parse: drop every tag, decode the
    // entities. Never leaks tag syntax to the user the way set_text on the raw
    // markup would.
    std::string cabiStripMarkupTags(const std::string& s) {
        std::string out;
        out.reserve(s.size());
        for (size_t i = 0; i < s.size();) {
            if (s[i] == '<') {
                if (const auto END = s.find('>', i); END != std::string::npos) {
                    i = END + 1;
                    continue;
                }
            }
            out += s[i++];
        }
        return cabiDecodeEntities(out);
    }

    // One quoted attribute out of one tag. The scan tracks the quote that
    // opened the current value, and the NAME must stand alone OUTSIDE any
    // value (tag start or whitespace before it) followed by '=' — a hit
    // inside a quoted value (title="..href=..") is not the attribute. Names
    // match case-insensitively against a lowered COPY, whose offsets still
    // line up with the original the value is cut from. "" if absent.
    std::string cabiAttrValue(const std::string& tag, const std::string& attr) {
        std::string lower = tag;
        std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return std::tolower(c); });
        char inQuote = 0;
        for (size_t i = 0; i < tag.size(); i++) {
            const auto CH = tag[i];
            if (inQuote) {
                if (CH == inQuote)
                    inQuote = 0;
                continue;
            }
            if (CH == '"' || CH == '\'') {
                inQuote = CH;
                continue;
            }
            if (lower.compare(i, attr.size(), attr) != 0)
                continue;
            if (i > 0 && lower[i - 1] != ' ' && lower[i - 1] != '\t')
                continue;
            size_t P = i + attr.size();
            while (P < lower.size() && (lower[P] == ' ' || lower[P] == '\t'))
                P++;
            if (P >= lower.size() || lower[P] != '=')
                continue;
            P++;
            while (P < lower.size() && (lower[P] == ' ' || lower[P] == '\t'))
                P++;
            if (P >= lower.size() || (lower[P] != '"' && lower[P] != '\''))
                continue;
            const char Q  = tag[P];
            const auto END = tag.find(Q, P + 1);
            return END == std::string::npos ? "" : tag.substr(P + 1, END - P - 1);
        }
        return "";
    }

    struct SCabiLinkSpan {
        std::string href;
        int         start = 0, len = 0; // byte range in the STRIPPED text
    };

    // Rewrite a live <a href> into a styled <span> Pango renders, tracking
    // each link's byte span in the stripped text for later hit-testing.
    std::string cabiConvertLinks(const std::string& in, const std::string& colHex, std::vector<SCabiLinkSpan>& out) {
        std::string md;
        md.reserve(in.size() + 32);
        int       plain = 0;
        SCabiLinkSpan cur;
        bool      inLink = false;
        for (size_t i = 0; i < in.size();) {
            if (in[i] == '<') {
                const auto END = in.find('>', i);
                if (END == std::string::npos) {
                    md += in[i++];
                    continue;
                }
                size_t j = i + 1;
                bool   close = false;
                if (j < END && in[j] == '/') {
                    close = true;
                    j++;
                }
                size_t ns = j;
                while (j < END && std::isalpha((unsigned char)in[j]))
                    j++;
                std::string name = in.substr(ns, j - ns);
                std::ranges::transform(name, name.begin(), [](unsigned char c) { return std::tolower(c); });
                if (name == "a") {
                    if (close) {
                        md += "</span>";
                        if (inLink) {
                            cur.len = plain - cur.start;
                            out.push_back(cur);
                            inLink = false;
                        }
                    } else {
                        md += "<span foreground=\"" + colHex + "\" underline=\"single\">";
                        cur    = SCabiLinkSpan{cabiDecodeEntities(cabiAttrValue(in.substr(i, END - i + 1), "href")), plain, 0};
                        inLink = true;
                    }
                } else
                    md += in.substr(i, END - i + 1); // other tag verbatim, 0 plain bytes
                i = END + 1;
                continue;
            }
            if (in[i] == '&') {
                const auto END = in.find(';', i);
                if (END != std::string::npos) {
                    md += in.substr(i, END - i + 1);
                    plain += cabiEntityBytes(in.substr(i + 1, END - i - 1));
                    i = END + 1;
                    continue;
                }
            }
            md += in[i];
            plain++;
            i++;
        }
        return md;
    }

    // hexOf for a CHyprColor, the way the markup link span wants it.
    std::string cabiHexOf(const CHyprColor& c) {
        char b[8];
        std::snprintf(b, sizeof b, "#%02x%02x%02x", (int)std::lround(c.r * 255), (int)std::lround(c.g * 255), (int)std::lround(c.b * 255));
        return b;
    }

} // namespace

hl_error_t hl_markup_text(hl_ctx* c, const char* text, hl_color_t col, uint32_t pt, const char* font, uint32_t max_w, int32_t max_h, float line_sp, int32_t weight,
                          const hl_color_t* link_col, uint32_t links_cap, hl_texture** out, uint32_t* out_w, uint32_t* out_h, hl_link_rect_t* out_links, hl_str_t* out_hrefs) {
    try {
        auto* ctx = reinterpret_cast<CCabiCtx*>(c);
        if (!ctx)
            return HL_E_ARG;
        if (!cabiThreadOk(ctx))
            return HL_E_THREAD;
        if (ctx->m_shutdown || !text || !*text || !out)
            return HL_E_ARG;
        const CHyprColor COL{col.r, col.g, col.b, col.a};

        PangoFontMap*         fontMap = pango_cairo_font_map_get_default();
        PangoContext*         context = pango_font_map_create_context(fontMap);
        PangoLayout*          layout  = pango_layout_new(context);
        PangoFontDescription* fd      = pango_font_description_new();
        g_object_unref(context);

        static auto           DEFAULTFONT = CConfigValue<std::string>("misc:font_family");
        const std::string     FAMILY      = (font && *font) ? std::string(font) : *DEFAULTFONT;
        pango_font_description_set_family_static(fd, FAMILY.c_str());
        pango_font_description_set_absolute_size(fd, pt * PANGO_SCALE);
        if (weight != 400)
            pango_font_description_set_weight(fd, (PangoWeight)weight);
        pango_layout_set_font_description(layout, fd);

        const bool HASLINKS = link_col != nullptr;
        PangoAttrList*         attrs = nullptr;
        std::vector<SCabiLinkSpan> linkSpans;
        if (HASLINKS || strpbrk(text, "<") != nullptr) {
            // link mode converts <a>; a plain string that still carries a '<'
            // is rendered with the whitelist so a stray tag shapes rather
            // than shows (the sanitizer upstream already guarantees the
            // whitelist for real input)
            const CHyprColor LINKCOL{link_col->r, link_col->g, link_col->b, link_col->a};
            std::string md = HASLINKS ? cabiConvertLinks(text, cabiHexOf(LINKCOL), linkSpans) : std::string(text);
            char*   stripped = nullptr;
            GError* err      = nullptr;
            if (pango_parse_markup(md.c_str(), -1, 0, &attrs, &stripped, nullptr, &err)) {
                pango_layout_set_text(layout, stripped, -1);
                g_free(stripped);
            } else {
                if (err)
                    g_error_free(err);
                const std::string PLAIN = cabiStripMarkupTags(text);
                pango_layout_set_text(layout, PLAIN.c_str(), -1);
                linkSpans.clear();
            }
        } else
            pango_layout_set_text(layout, text, -1);

        pango_layout_set_width(layout, std::max(max_w, 1u) * PANGO_SCALE);
        pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
        pango_layout_set_height(layout, max_h < 0 ? max_h : std::max((int)max_h, (int)pt) * PANGO_SCALE);
        pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
        if (attrs) {
            pango_layout_set_attributes(layout, attrs);
            pango_attr_list_unref(attrs);
        }
        if (line_sp > 0)
            pango_layout_set_line_spacing(layout, line_sp);

        PangoRectangle ink = {}, log = {};
        pango_layout_get_pixel_extents(layout, &ink, &log);
        const int W = std::max(log.width, ink.x + ink.width), H = std::max(log.height, ink.y + ink.height);
        if (W <= 0 || H <= 0) {
            pango_font_description_free(fd);
            g_object_unref(layout);
            return HL_E_FAILED;
        }

        // probe the laid-out layout for each link's hit rect (physical px)
        size_t written = 0;
        if (HASLINKS && !linkSpans.empty())
            for (const auto& L : linkSpans) {
                if (L.len <= 0)
                    continue;
                if (out_links && written < links_cap) {
                    PangoRectangle a, b;
                    pango_layout_index_to_pos(layout, L.start, &a);
                    pango_layout_index_to_pos(layout, L.start + L.len, &b);
                    const double X0 = a.x / (double)PANGO_SCALE, Y0 = a.y / (double)PANGO_SCALE;
                    double x1 = b.x / (double)PANGO_SCALE, h = a.height / (double)PANGO_SCALE;
                    if (b.y != a.y) { // the link wrapped a line: cover to the right edge and down
                        x1 = (double)W;
                        h  = (b.y + b.height) / (double)PANGO_SCALE - Y0;
                    }
                    out_links[written] = hl_link_rect{(float)std::min(X0, x1), (float)Y0, (float)std::max(X0, x1), (float)h};
                    if (out_hrefs)
                        out_hrefs[written] = hl_str_t{L.href.c_str(), (uint32_t)L.href.size()};
                    written++;
                }
            }

        auto* SURF = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, W, H);
        auto* CR   = cairo_create(SURF);
        cairo_set_source_rgba(CR, COL.r, COL.g, COL.b, COL.a);
        cairo_move_to(CR, 0, 0);
        pango_cairo_show_layout(CR, layout);

        pango_font_description_free(fd);
        g_object_unref(layout);
        cairo_surface_flush(SURF);

        auto tex = g_pHyprRenderer->createTexture(SURF);
        cairo_destroy(CR);
        cairo_surface_destroy(SURF);
        if (!tex)
            return HL_E_FAILED;
        *out = new hl_texture(std::move(tex));
        if (out_w)
            *out_w = (uint32_t)W;
        if (out_h)
            *out_h = (uint32_t)H;
        if (HASLINKS && written < linkSpans.size())
            return HL_E_FULL; // the texture is built; more links were dropped
        return HL_E_OK;
    } catch (const std::exception&) {
        return HL_E_FAILED;
    } catch (...) {
        return HL_E_FAILED;
    }
}

// =======================================================================
// focused-keyboard state (the inline-reply field)
// =======================================================================

hl_error_t hl_keyboard_key(hl_ctx* c, uint32_t keycode, hl_str_t* sym, uint32_t* ctrl, uint32_t* alt, uint32_t* logo, char* utf8, uint32_t utf8_cap) {
    try {
        auto* ctx = reinterpret_cast<CCabiCtx*>(c);
        if (!ctx)
            return HL_E_ARG;
        if (!cabiThreadOk(ctx))
            return HL_E_THREAD;
        if (ctx->m_shutdown)
            return HL_E_UNAVAILABLE;
        if (!utf8 || utf8_cap < 5)
            return HL_E_ARG;
        utf8[0] = 0;
        if (ctrl)
            *ctrl = 0;
        if (alt)
            *alt = 0;
        if (logo)
            *logo = 0;
        if (!g_pSeatManager)
            return HL_E_UNAVAILABLE;
        auto KB = g_pSeatManager->m_keyboard.lock();
        if (!KB || !KB->m_xkbState)
            return HL_E_NOT_FOUND;

        const auto SYM = xkb_state_key_get_one_sym(KB->m_xkbState, keycode);
        if (sym) {
            char N[64] = {};
            xkb_keysym_get_name(SYM, N, sizeof N);
            *sym = ctx->scratch(N[0] ? N : "Unknown");
        }
        if (ctrl)
            *ctrl = xkb_state_mod_name_is_active(KB->m_xkbState, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE) > 0;
        if (alt)
            *alt = xkb_state_mod_name_is_active(KB->m_xkbState, XKB_MOD_NAME_ALT, XKB_STATE_MODS_EFFECTIVE) > 0;
        if (logo)
            *logo = xkb_state_mod_name_is_active(KB->m_xkbState, XKB_MOD_NAME_LOGO, XKB_STATE_MODS_EFFECTIVE) > 0;
        char buf[8]{};
        const int N = xkb_state_key_get_utf8(KB->m_xkbState, keycode, buf, sizeof buf);
        if (N > 0)
            std::strncpy(utf8, buf, utf8_cap - 1);
        return HL_E_OK;
    } catch (const std::exception&) {
        return HL_E_FAILED;
    } catch (...) {
        return HL_E_FAILED;
    }
}

// =======================================================================
// cursor override (the drawn surfaces own the pointer)
// =======================================================================

void hl_cursor_override(hl_ctx* c, const char* shape, uint32_t on) {
    try {
        auto* ctx = reinterpret_cast<CCabiCtx*>(c);
        if (!ctx || !cabiThreadOk(ctx) || ctx->m_shutdown)
            return;
        if (!on || !shape || !*shape)
            Pointer::Cursor::overrideController->unsetOverride(Pointer::Cursor::CURSOR_OVERRIDE_SPECIAL_ACTION);
        else
            Pointer::Cursor::overrideController->setOverride(shape, Pointer::Cursor::CURSOR_OVERRIDE_SPECIAL_ACTION);
    } catch (const std::exception&) {
    } catch (...) {
    }
}

// =======================================================================
// window pid + X11 + xdg-activation (the X11 activation lookup)
// =======================================================================

uint32_t hl_window_pid(hl_ctx* c, hl_window* w) {
    try {
        auto* ctx = reinterpret_cast<CCabiCtx*>(c);
        if (!ctx || !cabiThreadOk(ctx) || !w)
            return 0;
        auto W = w->ref.lock();
        if (!W)
            return 0;
        const pid_t PID = W->backend().pid();
        return PID > 0 ? (uint32_t)PID : 0u;
    } catch (const std::exception&) {
        return 0;
    } catch (...) {
        return 0;
    }
}

uint32_t hl_window_is_x11(hl_ctx* c, hl_window* w) {
    try {
        auto* ctx = reinterpret_cast<CCabiCtx*>(c);
        if (!ctx || !cabiThreadOk(ctx) || !w)
            return 0;
        auto W = w->ref.lock();
        if (!W)
            return 0;
        return W->backend().isX11() ? 1u : 0u;
    } catch (const std::exception&) {
        return 0;
    } catch (...) {
        return 0;
    }
}

hl_error_t hl_activation_token(hl_ctx* c, hl_str_t* out) {
    try {
        auto* ctx = reinterpret_cast<CCabiCtx*>(c);
        if (!ctx)
            return HL_E_ARG;
        if (!cabiThreadOk(ctx))
            return HL_E_THREAD;
        if (ctx->m_shutdown || !out)
            return HL_E_ARG;
        if (!PROTO::activation)
            return HL_E_UNAVAILABLE;
        *out = ctx->scratch(PROTO::activation->mintToken());
        return HL_E_OK;
    } catch (const std::exception&) {
        return HL_E_FAILED;
    } catch (...) {
        return HL_E_FAILED;
    }
}

// =======================================================================
// async file-icon decode (off the event loop)
// =======================================================================
//
// A file icon's decode happens on one worker thread, not the compositor's
// event loop: a 4K app icon must not stall a frame. The event loop polls
// status and asks for a bounded, scaled texture when the decode lands.
//
// The slot cap bounds the decode work: a decode cannot be cancelled once
// the worker has it, so the cap also bounds replaced and deleted cards
// (the same reason the C++ gatherer path was capped at 24).

namespace {

    struct SCabiImageJob {
        std::string path;
        int         svgPx = 0;
        bool        tint  = false; // symbolic recolor (the freedesktop convention)
        uint8_t     r = 0, g = 0, b = 0;
        // result: premultiplied BGRA (cairo ARGB32 in memory), worker-written,
        // event-loop-read after state flips to ready
        std::vector<uint8_t> rgba;
        uint32_t w = 0, h = 0;
        std::atomic<int> state{0}; // 0 pending, 1 ready, 2 failed
    };

    constexpr size_t MAX_IMAGE_JOBS     = 24;
    constexpr size_t MAX_IMAGE_FILE_BYTES = 32 * 1024 * 1024;
    constexpr double MAX_IMAGE_PIXELS   = 16.0 * 1024 * 1024;

    std::unordered_map<uint32_t, UP<SCabiImageJob>> s_jobs;
    std::deque<uint32_t>                            s_queue;
    std::mutex                                      s_mutex;
    std::condition_variable                         s_cv;
    std::atomic<bool>                               s_workerDown{true};
    std::atomic<uint32_t>                           s_nextToken{1};

    // The worker: wait for a token, decode it. Owns no plugin state — the
    // jobs are fork-side, so a plugin unload cannot race a decode.
    void cabiImageWorkerLoop() {
        for (;;) {
            uint32_t token = 0;
            {
                std::unique_lock<std::mutex> lk(s_mutex);
                s_cv.wait(lk, [] { return !s_queue.empty(); });
                if (s_queue.empty())
                    continue;
                token = s_queue.front();
                s_queue.pop_front();
            }
            std::lock_guard<std::mutex> lk(s_mutex);
            const auto IT = s_jobs.find(token);
            if (IT == s_jobs.end())
                continue; // dropped while queued
            auto& JOB = *IT->second;

            std::error_code ec;
            bool            ok = false;
            if (const auto STATUS = std::filesystem::status(JOB.path, ec); !ec && std::filesystem::is_regular_file(STATUS) &&
                std::filesystem::file_size(JOB.path, ec) <= MAX_IMAGE_FILE_BYTES) {
                Hyprgraphics::CImage img(JOB.path, JOB.svgPx > 0 ? Hyprutils::Math::Vector2D{(double)JOB.svgPx, (double)JOB.svgPx} : Hyprutils::Math::Vector2D{});
                const auto SURF = img.success() ? img.cairoSurface() : nullptr;
                if (SURF && SURF->status() == CAIRO_STATUS_SUCCESS) {
                    const auto SZ = SURF->size();
                    if (SZ.x > 0 && SZ.y > 0 && (double)SZ.x * SZ.y <= MAX_IMAGE_PIXELS) {
                        auto* C = SURF->cairo();
                        const auto F = cairo_image_surface_get_format(C);
                        if (F == CAIRO_FORMAT_ARGB32 || F == CAIRO_FORMAT_RGB24) {
                            const int W = cairo_image_surface_get_width(C), H = cairo_image_surface_get_height(C);
                            const int STRIDE = cairo_image_surface_get_stride(C);
                            const auto* DATA = (const uint8_t*)cairo_image_surface_get_data(C);
                            std::vector<uint8_t> out((size_t)W * H * 4);
                            if (F == CAIRO_FORMAT_ARGB32) {
                                // premultiplied BGRA in memory: row-copy
                                for (int y = 0; y < H; y++)
                                    std::memcpy(out.data() + (size_t)y * W * 4, DATA + (size_t)y * STRIDE, (size_t)W * 4);
                            } else {
                                // RGB24: BGR opaque
                                for (int y = 0; y < H; y++)
                                    for (int x = 0; x < W; x++) {
                                        out[(size_t)y * W * 4 + x * 4] = DATA[(size_t)y * STRIDE + x * 3];
                                        out[(size_t)y * W * 4 + x * 4 + 1] = DATA[(size_t)y * STRIDE + x * 3 + 1];
                                        out[(size_t)y * W * 4 + x * 4 + 2] = DATA[(size_t)y * STRIDE + x * 3 + 2];
                                        out[(size_t)y * W * 4 + x * 4 + 3] = 255;
                                    }
                            }
                            if (JOB.tint) {
                                // repaint a symbolic mark to col: the pixel math
                                // of the C++ tintSurface (alpha untouched)
                                for (size_t i = 0; i < out.size(); i += 4) {
                                    const uint8_t A = out[i + 3];
                                    out[i]     = (uint8_t)(JOB.b * A / 255);
                                    out[i + 1] = (uint8_t)(JOB.g * A / 255);
                                    out[i + 2] = (uint8_t)(JOB.r * A / 255);
                                }
                            }
                            JOB.rgba = std::move(out);
                            JOB.w    = (uint32_t)W;
                            JOB.h    = (uint32_t)H;
                            ok       = true;
                        }
                    }
                }
            }
            JOB.state.store(ok ? 1 : 2, std::memory_order_release);
        }
    }

    void cabiImageEnsureWorker() {
        // one worker per compositor lifetime; fork-owned, so no plugin can
        // strand it
        std::unique_lock<std::mutex> lk(s_mutex);
        if (s_workerDown.load(std::memory_order_acquire)) {
            s_workerDown.store(false, std::memory_order_release);
            std::thread(cabiImageWorkerLoop).detach();
        }
    }

    bool cabiImageAdmissible(const std::string& path) {
        std::error_code ec;
        const auto      STATUS = std::filesystem::status(path, ec);
        return !ec && std::filesystem::is_regular_file(STATUS) && std::filesystem::file_size(path, ec) <= MAX_IMAGE_FILE_BYTES;
    }

} // namespace

uint32_t hl_image_decode(hl_ctx* c, const char* path, int svg_px, uint32_t tint, uint8_t r, uint8_t g, uint8_t b) {
    try {
        auto* ctx = reinterpret_cast<CCabiCtx*>(c);
        if (!ctx || !cabiThreadOk(ctx) || ctx->m_shutdown || !path || !*path)
            return 0;
        const auto P = std::string(path);
        if (!cabiImageAdmissible(P))
            return 0;
        const int SVG = (P.size() > 4 && P.substr(P.size() - 4) == ".svg") ? std::clamp(svg_px, 1, 256) : 0;
        std::lock_guard<std::mutex> lk(s_mutex);
        if (s_jobs.size() >= MAX_IMAGE_JOBS)
            return 0; // the slot is taken: the caller retries later
        const uint32_t TOKEN = s_nextToken.fetch_add(1, std::memory_order_relaxed);
        auto           job   = makeUnique<SCabiImageJob>();
        job->path            = std::move(P);
        job->svgPx           = SVG;
        job->tint            = tint != 0;
        job->r               = r;
        job->g               = g;
        job->b               = b;
        s_jobs[TOKEN]        = std::move(job);
        s_queue.push_back(TOKEN);
        cabiImageEnsureWorker();
        s_cv.notify_one();
        return TOKEN;
    } catch (const std::exception&) {
        return 0;
    } catch (...) {
        return 0;
    }
}

int hl_image_token_status(hl_ctx* c, uint32_t token) {
    try {
        auto* ctx = reinterpret_cast<CCabiCtx*>(c);
        if (!ctx || !cabiThreadOk(ctx) || ctx->m_shutdown)
            return -1;
        std::lock_guard<std::mutex> lk(s_mutex);
        const auto IT = s_jobs.find(token);
        return IT == s_jobs.end() ? -1 : IT->second->state.load(std::memory_order_acquire);
    } catch (const std::exception&) {
        return -1;
    } catch (...) {
        return -1;
    }
}

hl_error_t hl_image_token_size(hl_ctx* c, uint32_t token, uint32_t* w, uint32_t* h) {
    try {
        auto* ctx = reinterpret_cast<CCabiCtx*>(c);
        if (!ctx)
            return HL_E_ARG;
        if (!cabiThreadOk(ctx))
            return HL_E_THREAD;
        std::lock_guard<std::mutex> lk(s_mutex);
        const auto IT = s_jobs.find(token);
        if (IT == s_jobs.end() || IT->second->state.load(std::memory_order_acquire) != 1)
            return HL_E_NOT_FOUND;
        if (w)
            *w = IT->second->w;
        if (h)
            *h = IT->second->h;
        return HL_E_OK;
    } catch (const std::exception&) {
        return HL_E_FAILED;
    } catch (...) {
        return HL_E_FAILED;
    }
}

hl_error_t hl_image_token_texture(hl_ctx* c, uint32_t token, uint32_t mode, uint32_t max_px, uint32_t w, uint32_t h, hl_texture** out, uint32_t* out_w, uint32_t* out_h) {
    try {
        auto* ctx = reinterpret_cast<CCabiCtx*>(c);
        if (!ctx)
            return HL_E_ARG;
        if (!cabiThreadOk(ctx))
            return HL_E_THREAD;
        if (ctx->m_shutdown || !out)
            return HL_E_ARG;

        // grab the result out of the map (a copy of the buffer; the token may
        // be dropped while the texture is alive)
        std::vector<uint8_t> DATA;
        uint32_t W = 0, H = 0;
        {
            std::lock_guard<std::mutex> lk(s_mutex);
            const auto IT = s_jobs.find(token);
            if (IT == s_jobs.end() || IT->second->state.load(std::memory_order_acquire) != 1)
                return HL_E_NOT_FOUND;
            DATA = IT->second->rgba;
            W    = IT->second->w;
            H    = IT->second->h;
        }
        if (W == 0 || H == 0 || DATA.empty())
            return HL_E_FAILED;

        auto* SRC = cairo_image_surface_create_for_data(DATA.data(), CAIRO_FORMAT_ARGB32, (int)W, (int)H, (int)(W * 4));
        if (cairo_surface_status(SRC) != CAIRO_STATUS_SUCCESS) {
            cairo_surface_destroy(SRC);
            return HL_E_FAILED;
        }

        SP<Render::ITexture> TEX;
        uint32_t OW = 0, OH = 0;
        if (mode == 1 && w > 0 && h > 0) {
            // cover: scale to fill w x h, center-crop the overflow
            OW = w;
            OH = h;
            auto* DST = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, (int)OW, (int)OH);
            auto* CR  = cairo_create(DST);
            const double S = std::max((double)OW / W, (double)OH / H);
            cairo_translate(CR, (OW - (double)W * S) / 2.0, (OH - (double)H * S) / 2.0);
            cairo_scale(CR, S, S);
            cairo_set_source_surface(CR, SRC, 0, 0);
            cairo_pattern_set_filter(cairo_get_source(CR), CAIRO_FILTER_GOOD);
            cairo_paint(CR);
            cairo_destroy(CR);
            cairo_surface_flush(DST);
            TEX = g_pHyprRenderer->createTexture(DST);
            cairo_surface_destroy(DST);
        } else if (W > max_px || H > max_px) {
            // fit: downscale once, preserving the aspect
            const double SCALE = std::min((double)max_px / W, (double)max_px / H);
            OW = std::max(1, (int)std::lround(W * SCALE));
            OH = std::max(1, (int)std::lround(H * SCALE));
            auto* DST = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, (int)OW, (int)OH);
            auto* CR  = cairo_create(DST);
            cairo_scale(CR, (double)OW / W, (double)OH / H);
            cairo_set_source_surface(CR, SRC, 0, 0);
            cairo_pattern_set_filter(cairo_get_source(CR), CAIRO_FILTER_GOOD);
            cairo_paint(CR);
            cairo_destroy(CR);
            cairo_surface_flush(DST);
            TEX = g_pHyprRenderer->createTexture(DST);
            cairo_surface_destroy(DST);
        } else {
            // small enough: upload as-is
            TEX = g_pHyprRenderer->createTexture(SRC);
            OW = W;
            OH = H;
        }
        cairo_surface_destroy(SRC);
        if (!TEX)
            return HL_E_FAILED;
        *out = new hl_texture(std::move(TEX));
        if (out_w)
            *out_w = OW;
        if (out_h)
            *out_h = OH;
        return HL_E_OK;
    } catch (const std::exception&) {
        return HL_E_FAILED;
    } catch (...) {
        return HL_E_FAILED;
    }
}

void hl_image_token_drop(hl_ctx* c, uint32_t token) {
    try {
        auto* ctx = reinterpret_cast<CCabiCtx*>(c);
        if (!ctx || !cabiThreadOk(ctx))
            return;
        std::lock_guard<std::mutex> lk(s_mutex);
        s_jobs.erase(token);
    } catch (const std::exception&) {
    } catch (...) {
    }
}

// =======================================================================
// avatar + chevron (the generated faces and the fold indicator)
// =======================================================================

hl_error_t hl_avatar_texture(hl_ctx* c, hl_color_t bg, const char* text, hl_color_t fg, uint32_t px, const char* font, hl_texture** out) {
    try {
        auto* ctx = reinterpret_cast<CCabiCtx*>(c);
        if (!ctx)
            return HL_E_ARG;
        if (!cabiThreadOk(ctx))
            return HL_E_THREAD;
        if (ctx->m_shutdown || !text || !*text || !out || px < 8)
            return HL_E_ARG;

        const CHyprColor BG{bg.r, bg.g, bg.b, bg.a};
        const CHyprColor FG{fg.r, fg.g, fg.b, fg.a};
        auto* SURF = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, (int)px, (int)px);
        if (!SURF)
            return HL_E_FAILED;
        auto* CR = cairo_create(SURF);
        cairo_set_source_rgb(CR, BG.r, BG.g, BG.b);
        cairo_paint(CR);

        static auto DEFAULTFONT = CConfigValue<std::string>("misc:font_family");
        auto* LAYOUT = pango_cairo_create_layout(CR);
        pango_layout_set_text(LAYOUT, text, -1);
        auto* FONT = pango_font_description_new();
        pango_font_description_set_family_static(FONT, (font && *font) ? font : (*DEFAULTFONT).c_str());
        pango_font_description_set_size(FONT, std::max(8, (int)std::lround(px * 0.38)) * PANGO_SCALE);
        pango_font_description_set_weight(FONT, PANGO_WEIGHT_BOLD);
        pango_layout_set_font_description(LAYOUT, FONT);
        pango_layout_set_alignment(LAYOUT, PANGO_ALIGN_CENTER);
        pango_layout_set_width(LAYOUT, px * PANGO_SCALE);
        int TW = 0, TH = 0;
        pango_layout_get_pixel_size(LAYOUT, &TW, &TH);
        cairo_set_source_rgba(CR, FG.r, FG.g, FG.b, FG.a);
        cairo_move_to(CR, 0, (px - TH) / 2.0);
        pango_cairo_show_layout(CR, LAYOUT);
        pango_font_description_free(FONT);
        g_object_unref(LAYOUT);
        cairo_destroy(CR);
        cairo_surface_flush(SURF);

        auto tex = g_pHyprRenderer->createTexture(SURF);
        cairo_surface_destroy(SURF);
        if (!tex)
            return HL_E_FAILED;
        *out = new hl_texture(std::move(tex));
        return HL_E_OK;
    } catch (const std::exception&) {
        return HL_E_FAILED;
    } catch (...) {
        return HL_E_FAILED;
    }
}

hl_error_t hl_chevron_texture(hl_ctx* c, uint32_t dir, hl_color_t col, uint32_t px, hl_texture** out) {
    try {
        auto* ctx = reinterpret_cast<CCabiCtx*>(c);
        if (!ctx)
            return HL_E_ARG;
        if (!cabiThreadOk(ctx))
            return HL_E_THREAD;
        if (ctx->m_shutdown || !out || px <= 4)
            return HL_E_ARG;

        // A stroked chevron, not a font glyph: Pixel's expand_more/less is
        // two 45° strokes with round caps (the glyph weight is the font's)
        const CHyprColor C{col.r, col.g, col.b, col.a};
        const double S = px / 24.0; // Material's 24dp canvas
        auto* SURF = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, (int)px, (int)px);
        if (!SURF)
            return HL_E_FAILED;
        auto* CR = cairo_create(SURF);
        cairo_set_source_rgba(CR, C.r, C.g, C.b, C.a);
        cairo_set_line_width(CR, 2.0 * S);
        cairo_set_line_cap(CR, CAIRO_LINE_CAP_ROUND);
        cairo_set_line_join(CR, CAIRO_LINE_JOIN_ROUND);
        if (dir > 0) { // up
            cairo_move_to(CR, 6.0 * S, 14.0 * S);
            cairo_line_to(CR, 12.0 * S, 8.0 * S);
            cairo_line_to(CR, 18.0 * S, 14.0 * S);
        } else { // down
            cairo_move_to(CR, 6.0 * S, 10.0 * S);
            cairo_line_to(CR, 12.0 * S, 16.0 * S);
            cairo_line_to(CR, 18.0 * S, 10.0 * S);
        }
        cairo_stroke(CR);
        cairo_surface_flush(SURF);

        auto tex = g_pHyprRenderer->createTexture(SURF);
        cairo_destroy(CR);
        cairo_surface_destroy(SURF);
        if (!tex)
            return HL_E_FAILED;
        *out = new hl_texture(std::move(tex));
        return HL_E_OK;
    } catch (const std::exception&) {
        return HL_E_FAILED;
    } catch (...) {
        return HL_E_FAILED;
    }
}

// The deterministic generic mark an iconless card wears when neither an
// identity nor the fallback face dir resolves: a rounded plate in the
// theme's frame color with a 2x2 grid of rounded squares in the ink
// (the theme's application-default-icon when the theme ships one is the
// first choice, resolved on the plugin side).
hl_error_t hl_generic_mark_texture(hl_ctx* c, hl_color_t plate, hl_color_t ink, uint32_t px, hl_texture** out) {
    try {
        auto* ctx = reinterpret_cast<CCabiCtx*>(c);
        if (!ctx)
            return HL_E_ARG;
        if (!cabiThreadOk(ctx))
            return HL_E_THREAD;
        if (ctx->m_shutdown || !out || px < 8)
            return HL_E_ARG;

        const CHyprColor PL{plate.r, plate.g, plate.b, plate.a};
        const CHyprColor IN{ink.r, ink.g, ink.b, ink.a};
        auto* SURF = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, (int)px, (int)px);
        if (!SURF)
            return HL_E_FAILED;
        auto* CT = cairo_create(SURF);
        auto rounded = [&](double x, double y, double w, double h, double r) {
            cairo_new_sub_path(CT);
            cairo_arc(CT, x + w - r, y + r, r, -M_PI_2, 0);
            cairo_arc(CT, x + w - r, y + h - r, r, 0, M_PI_2);
            cairo_arc(CT, x + r, y + h - r, r, M_PI_2, M_PI);
            cairo_arc(CT, x + r, y + r, r, M_PI, 1.5 * M_PI);
            cairo_close_path(CT);
        };
        rounded(0, 0, px, px, px * 0.22);
        cairo_set_source_rgba(CT, PL.r, PL.g, PL.b, PL.a * 0.55);
        cairo_fill(CT);
        const double G = px * 0.16, GAP = px * 0.12;
        const double X0 = (px - (2 * G + GAP)) / 2;
        for (int q = 0; q < 2; q++)
            for (int r = 0; r < 2; r++) {
                rounded(X0 + q * (G + GAP), X0 + r * (G + GAP), G, G, G * 0.35);
                cairo_set_source_rgba(CT, IN.r, IN.g, IN.b, IN.a * 0.7);
                cairo_fill(CT);
            }
        cairo_destroy(CT);
        if (cairo_surface_status(SURF) != CAIRO_STATUS_SUCCESS) {
            cairo_surface_destroy(SURF);
            return HL_E_FAILED;
        }
        auto tex = g_pHyprRenderer->createTexture(SURF);
        cairo_surface_destroy(SURF);
        if (!tex)
            return HL_E_FAILED;
        *out = new hl_texture(std::move(tex));
        return HL_E_OK;
    } catch (const std::exception&) {
        return HL_E_FAILED;
    } catch (...) {
        return HL_E_FAILED;
    }
}
