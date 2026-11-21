cmake_minimum_required(VERSION 3.20)

# macOS: universal binary (Intel + Apple silicon), runs on 10.13 High Sierra+.
# Must be set before project().
if(APPLE)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "10.13" CACHE STRING "Minimum macOS")
  if(NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)
  endif()
endif()

project(hp8903 VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(HP8903_SYSTEM_LIBUSB "Link the system libusb instead of building it" OFF)
option(HP8903_BUILD_TESTS "Build unit tests" ON)

# ---------------------------------------------------------------- libusb
if(HP8903_SYSTEM_LIBUSB)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(LIBUSB REQUIRED IMPORTED_TARGET libusb-1.0)
  add_library(usb_dep ALIAS PkgConfig::LIBUSB)
else()
  include(FetchContent)
  set(LIBUSB_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  set(LIBUSB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(LIBUSB_BUILD_TESTING OFF CACHE BOOL "" FORCE)
  set(LIBUSB_ENABLE_UDEV OFF CACHE BOOL "" FORCE)
  set(LIBUSB_INSTALL_TARGETS OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(libusb
    GIT_REPOSITORY https://github.com/libusb/libusb-cmake.git
    GIT_TAG v1.0.29-0
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(libusb)
  add_library(usb_dep ALIAS usb-1.0)
endif()

# ---------------------------------------------------------------- core
add_library(hp8903core STATIC
  core/gpib/Transport.cpp
  core/gpib/NiUsbGpib.cpp
  core/gpib/MockTransport.cpp
  core/hp8903/HP8903.cpp
  core/hp8903/Sweep.cpp)
target_include_directories(hp8903core PUBLIC core)
target_link_libraries(hp8903core PUBLIC usb_dep)
if(APPLE)
  target_link_libraries(hp8903core PUBLIC "-framework IOKit" "-framework CoreFoundation" "-framework Security")
endif()
if(MSVC)
  target_compile_options(hp8903core PRIVATE /W3)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
  target_compile_options(hp8903core PRIVATE -Wall -Wextra)
endif()

# ---------------------------------------------------------------- tools
add_executable(hp8903-cli tools/hp8903_cli.cpp)
target_link_libraries(hp8903-cli PRIVATE hp8903core)

# ---------------------------------------------------------------- GUI
option(HP8903_BUILD_APP "Build the graphical application" ON)
if(HP8903_BUILD_APP)
  include(FetchContent)
  set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(glfw GIT_REPOSITORY https://github.com/glfw/glfw.git
                       GIT_TAG 3.4 GIT_SHALLOW TRUE)
  FetchContent_Declare(imgui GIT_REPOSITORY https://github.com/ocornut/imgui.git
                       GIT_TAG v1.93.0 GIT_SHALLOW TRUE)
  FetchContent_Declare(implot GIT_REPOSITORY https://github.com/epezent/implot.git
                       GIT_TAG v1.1 GIT_SHALLOW TRUE)
  FetchContent_Declare(stb GIT_REPOSITORY https://github.com/nothings/stb.git
                       GIT_TAG 6b5799bae8a1fe3792691ef4b72ae26434c3e799)
  FetchContent_MakeAvailable(glfw imgui implot stb)   # the last three are source-only

  find_package(OpenGL REQUIRED)
  add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
    ${implot_SOURCE_DIR}/implot.cpp ${implot_SOURCE_DIR}/implot_items.cpp)
  target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends
                             ${implot_SOURCE_DIR} ${stb_SOURCE_DIR})
  target_compile_definitions(imgui PUBLIC GL_SILENCE_DEPRECATION)
  target_link_libraries(imgui PUBLIC glfw OpenGL::GL)

  set(FONT_H ${CMAKE_BINARY_DIR}/generated/font_roboto.h)
  add_custom_command(OUTPUT ${FONT_H}
    COMMAND ${CMAKE_COMMAND} -DIN=${imgui_SOURCE_DIR}/misc/fonts/Roboto-Medium.ttf
            -DOUT=${FONT_H} -DNAME=kRobotoTtf -P ${CMAKE_SOURCE_DIR}/cmake/embed.cmake
    DEPENDS ${CMAKE_SOURCE_DIR}/cmake/embed.cmake)

  add_executable(HP8903 WIN32 MACOSX_BUNDLE app/main.cpp app/App.cpp ${FONT_H})
  target_include_directories(HP8903 PRIVATE ${CMAKE_BINARY_DIR}/generated)
  target_link_libraries(HP8903 PRIVATE hp8903core imgui)
  if(APPLE)
    set_target_properties(HP8903 PROPERTIES
      MACOSX_BUNDLE_INFO_PLIST ${CMAKE_SOURCE_DIR}/app/Info.plist.in
      OUTPUT_NAME HP8903)
  endif()
  if(MSVC)
    set_property(TARGET HP8903 PROPERTY LINK_FLAGS "/ENTRY:mainCRTStartup")
  endif()
endif()

# ---------------------------------------------------------------- tests
if(HP8903_BUILD_TESTS)
  enable_testing()
  add_executable(hp8903-tests tests/test_core.cpp)
  target_link_libraries(hp8903-tests PRIVATE hp8903core)
  add_test(NAME core COMMAND hp8903-tests)
endif()
