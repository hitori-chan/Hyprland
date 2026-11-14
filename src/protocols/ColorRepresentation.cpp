#include "ColorRepresentation.hpp"
#include "core/Compositor.hpp"
#include "../helpers/Format.hpp"
#include "../helpers/cm/YCbCr.hpp"
#include <algorithm>

using namespace NColorManagement;

// the matrices the renderer's EGL import and the planes' COLOR_ENCODING both have
static std::optional<eYCbCrEncoding> encodingFromWP(wpColorRepresentationSurfaceV1Coefficients coefficients) {
    switch (coefficients) {
        case WP_COLOR_REPRESENTATION_SURFACE_V1_COEFFICIENTS_BT601: return CM_YCBCR_BT601;
        case WP_COLOR_REPRESENTATION_SURFACE_V1_COEFFICIENTS_BT709: return CM_YCBCR_BT709;
        case WP_COLOR_REPRESENTATION_SURFACE_V1_COEFFICIENTS_BT2020: return CM_YCBCR_BT2020;
        default: return std::nullopt;
    }
}

CColorRepresentation::CColorRepresentation(UP<CWpColorRepresentationSurfaceV1>&& resource, SP<CWLSurfaceResource> surface) : m_resource(std::move(resource)), m_surface(surface) {
    if UNLIKELY (!good())
        return;

    m_resource->setDestroy([this](CWpColorRepresentationSurfaceV1* r) { PROTO::colorRepresentation->destroyResource(this); });
    m_resource->setOnDestroy([this](CWpColorRepresentationSurfaceV1* r) { PROTO::colorRepresentation->destroyResource(this); });

    m_resource->setSetAlphaMode([this](CWpColorRepresentationSurfaceV1* r, wpColorRepresentationSurfaceV1AlphaMode mode) {
        if (checkInert())
            return;

        // premultiplied electrical is what every surface already is
        if (mode != WP_COLOR_REPRESENTATION_SURFACE_V1_ALPHA_MODE_PREMULTIPLIED_ELECTRICAL)
            r->error(WP_COLOR_REPRESENTATION_SURFACE_V1_ERROR_ALPHA_MODE, "Unsupported alpha mode");
    });

    m_resource->setSetCoefficientsAndRange(
        [this](CWpColorRepresentationSurfaceV1* r, wpColorRepresentationSurfaceV1Coefficients coefficients, wpColorRepresentationSurfaceV1Range range) {
            if (checkInert())
                return;

            const bool IDENTITY = coefficients == WP_COLOR_REPRESENTATION_SURFACE_V1_COEFFICIENTS_IDENTITY;
            const auto ENCODING = encodingFromWP(coefficients);
            const bool FULL     = range == WP_COLOR_REPRESENTATION_SURFACE_V1_RANGE_FULL;

            if ((!FULL && range != WP_COLOR_REPRESENTATION_SURFACE_V1_RANGE_LIMITED) || (!IDENTITY && !ENCODING) || (IDENTITY && !FULL)) {
                r->error(WP_COLOR_REPRESENTATION_SURFACE_V1_ERROR_COEFFICIENTS, "Unsupported coefficients and range");
                return;
            }

            m_coefficients = coefficients;

            // identity is RGB, which needs no conversion
            m_surface->m_pending.updated.bits.colorRepresentation = true;
            m_surface->m_pending.colorRepresentation              = IDENTITY ? std::nullopt : std::optional{SYCbCr{.encoding = *ENCODING, .fullRange = FULL}};
        });

    m_resource->setSetChromaLocation([this](CWpColorRepresentationSurfaceV1* r, wpColorRepresentationSurfaceV1ChromaLocation location) {
        if (checkInert())
            return;

        // neither the sampler nor KMS takes a chroma location: checked, then unused
        if (location < WP_COLOR_REPRESENTATION_SURFACE_V1_CHROMA_LOCATION_TYPE_0 || location > WP_COLOR_REPRESENTATION_SURFACE_V1_CHROMA_LOCATION_TYPE_5)
            r->error(WP_COLOR_REPRESENTATION_SURFACE_V1_ERROR_CHROMA_LOCATION, "Invalid chroma location");
    });

    m_listeners.surfacePrecommit = m_surface->m_events.precommit.listen([this] { onPrecommit(); });
}

CColorRepresentation::~CColorRepresentation() {
    if (!m_surface)
        return;

    // destroying the object unsets the representation, double-buffered
    m_surface->m_pending.updated.bits.colorRepresentation = true;
    m_surface->m_pending.colorRepresentation.reset();
}

bool CColorRepresentation::good() {
    return m_resource->resource();
}

