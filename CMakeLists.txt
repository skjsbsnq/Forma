cmake_minimum_required(VERSION 3.21)
project(mayonnaise VERSION 0.1.0 LANGUAGES C CXX)

set(MAYONNAISE_IS_TOP_LEVEL ${PROJECT_IS_TOP_LEVEL})

option(MAYONNAISE_BUILD_APPS "Build the Mayonnaise applications (Finder, Terminal, ...)" ${MAYONNAISE_IS_TOP_LEVEL})
# Older name, still honoured.
if(DEFINED MAYONNAISE_BUILD_EXAMPLES)
  set(MAYONNAISE_BUILD_APPS ${MAYONNAISE_BUILD_EXAMPLES})
endif()
option(MAYONNAISE_BUILD_TESTS "Build Mayonnaise tests" ${MAYONNAISE_IS_TOP_LEVEL})
option(FORMA_SCREENSHOT_ONLY "Build only the standalone Forma Screenshot application" OFF)
option(FORMA_POLKIT_AGENT_ONLY "Build only the standalone Forma polkit authentication agent" OFF)
option(MAYONNAISE_USE_SYSTEM_DEPS "Require system packages for SDL3/FreeType/doctest instead of fetching" OFF)
option(MAYONNAISE_VULKAN "Build the Vulkan renderer backend (used when the GPU supports it, else OpenGL)" ON)
option(MAYONNAISE_SANITIZE "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

include(MayonnaiseWarnings)
include(MayonnaiseDependencies)
include(Embed)

if(MAYONNAISE_BUILD_TESTS)
  enable_testing()
endif()

add_subdirectory(third_party)
add_subdirectory(src)

if(MAYONNAISE_BUILD_APPS)
  add_subdirectory(applications)
endif()

if(MAYONNAISE_BUILD_TESTS)
  add_subdirectory(tests)
endif()

# Installing a package config only makes sense when our dependencies are themselves installed.
if(NOT MAYONNAISE_FETCHED_DEPS)
  include(MayonnaiseInstall)
else()
  message(STATUS "Mayonnaise: install() disabled because dependencies were fetched")
endif()
