add_executable(mayonnaise_tests test_main.cpp test_geometry.cpp test_text.cpp test_layout.cpp test_events.cpp test_widgets.cpp test_images.cpp test_settings.cpp test_fonts.cpp)
target_link_libraries(mayonnaise_tests PRIVATE mayonnaise::mayonnaise doctest::doctest mayonnaise_stb)
# The library already compiles stb_image_write's implementation; tests only use the declarations.
target_include_directories(mayonnaise_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
mayonnaise_set_warnings(mayonnaise_tests)
# doctest's registration macros use __COUNTER__, which recent Clang flags under -Wpedantic.
include(CheckCXXCompilerFlag)
check_cxx_compiler_flag(-Wno-c2y-extensions MAYONNAISE_HAS_WNO_C2Y)
if(MAYONNAISE_HAS_WNO_C2Y)
  target_compile_options(mayonnaise_tests PRIVATE -Wno-c2y-extensions)
endif()
mayonnaise_set_sanitizers(mayonnaise_tests)
# The terminal example's emulation core has no UI dependencies; test it with the library.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_sources(mayonnaise_tests PRIVATE test_terminal_screen.cpp ${PROJECT_SOURCE_DIR}/applications/terminal/screen.cpp)
  target_include_directories(mayonnaise_tests PRIVATE ${PROJECT_SOURCE_DIR}/applications/terminal)
endif()
# So is the calculator's arithmetic.
target_sources(mayonnaise_tests PRIVATE test_calculator_engine.cpp ${PROJECT_SOURCE_DIR}/applications/calculator/engine.cpp)
target_include_directories(mayonnaise_tests PRIVATE ${PROJECT_SOURCE_DIR}/applications/calculator)
# The System Settings example's KConfig reader/writer (KDE support) needs GIO, like the app.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
  pkg_check_modules(MAYO_TEST_GIO IMPORTED_TARGET gio-2.0 gio-unix-2.0)
  if(MAYO_TEST_GIO_FOUND)
    set(SETTINGS_DIR ${PROJECT_SOURCE_DIR}/applications/system_settings)
    target_sources(mayonnaise_tests PRIVATE test_kconfig.cpp ${SETTINGS_DIR}/kconfig.cpp ${SETTINGS_DIR}/glib_support.cpp
                   ${SETTINGS_DIR}/ui.cpp)
    target_include_directories(mayonnaise_tests PRIVATE ${SETTINGS_DIR})
    target_link_libraries(mayonnaise_tests PRIVATE PkgConfig::MAYO_TEST_GIO)
  endif()
endif()
add_test(NAME mayonnaise_tests COMMAND mayonnaise_tests)

# Text editor event-level regression tests run without a window or display server.
add_executable(text_editor_tests ${PROJECT_SOURCE_DIR}/applications/text_editor/test_editor.cpp
                                ${PROJECT_SOURCE_DIR}/applications/text_editor/editor.cpp)
target_link_libraries(text_editor_tests PRIVATE mayonnaise::mayonnaise doctest::doctest)
mayonnaise_set_warnings(text_editor_tests)
mayonnaise_set_sanitizers(text_editor_tests)
add_test(NAME text_editor_tests COMMAND text_editor_tests)

# App Store catalogue and UI layout regressions run without a display server.
if(TARGET forma-app-store)
  pkg_check_modules(FORMA_STORE_TEST_CURL REQUIRED IMPORTED_TARGET libcurl)
  add_executable(forma_store_tests ${PROJECT_SOURCE_DIR}/applications/app_store/tests/catalog_tests.cpp
                 ${PROJECT_SOURCE_DIR}/applications/app_store/tests/ui_tests.cpp
                 ${PROJECT_SOURCE_DIR}/applications/app_store/store_ui.cpp
                 ${PROJECT_SOURCE_DIR}/applications/app_store/images.cpp
                 ${PROJECT_SOURCE_DIR}/applications/app_store/catalog.cpp
                 ${PROJECT_SOURCE_DIR}/applications/app_store/json_lite.cpp)
  target_link_libraries(forma_store_tests PRIVATE mayonnaise::mayonnaise doctest::doctest PkgConfig::FORMA_STORE_TEST_CURL)
  target_include_directories(forma_store_tests PRIVATE ${PROJECT_SOURCE_DIR}/applications/app_store
                             ${PROJECT_SOURCE_DIR}/include)
  mayonnaise_set_warnings(forma_store_tests)
  mayonnaise_set_sanitizers(forma_store_tests)
  add_test(NAME forma_store_tests COMMAND forma_store_tests)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  pkg_check_modules(FINDER_TEST_GIO REQUIRED IMPORTED_TARGET gio-2.0)
  add_executable(finder_volume_tests test_finder_volumes.cpp ${PROJECT_SOURCE_DIR}/applications/finder/volumes.cpp)
  target_include_directories(finder_volume_tests PRIVATE ${PROJECT_SOURCE_DIR}/applications/finder ${PROJECT_SOURCE_DIR}/src)
  target_link_libraries(finder_volume_tests PRIVATE mayonnaise::mayonnaise PkgConfig::FINDER_TEST_GIO)
  mayonnaise_set_warnings(finder_volume_tests)
  mayonnaise_set_sanitizers(finder_volume_tests)
  add_test(NAME finder_volume_tests COMMAND finder_volume_tests)
  add_executable(finder_open_tests test_finder_open.cpp
    ${PROJECT_SOURCE_DIR}/applications/finder/applications.cpp
    ${PROJECT_SOURCE_DIR}/applications/finder/file_model.cpp
    ${PROJECT_SOURCE_DIR}/applications/finder/file_panels.cpp
    ${PROJECT_SOURCE_DIR}/applications/finder/panels.cpp
    ${PROJECT_SOURCE_DIR}/applications/finder/browser.cpp
    ${PROJECT_SOURCE_DIR}/applications/finder/file_icons.cpp
    ${PROJECT_SOURCE_DIR}/applications/finder/file_ops.cpp
    ${PROJECT_SOURCE_DIR}/applications/finder/preview_cache.cpp)
  target_include_directories(finder_open_tests PRIVATE ${PROJECT_SOURCE_DIR}/applications/finder ${PROJECT_SOURCE_DIR}/src)
  target_link_libraries(finder_open_tests PRIVATE mayonnaise::mayonnaise)
  mayonnaise_set_warnings(finder_open_tests)
  mayonnaise_set_sanitizers(finder_open_tests)
  add_test(NAME finder_open_tests COMMAND finder_open_tests)
endif()

# The screenshot backend can be tested without a desktop session.
if(TARGET screenshot_backend_test)
  add_test(NAME screenshot_backend COMMAND screenshot_backend_test)
endif()

if(FORMA_WAYLAND_SCANNER AND MAYONNAISE_HAS_WAYLAND_MATERIAL)
  pkg_check_modules(FORMA_WAYLAND_TEST QUIET IMPORTED_TARGET wayland-client)
  pkg_check_modules(FORMA_WAYLAND_SERVER QUIET IMPORTED_TARGET wayland-server)
  if(FORMA_WAYLAND_SERVER_FOUND)
    foreach(protocol ext-background-effect-v1 kos-surface-shape-v1)
      set(server_header "${CMAKE_CURRENT_BINARY_DIR}/${protocol}-server.h")
      add_custom_command(OUTPUT "${server_header}"
        COMMAND "${FORMA_WAYLAND_SCANNER}" server-header
          "${PROJECT_SOURCE_DIR}/src/platform/protocols/${protocol}.xml" "${server_header}"
        DEPENDS "${PROJECT_SOURCE_DIR}/src/platform/protocols/${protocol}.xml")
      target_sources(mayonnaise_tests PRIVATE "${server_header}")
    endforeach()
    target_sources(mayonnaise_tests PRIVATE test_wayland_material.cpp)
    target_include_directories(mayonnaise_tests PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
    target_link_libraries(mayonnaise_tests PRIVATE PkgConfig::FORMA_WAYLAND_SERVER PkgConfig::FORMA_WAYLAND_TEST)
  endif()
endif()
