find_package(PkgConfig REQUIRED)
pkg_check_modules(SCREENSHOT_GIO REQUIRED IMPORTED_TARGET gio-unix-2.0)
find_package(PNG REQUIRED)
mayonnaise_app(forma-screenshot ID io.github.skjsbsnq.Forma.Screenshot SOURCES main.cpp capture.cpp capture_view.cpp session.cpp)
target_link_libraries(forma-screenshot PRIVATE PkgConfig::SCREENSHOT_GIO PNG::PNG)
install(PROGRAMS ${PROJECT_SOURCE_DIR}/tools/forma_screenshot_shortcut.py
  DESTINATION ${CMAKE_INSTALL_BINDIR} RENAME forma-screenshot-shortcut COMPONENT forma-screenshot)
install(PROGRAMS ${PROJECT_SOURCE_DIR}/tools/setup_packaged_screenshot.sh
  DESTINATION ${CMAKE_INSTALL_BINDIR} RENAME forma-screenshot-setup COMPONENT forma-screenshot)
if(MAYONNAISE_BUILD_TESTS)
  add_executable(screenshot_backend_test capture_test.cpp capture.cpp)
  target_link_libraries(screenshot_backend_test PRIVATE PkgConfig::SCREENSHOT_GIO PNG::PNG)
endif()
