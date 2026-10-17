if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  mayonnaise_app(finder ID io.github.egely1337.Mayonnaise.Finder
    SOURCES applications.cpp main.cpp browser.cpp file_icons.cpp file_model.cpp file_ops.cpp file_panels.cpp panels.cpp
            preview_cache.cpp views.cpp)
endif()
