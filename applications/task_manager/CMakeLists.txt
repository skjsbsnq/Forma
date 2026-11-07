# Linux-only: reads /proc and /sys.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  mayonnaise_app(taskmanager ID io.github.egely1337.Mayonnaise.TaskManager
    SOURCES main.cpp system_stats.cpp process_control.cpp widgets.cpp)
  # UI-free executable; the root test list can register it without launching a GUI.
  if(MAYONNAISE_BUILD_TESTS AND TARGET doctest::doctest)
    add_executable(taskmanager_process_tests test_process_control.cpp process_control.cpp system_stats.cpp)
    target_link_libraries(taskmanager_process_tests PRIVATE doctest::doctest)
    target_compile_features(taskmanager_process_tests PRIVATE cxx_std_20)
    mayonnaise_set_warnings(taskmanager_process_tests)
    mayonnaise_set_sanitizers(taskmanager_process_tests)
  endif()
endif()

if(TARGET taskmanager)
  set_target_properties(taskmanager PROPERTIES OUTPUT_NAME forma-taskmanager)
endif()
