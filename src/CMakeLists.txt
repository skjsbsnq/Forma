add_library(mayonnaise
  core/animation.cpp
  core/app.cpp
  core/color.cpp
  core/event_dispatcher.cpp
  core/layout.cpp
  core/settings.cpp
  core/test_scene.cpp
  core/widget.cpp
  core/window.cpp
  core/window_root.cpp
  platform/portal.cpp
  platform/sdl_audio.cpp
  platform/sdl_platform.cpp
  platform/wayland_drag.cpp
  render/gl.cpp
  render/rhi/gl_device.cpp
  render/rhi/rhi.cpp
  render/font.cpp
  render/font_api.cpp
  render/image_io.cpp
  render/painter.cpp
  render/renderer2d.cpp
  render/text_system.cpp
  render/video_texture.cpp
  theme/theme.cpp
  widgets/appearance_settings.cpp
  widgets/button.cpp
  widgets/control.cpp
  widgets/form.cpp
  widgets/icons.cpp
  widgets/image.cpp
  widgets/label.cpp
  widgets/menu.cpp
  widgets/popup.cpp
  widgets/progress_bar.cpp
  widgets/scroll_physics.cpp
  widgets/scroll_view.cpp
  widgets/segmented_control.cpp
  widgets/sidebar.cpp
  widgets/slider.cpp
  widgets/split_view.cpp
  widgets/text_field.cpp
  widgets/toggle.cpp
  widgets/toolbar.cpp
)
add_library(mayonnaise::mayonnaise ALIAS mayonnaise)

target_compile_features(mayonnaise PUBLIC cxx_std_20)
set_target_properties(mayonnaise PROPERTIES CXX_EXTENSIONS OFF)
target_include_directories(mayonnaise
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})
# SDL/GL/FreeType are implementation details: public headers never include them.
target_link_libraries(mayonnaise
  PRIVATE
    mayonnaise_glad  # static: must be exported so installed consumers link it
    $<BUILD_INTERFACE:mayonnaise_stb>
    SDL3::SDL3
    Freetype::Freetype
    OpenGL::GL
    ${CMAKE_DL_LIBS})  # libdbus is dlopen()ed for desktop preferences
mayonnaise_set_warnings(mayonnaise)
mayonnaise_set_sanitizers(mayonnaise)

mayonnaise_embed(mayonnaise
  ${PROJECT_SOURCE_DIR}/shaders/ui.vert
  ${PROJECT_SOURCE_DIR}/shaders/ui.frag
  ${PROJECT_SOURCE_DIR}/shaders/fullscreen.vert
  ${PROJECT_SOURCE_DIR}/shaders/blur_down.frag
  ${PROJECT_SOURCE_DIR}/shaders/blur_up.frag
  ${PROJECT_SOURCE_DIR}/assets/fonts/InterVariable.ttf
)

if(MAYONNAISE_VULKAN)
  target_sources(mayonnaise PRIVATE
    render/rhi/vk_allocator.cpp
    render/rhi/vk_device.cpp
    render/rhi/vk_dmabuf.cpp)
  target_include_directories(mayonnaise SYSTEM PRIVATE ${MAYONNAISE_VULKAN_INCLUDE_DIR})
  target_compile_definitions(mayonnaise PRIVATE MAYONNAISE_HAS_VULKAN=1)
  mayonnaise_embed_spirv(mayonnaise
    ${PROJECT_SOURCE_DIR}/shaders/ui.vert
    ${PROJECT_SOURCE_DIR}/shaders/ui.frag
    ${PROJECT_SOURCE_DIR}/shaders/fullscreen.vert
    ${PROJECT_SOURCE_DIR}/shaders/blur_down.frag
    ${PROJECT_SOURCE_DIR}/shaders/blur_up.frag)
endif()

# Optional compositor blur; ordinary builds retain the opaque material fallback.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(FORMA_WAYLAND QUIET IMPORTED_TARGET wayland-client)
endif()
find_program(FORMA_WAYLAND_SCANNER wayland-scanner)
target_sources(mayonnaise PRIVATE platform/wayland_material.cpp)
if(FORMA_WAYLAND_FOUND AND FORMA_WAYLAND_SCANNER)
  foreach(protocol ext-background-effect-v1 kos-surface-shape-v1)
    set(xml "${CMAKE_CURRENT_SOURCE_DIR}/platform/protocols/${protocol}.xml")
    set(header "${CMAKE_CURRENT_BINARY_DIR}/${protocol}-client.h")
    set(code "${CMAKE_CURRENT_BINARY_DIR}/${protocol}-protocol.c")
    add_custom_command(OUTPUT "${header}" COMMAND "${FORMA_WAYLAND_SCANNER}" client-header "${xml}" "${header}" DEPENDS "${xml}")
    add_custom_command(OUTPUT "${code}" COMMAND "${FORMA_WAYLAND_SCANNER}" private-code "${xml}" "${code}" DEPENDS "${xml}")
    target_sources(mayonnaise PRIVATE "${header}" "${code}")
  endforeach()
  target_include_directories(mayonnaise PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
  target_compile_definitions(mayonnaise PRIVATE MAYONNAISE_WAYLAND_MATERIAL=1)
  target_link_libraries(mayonnaise PRIVATE PkgConfig::FORMA_WAYLAND)
  set(MAYONNAISE_HAS_WAYLAND_MATERIAL TRUE PARENT_SCOPE)
endif()
