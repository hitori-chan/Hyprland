#include <helpers/PowerSource.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <string>

// a fake /sys/class/power_supply
class CFakeSupplies {
  public:
    CFakeSupplies() {
        const auto NOW = std::chrono::steady_clock::now().time_since_epoch().count();
        m_path         = std::filesystem::temp_directory_path() / std::format("hyprland-power-supply-{}", NOW);
        std::filesystem::create_directories(m_path);
    }

    ~CFakeSupplies() {
        std::error_code ec;
        std::filesystem::remove_all(m_path, ec);
    }

    void add(const std::string& name, const std::map<std::string, std::string>& attributes) {
        std::filesystem::create_directories(m_path / name);
        for (const auto& [attribute, value] : attributes) {
            std::ofstream(m_path / name / attribute) << value << "\n";
        }
    }

    const std::filesystem::path& path() const {
        return m_path;
    }

  private:
    std::filesystem::path m_path;
};

TEST(PowerSource, noSuppliesIsADesktopOnAC) {
    CFakeSupplies supplies;
    EXPECT_FALSE(Power::onBattery(supplies.path()));
}

TEST(PowerSource, missingClassIsOnAC) {
    EXPECT_FALSE(Power::onBattery("/nonexistent/hyprland/power_supply"));
}

TEST(PowerSource, mainsOnline) {
    CFakeSupplies supplies;
    supplies.add("macsmc-battery", {{"type", "Battery"}, {"status", "Charging"}});
    supplies.add("macsmc-ac", {{"type", "Mains"}, {"online", "1"}});
    EXPECT_FALSE(Power::onBattery(supplies.path()));
}

TEST(PowerSource, mainsOffline) {
    CFakeSupplies supplies;
    supplies.add("macsmc-battery", {{"type", "Battery"}, {"status", "Discharging"}});
    supplies.add("macsmc-ac", {{"type", "Mains"}, {"online", "0"}});
    EXPECT_TRUE(Power::onBattery(supplies.path()));
}

TEST(PowerSource, usbSourceOnline) {
    CFakeSupplies supplies;
    supplies.add("BAT0", {{"type", "Battery"}});
    supplies.add("AC", {{"type", "Mains"}, {"online", "0"}});
    supplies.add("ucsi-source-psy-USBC000:001", {{"type", "USB"}, {"online", "0"}, {"scope", "System"}});
    supplies.add("ucsi-source-psy-USBC000:002", {{"type", "USB"}, {"online", "1"}, {"scope", "System"}});
    EXPECT_FALSE(Power::onBattery(supplies.path()));
}

TEST(PowerSource, batteryAlone) {
    CFakeSupplies supplies;
    supplies.add("BAT0", {{"type", "Battery"}});
    EXPECT_TRUE(Power::onBattery(supplies.path()));
}

// a wireless mouse's battery and its charging cable are the device's, not the system's
TEST(PowerSource, deviceScopeIgnored) {
    CFakeSupplies supplies;
    supplies.add("hidpp_battery_0", {{"type", "Battery"}, {"scope", "Device"}});
    supplies.add("hid-dock", {{"type", "USB"}, {"online", "1"}, {"scope", "Device"}});
    EXPECT_FALSE(Power::onBattery(supplies.path()));

    supplies.add("BAT0", {{"type", "Battery"}});
    EXPECT_TRUE(Power::onBattery(supplies.path()));
}

// an online that cannot be read is not online
TEST(PowerSource, unreadableOnline) {
    CFakeSupplies supplies;
    supplies.add("BAT0", {{"type", "Battery"}});
    supplies.add("AC", {{"type", "Mains"}, {"online", ""}});
    supplies.add("USB", {{"type", "USB"}});
    EXPECT_TRUE(Power::onBattery(supplies.path()));
}
