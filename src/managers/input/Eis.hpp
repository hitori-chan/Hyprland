#pragma once

#include <libeis.h>
#include <string>
#include <wayland-server-core.h>

/*
 * Responsible to creating a socket for input communication
 */
class CEis {
  public:
    CEis(std::string socketPath);
    ~CEis();

    void startEmulating(int activationId);
    void stopEmulating();

    void resetKeyboard();
    void resetPointer();

    void sendMotion(double x, double y);
    void sendKey(uint32_t key, bool pressed);
    void sendModifiers(uint32_t modsDepressed, uint32_t modsLatched, uint32_t modsLocked, uint32_t group);
    void sendButton(uint32_t button, bool pressed);
    void sendScrollDelta(double x, double y);
    void sendScrollDiscrete(int32_t x, int32_t y);
    void sendScrollStop(bool stopX, bool stopY);
    void sendPointerFrame();

    int  getFileDescriptor();

    //

    std::string m_socketPath;

  private:
    struct SKeymap {
        int32_t  m_fd   = 0;
        uint32_t m_size = 0;
    };

    struct SClient {
        eis_client* m_handle = nullptr;
        eis_seat*   m_seat   = nullptr;

        eis_device* m_pointer  = nullptr;
        eis_device* m_keyboard = nullptr;
    } m_client;

    int              onEvent(eis_event* e);
    int              pollEvents();
    void             ensurePointer();
    void             ensureKeyboard();
    SKeymap          getKeymap();
    void             clearPointer();
    void             clearKeyboard();

    // The libeis emulation state gates which devices actually deliver to the
    // receiver, and it is per device: a device recreated after activate
    // (resetKeyboard on a keymap churn) starts unemulated, and every send
    // into it is dropped until emulation is re-armed with the live
    // activation sequence (2026-10-03: input-capture keys silently died
    // after the first keymap-driven keyboard reset).
    bool             m_emulating = false;
    int              m_emulatingSeq = 0;
    bool             m_stop   = false;
    eis*             m_eisCtx = nullptr;

    wl_event_source* m_eventSource = nullptr;
};
