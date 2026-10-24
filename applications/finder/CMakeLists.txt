if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(FINDER_GIO REQUIRED IMPORTED_TARGET gio-2.0)
  mayonnaise_app(finder ID io.github.egely1337.Mayonnaise.Finder
    SOURCES applications.cpp main.cpp browser.cpp file_icons.cpp file_model.cpp file_ops.cpp file_panels.cpp panels.cpp
            preview_cache.cpp views.cpp volumes.cpp)
  target_link_libraries(finder PRIVATE PkgConfig::FINDER_GIO)
endif()
