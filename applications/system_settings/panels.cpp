#include "panels.hpp"

#include <vector>

#include "kconfig.hpp"

using namespace mayo;

namespace {

using TC = ThemeColors;

const PanelInfo kPanels[] = {
    {"wifi", "Wi-Fi", Icon::Wifi, &TC::blue, 0, "wireless wlan network internet hotspot airplane", wifiPanel},
    {"network", "Network", Icon::Network, &TC::blue, 0, "ethernet wired vpn proxy ip dns", networkPanel},
    {"bluetooth", "Bluetooth", Icon::Bluetooth, &TC::blue, 0, "devices pair headphones", bluetoothPanel},
    {"printers", "Printers", Icon::Printer, &TC::gray, 0, "print queue cups paper default printer jobs", printersPanel},

    {"displays", "Displays", Icon::Display, &TC::blue, 1,
     "monitor screen resolution refresh rate scale night light brightness orientation", displaysPanel},
    {"sound", "Sound", Icon::Speaker, &TC::red, 1, "audio volume output input microphone speakers alert",
     soundPanel},
    {"power", "Power", Icon::Battery, &TC::green, 1, "battery energy suspend sleep power mode performance",
     powerPanel},

    {"appearance", "Appearance", Icon::Paintbrush, &TC::indigo, 2,
     "dark light style accent color wallpaper background fonts", appearancePanel},
    {"multitasking", "Multitasking", Icon::Windows, &TC::indigo, 2,
     "workspaces hot corner edge tiling app switching", multitaskingPanel},
    {"notifications", "Notifications", Icon::Bell, &TC::red, 2, "do not disturb banners lock screen",
     notificationsPanel},
    {"search", "Search", Icon::Magnifier, &TC::gray, 2, "search providers locations", searchPanel},
    {"apps", "Apps", Icon::Grid, &TC::purple, 2, "default applications browser mail removable media autorun",
     appsPanel},
    {"effects", "Desktop Effects", Icon::Sparkles, &TC::purple, 2,
     "compositor effects animations scripts kwin blur window open close", kde::effectsPanel},
    {"decorations", "Window Decorations", Icon::Windows, &TC::indigo, 2,
     "titlebar buttons border decoration theme window manager", kde::decorationsPanel},

    {"mouse", "Mouse & Touchpad", Icon::Mouse, &TC::gray, 3,
     "pointer speed scrolling natural tap click acceleration trackpad", mousePanel},
    {"keyboard", "Keyboard", Icon::Keyboard, &TC::gray, 3, "input sources layout shortcuts repeat compose",
     keyboardPanel},
    {"accessibility", "Accessibility", Icon::Accessibility, &TC::blue, 3,
     "a11y contrast large text zoom screen reader animations cursor sticky keys", accessibilityPanel},

    {"privacy", "Privacy & Security", Icon::Hand, &TC::blue, 4,
     "screen lock location camera microphone file history trash", privacyPanel},
    {"wellbeing", "Wellbeing", Icon::Hourglass, &TC::teal, 4, "screen time breaks limits", wellbeingPanel},

    {"region", "Region & Language", Icon::Globe, &TC::blue, 5, "locale formats language", regionPanel},
    {"locations", "Locations", Icon::Folder, &TC::orange, 5, "home folders desktop documents downloads music pictures videos",
     locationsPanel},
    {"autostart", "Login Items", Icon::Play, &TC::green, 5, "autostart startup login apps session", autostartPanel},
    {"datetime", "Date & Time", Icon::Clock, &TC::blue, 5, "clock timezone ntp calendar", dateTimePanel},
    {"users", "Users", Icon::PersonCircle, &TC::orange, 5, "account name avatar password login", usersPanel},
    {"about", "About", Icon::Info, &TC::gray, 5, "system details hardware os version device name", aboutPanel},
};

}  // namespace

std::span<const PanelInfo> panels() {
    // Wellbeing (screen time limits, break reminders) only exists in GNOME.
    static const std::vector<PanelInfo> list = [] {
        std::vector<PanelInfo> out;
        for (const auto& p : kPanels) {
            const std::string id = p.id;
            // Wellbeing is GNOME-only; the KWin panels are Plasma-only. Everything else (NetworkManager,
            // BlueZ, CUPS, xdg-user-dirs, autostart) exists on both desktops.
            if (desk::kde() ? id == "wellbeing" : id == "effects" || id == "decorations") continue;
            out.push_back(p);
        }
        return out;
    }();
    return list;
}

const PanelInfo* findPanel(const std::string& id) {
    for (const auto& p : panels()) {
        if (id == p.id) return &p;
    }
    return nullptr;
}
