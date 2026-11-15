#include "RectPassElement.hpp"
#include "../Renderer.hpp"

CRectPassElement::CRectPassElement(const CRectPassElement::SRectData& data_) : m_data(data_) {
    ;
}

bool CRectPassElement::needsLiveBlur(Render::CRenderContext& ctx) {
    return m_data.color.a < 1.F && m_data.blur && (!m_data.xray || g_pHyprRenderer->blurProviderRequiresLiveBlur());
}

bool CRectPassElement::needsPrecomputeBlur(Render::CRenderContext& ctx) {
    return m_data.color.a < 1.F && m_data.xray && m_data.blur && !g_pHyprRenderer->blurProviderRequiresLiveBlur();
}

std::optional<CBox> CRectPassElement::boundingBox(Render::CRenderContext& ctx) {
    return m_data.box.copy().scale(1.F / ctx.m_data.pMonitor->m_scale).round();
}

CRegion CRectPassElement::opaqueRegion(Render::CRenderContext& ctx) {
    // a hole hides what is under it as an opaque rect does. One logical pixel in, so the rounding to pixels never hides
    // more than the hole replaces, and a rounded one's corners in by their radius, as a rounded surface's
    if (m_data.punch)
        return m_data.box.copy().scale(1.F / ctx.m_data.pMonitor->m_scale).expand(-1 - m_data.round / ctx.m_data.pMonitor->m_scale);

    if (m_data.color.a < 1.F)
        return CRegion{};

    CRegion rg = boundingBox(ctx)->expand(-m_data.round);

    if (!m_data.clipBox.empty())
        rg.intersect(m_data.clipBox);

    return rg;
}
