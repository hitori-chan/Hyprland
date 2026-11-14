#pragma once

#include <optional>
#include <vector>
#include "WaylandProtocol.hpp"
#include "color-representation-v1.hpp"
#include "../helpers/signal/Signal.hpp"

class CWLSurfaceResource;

class CColorRepresentation {
  public:
    CColorRepresentation(UP<CWpColorRepresentationSurfaceV1>&& resource, SP<CWLSurfaceResource> surface);
    ~CColorRepresentation();

    bool good();

  private:
    UP<CWpColorRepresentationSurfaceV1> m_resource;
    WP<CWLSurfaceResource>              m_surface;

    // the coefficients set last, checked against the buffer at commit
    std::optional<wpColorRepresentationSurfaceV1Coefficients> m_coefficients;

    bool                                                      checkInert();
    void                                                      onPrecommit();

    struct {
        CHyprSignalListener surfacePrecommit;
    } m_listeners;

    friend class CColorRepresentationProtocol;
};

class CColorRepresentationProtocol : public IWaylandProtocol {
  public:
    CColorRepresentationProtocol(const wl_interface* iface, const int& ver, const std::string& name);

    virtual void bindManager(wl_client* client, void* data, uint32_t ver, uint32_t id);

  private:
    void destroyManager(CWpColorRepresentationManagerV1* manager);
    void destroyResource(CColorRepresentation* resource);
    void getSurface(CWpColorRepresentationManagerV1* manager, uint32_t id, SP<CWLSurfaceResource> surface);

    //
    std::vector<UP<CWpColorRepresentationManagerV1>> m_managers;
    std::vector<UP<CColorRepresentation>>            m_surfaces;

    friend class CColorRepresentation;
};

namespace PROTO {
    inline UP<CColorRepresentationProtocol> colorRepresentation;
};
