#pragma once

#include "memory/Memory.hpp"
#include "signal/Signal.hpp"

#include <filesystem>

#include <hyprutils/os/FileDescriptor.hpp>

struct wl_event_source;

namespace Power {
    // Whether the system runs on its battery, by the kernel's own test (power_supply_is_system_supplied()):
    // a supply that is not a battery and is online means AC, and a system with no supply at all, a desktop, is on AC.
    // Supplies in a device's scope, a mouse's battery, do not count.
    bool onBattery(const std::filesystem::path& supplies);

    // Follows the power source by the kernel's power_supply uevents.
    class CPowerSourceWatcher {
      public:
        CPowerSourceWatcher();
        ~CPowerSourceWatcher();

        CPowerSourceWatcher(const CPowerSourceWatcher&)            = delete;
        CPowerSourceWatcher(CPowerSourceWatcher&&)                 = delete;
        CPowerSourceWatcher& operator=(const CPowerSourceWatcher&) = delete;
        CPowerSourceWatcher& operator=(CPowerSourceWatcher&&)      = delete;

        bool                 onBattery() const;
        void                 onUevents();

        struct {
            CSignalT<bool> changed;
        } m_events;

      private:
        void                           update();

        Hyprutils::OS::CFileDescriptor m_fd;
        wl_event_source*               m_source    = nullptr;
        bool                           m_onBattery = false;
    };

    UP<CPowerSourceWatcher>& watcher();
}
