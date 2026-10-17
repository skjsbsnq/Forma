# Linux-only: GSettings and the desktop's D-Bus services through GIO (libglib2.0-dev / glib2-devel).
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  return()
endif()
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(MAYO_GIO IMPORTED_TARGET gio-2.0 gio-unix-2.0)
endif()
if(NOT MAYO_GIO_FOUND)
  message(STATUS "Mayonnaise: GIO not found, skipping System Settings")
  return()
endif()
mayonnaise_app(systemsettings ID io.github.egely1337.Mayonnaise.Settings
  SOURCES main.cpp glib_support.cpp ui.cpp panels.cpp panels_desktop.cpp panels_network.cpp panels_devices.cpp panels_input.cpp panels_system.cpp panels_kde.cpp kconfig.cpp json.cpp)
target_link_libraries(systemsettings PRIVATE PkgConfig::MAYO_GIO)

# Keep Forma alongside the desktop's own terminal and KDE System Settings.
set_target_properties(systemsettings PROPERTIES OUTPUT_NAME forma-settings)