bool CColorRepresentation::checkInert() {
    if (m_surface)
        return false;

    m_resource->error(WP_COLOR_REPRESENTATION_SURFACE_V1_ERROR_INERT, "The wl_surface is gone");
    return true;
}

void CColorRepresentation::onPrecommit() {
    if (!m_surface || !m_coefficients)
        return;

    const auto& PENDING = m_surface->m_pending;
    const auto& BUFFER  = PENDING.updated.bits.buffer ? PENDING.buffer : m_surface->m_current.buffer;
    if (!BUFFER)
        return;

    const auto ATTRS = BUFFER->dmabuf();
    const bool YUV   = ATTRS.success && NFormatUtils::isFormatYUV(ATTRS.format);

    // identity takes the RGB family, the matrices the YCbCr family
    if (YUV == (*m_coefficients == WP_COLOR_REPRESENTATION_SURFACE_V1_COEFFICIENTS_IDENTITY)) {
        m_resource->error(WP_COLOR_REPRESENTATION_SURFACE_V1_ERROR_PIXEL_FORMAT, "The buffer's pixel format does not take the coefficients");
        m_surface->m_pending.rejected = true;
    }
}

CColorRepresentationProtocol::CColorRepresentationProtocol(const wl_interface* iface, const int& ver, const std::string& name) : IWaylandProtocol(iface, ver, name) {
    ;
}

void CColorRepresentationProtocol::bindManager(wl_client* client, void* data, uint32_t ver, uint32_t id) {
    const auto RESOURCE = m_managers.emplace_back(makeUnique<CWpColorRepresentationManagerV1>(client, ver, id)).get();

    if UNLIKELY (!RESOURCE->resource()) {
        wl_client_post_no_memory(client);
        m_managers.pop_back();
        return;
    }

    RESOURCE->setOnDestroy([this](CWpColorRepresentationManagerV1* manager) { destroyManager(manager); });
    RESOURCE->setDestroy([this](CWpColorRepresentationManagerV1* manager) { destroyManager(manager); });
    RESOURCE->setGetSurface(
        [this](CWpColorRepresentationManagerV1* manager, uint32_t id, wl_resource* surface) { getSurface(manager, id, CWLSurfaceResource::fromResource(surface)); });

    RESOURCE->sendSupportedAlphaMode(WP_COLOR_REPRESENTATION_SURFACE_V1_ALPHA_MODE_PREMULTIPLIED_ELECTRICAL);
    RESOURCE->sendSupportedCoefficientsAndRanges(WP_COLOR_REPRESENTATION_SURFACE_V1_COEFFICIENTS_IDENTITY, WP_COLOR_REPRESENTATION_SURFACE_V1_RANGE_FULL);
    for (const auto COEFFICIENTS :
         {WP_COLOR_REPRESENTATION_SURFACE_V1_COEFFICIENTS_BT601, WP_COLOR_REPRESENTATION_SURFACE_V1_COEFFICIENTS_BT709, WP_COLOR_REPRESENTATION_SURFACE_V1_COEFFICIENTS_BT2020}) {
        RESOURCE->sendSupportedCoefficientsAndRanges(COEFFICIENTS, WP_COLOR_REPRESENTATION_SURFACE_V1_RANGE_FULL);
        RESOURCE->sendSupportedCoefficientsAndRanges(COEFFICIENTS, WP_COLOR_REPRESENTATION_SURFACE_V1_RANGE_LIMITED);
    }
    RESOURCE->sendDone();
}

void CColorRepresentationProtocol::destroyManager(CWpColorRepresentationManagerV1* manager) {
    std::erase_if(m_managers, [&](const auto& p) { return p.get() == manager; });
}

void CColorRepresentationProtocol::destroyResource(CColorRepresentation* resource) {
    std::erase_if(m_surfaces, [&](const auto& p) { return p.get() == resource; });
}

void CColorRepresentationProtocol::getSurface(CWpColorRepresentationManagerV1* manager, uint32_t id, SP<CWLSurfaceResource> surface) {
    if (std::ranges::any_of(m_surfaces, [&](const auto& p) { return p->m_surface == surface; })) {
        manager->error(WP_COLOR_REPRESENTATION_MANAGER_V1_ERROR_SURFACE_EXISTS, "The surface has a color representation already");
        return;
    }

    const auto& RESOURCE =
        m_surfaces.emplace_back(makeUnique<CColorRepresentation>(makeUnique<CWpColorRepresentationSurfaceV1>(manager->client(), manager->version(), id), surface));

    if UNLIKELY (!RESOURCE->good()) {
        manager->noMemory();
        m_surfaces.pop_back();
    }
}
