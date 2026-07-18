#pragma once

#include <vector>
#include <cstdint>
#include "WaylandProtocol.hpp"
#include "xdg-activation-v1.hpp"

class CXDGActivationToken {
  public:
    CXDGActivationToken(SP<CXdgActivationTokenV1> resource_);
    ~CXDGActivationToken();

    bool good();

  private:
    SP<CXdgActivationTokenV1> m_resource;

    uint32_t                  m_serial    = 0;
    std::string               m_appID     = "";
    bool                      m_committed = false;

    std::string               m_token = "";

    friend class CXDGActivationProtocol;
};

class CXDGActivationProtocol : public IWaylandProtocol {
  public:
    CXDGActivationProtocol(const wl_interface* iface, const int& ver, const std::string& name);

    virtual void bindManager(wl_client* client, void* data, uint32_t ver, uint32_t id);

    // mint a token on the compositor's behalf: registered like a
    // client-requested one, honored once by activate(). For in-compositor
    // requesters that must hand a token to a foreign client, e.g. a
    // notification daemon's ActivationToken signal (fdo notifications 1.3).
    std::string mintToken();

  private:
    void onManagerResourceDestroy(wl_resource* res);
    void destroyToken(CXDGActivationToken* pointer);
    void onGetToken(CXdgActivationV1* pMgr, uint32_t id);

    struct SSentToken {
        std::string token;
        wl_client*  client = nullptr; // READ-ONLY: can be dead
    };
    std::vector<SSentToken> m_sentTokens;

    //
    std::vector<UP<CXdgActivationV1>>    m_managers;
    std::vector<UP<CXDGActivationToken>> m_tokens;

    friend class CXDGActivationToken;
};

namespace PROTO {
    inline UP<CXDGActivationProtocol> activation;
};
