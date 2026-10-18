find_package(PkgConfig REQUIRED)
pkg_check_modules(FORMA_STORE_CURL REQUIRED IMPORTED_TARGET libcurl)
mayonnaise_app(forma-app-store ID io.github.skjsbsnq.Forma.AppStore
  SOURCES main.cpp store_ui.cpp catalog.cpp images.cpp json_lite.cpp)
target_link_libraries(forma-app-store PRIVATE PkgConfig::FORMA_STORE_CURL)
