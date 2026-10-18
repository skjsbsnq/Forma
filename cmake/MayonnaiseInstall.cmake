include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS mayonnaise mayonnaise_glad
  EXPORT mayonnaiseTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT devel
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT devel
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT devel
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY include/mayonnaise DESTINATION ${CMAKE_INSTALL_INCLUDEDIR} COMPONENT devel)
install(FILES assets/fonts/Inter-LICENSE.txt DESTINATION ${CMAKE_INSTALL_DATADIR}/mayonnaise/licenses COMPONENT devel)
install(FILES third_party/mygo/LICENSE third_party/mygo/NOTICE
        DESTINATION ${CMAKE_INSTALL_DATADIR}/mayonnaise/licenses/mygo COMPONENT devel)

install(EXPORT mayonnaiseTargets
  NAMESPACE mayonnaise::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/mayonnaise
  COMPONENT devel)

configure_package_config_file(cmake/mayonnaiseConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/mayonnaiseConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/mayonnaise)
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/mayonnaiseConfigVersion.cmake
  COMPATIBILITY SameMinorVersion)
install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/mayonnaiseConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/mayonnaiseConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/mayonnaise
  COMPONENT devel)
