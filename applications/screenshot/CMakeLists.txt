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
  add_executable(screenshot_resident_fixture ../../tests/screenshot/resident_fixture.cpp
    capture_view.cpp capture.cpp session.cpp)
  target_include_directories(screenshot_resident_fixture PRIVATE ${PROJECT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/src)
  target_compile_definitions(screenshot_resident_fixture PRIVATE MAYO_DESKTOP_ID="io.github.Forma.ResidentTest")
  target_link_libraries(screenshot_resident_fixture PRIVATE mayonnaise::mayonnaise PkgConfig::SCREENSHOT_GIO PNG::PNG)
  mayonnaise_set_warnings(screenshot_resident_fixture)
  mayonnaise_set_sanitizers(screenshot_resident_fixture)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  add_test(NAME screenshot_resident_memory COMMAND ${Python3_EXECUTABLE}
    ${PROJECT_SOURCE_DIR}/tests/screenshot/resident_memory.py $<TARGET_FILE:screenshot_resident_fixture>)
  set_tests_properties(screenshot_resident_memory PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
endif()
