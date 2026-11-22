#include "PowerSource.hpp"
#include "fs/FsUtils.hpp"
#include "../Compositor.hpp"
#include "../debug/log/Logger.hpp"

#include <array>
#include <cerrno>
#include <string_view>

#include <hyprutils/string/Numeric.hpp>
#include <sys/socket.h>
#include <wayland-server-core.h>

#if defined(__linux__)
#include <linux/netlink.h>
#endif

using namespace Power;
using namespace Hyprutils::OS;
using namespace Hyprutils::String;
using namespace std::string_view_literals;

static const std::filesystem::path SUPPLIES = "/sys/class/power_supply";

// the kernel's test: power_supply_is_system_supplied()
bool Power::onBattery(const std::filesystem::path& supplies) {
    std::error_code ec;
    bool            systemSupply = false;

    for (auto it = std::filesystem::directory_iterator(supplies, ec); !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
        const auto& PATH = it->path();

        if (NFsUtils::readFileAsString(PATH / "scope") == "Device")
            continue;

        systemSupply = true;

        if (NFsUtils::readFileAsString(PATH / "type") == "Battery")
            continue;

        const auto ONLINE = NFsUtils::readFileAsString(PATH / "online");
        if (!ONLINE)
            continue;

        const auto VALUE = strToNumber<int>(*ONLINE);
        if (VALUE && *VALUE > 0)
            return false;
    }

    return systemSupply;
}

UP<CPowerSourceWatcher>& Power::watcher() {
    static UP<CPowerSourceWatcher> p = makeUnique<CPowerSourceWatcher>();
    return p;
}

static int onUeventsReadable(int fd, uint32_t mask, void* data) {
    sc<CPowerSourceWatcher*>(data)->onUevents();
    return 0;
}

CPowerSourceWatcher::CPowerSourceWatcher() : m_onBattery(Power::onBattery(SUPPLIES)) {
#if defined(__linux__)
    m_fd = CFileDescriptor{socket(AF_NETLINK, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, NETLINK_KOBJECT_UEVENT)};
    if (!m_fd.isValid()) {
        LOG(Log::ERR, "power: couldn't open a uevent socket, the power source will not be followed");
        return;
    }

    // group 1 is the kernel's own uevents, which any user may read
    const sockaddr_nl ADDR = {.nl_family = AF_NETLINK, .nl_pad = 0, .nl_pid = 0, .nl_groups = 1};
    if (bind(m_fd.get(), rc<const sockaddr*>(&ADDR), sizeof(ADDR)) < 0) {
        LOG(Log::ERR, "power: couldn't bind the uevent socket, the power source will not be followed");
        m_fd.reset();
        return;
    }

    if (g_pCompositor && g_pCompositor->m_wlEventLoop)
        m_source = wl_event_loop_add_fd(g_pCompositor->m_wlEventLoop, m_fd.get(), WL_EVENT_READABLE, ::onUeventsReadable, this);
#endif

    LOG(Log::DEBUG, "power: on {}", m_onBattery ? "battery" : "AC");
}

CPowerSourceWatcher::~CPowerSourceWatcher() {
    // a static: past the compositor's cleanup, the event loop is gone with its sources
    if (m_source && g_pCompositor && !g_pCompositor->m_isShuttingDown)
        wl_event_source_remove(m_source);
}

bool CPowerSourceWatcher::onBattery() const {
    return m_onBattery;
}

void CPowerSourceWatcher::onUevents() {
    // a uevent is "action@devpath" and its KEY=value pairs, each ended by a NUL
    std::array<char, 8192> buffer         = {};
    bool                   suppliesChange = false;

    while (true) {
        const auto LEN = recv(m_fd.get(), buffer.data(), buffer.size(), 0);
        if (LEN < 0 && errno == ENOBUFS) { // the socket overflowed: what was lost may have been a supply's
            suppliesChange = true;
            continue;
        }

        if (LEN <= 0)
            break;

        if (std::string_view{buffer.data(), sc<size_t>(LEN)}.contains("\0SUBSYSTEM=power_supply\0"sv))
            suppliesChange = true;
    }

    if (suppliesChange)
        update();
}

void CPowerSourceWatcher::update() {
    const bool ON_BATTERY = Power::onBattery(SUPPLIES);
    if (ON_BATTERY == m_onBattery)
        return;

    m_onBattery = ON_BATTERY;

    LOG(Log::DEBUG, "power: on {}", m_onBattery ? "battery" : "AC");

    m_events.changed.emit(m_onBattery);
}
