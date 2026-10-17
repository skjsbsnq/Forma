# Linux-only: forkpty and /proc.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  mayonnaise_app(terminal ID io.github.egely1337.Mayonnaise.Terminal
    SOURCES main.cpp profiles.cpp pty.cpp screen.cpp terminal_view.cpp)
  find_library(MAYO_UTIL_LIB util)  # forkpty lives in libutil before glibc 2.34
  if(MAYO_UTIL_LIB)
    target_link_libraries(terminal PRIVATE ${MAYO_UTIL_LIB})
  endif()
endif()

# Keep Forma alongside the desktop's own terminal and KDE System Settings.
if(TARGET terminal)
  set_target_properties(terminal PROPERTIES OUTPUT_NAME forma-terminal)
endif()
