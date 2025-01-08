cmake_minimum_required(VERSION 3.10)

# This logic needs to be considered before project()
set(_change_MSVC_flags FALSE)
if(WIN32)
  if(CMAKE_VERSION VERSION_LESS 3.15.0)
    set(_change_MSVC_flags TRUE)
  else()
    # Set MSVC runtime to MultiThreaded (/MT)
    cmake_policy(SET CMP0091 NEW)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  endif()
endif()

# Set the project name
project(FFGLTouchEngine
    VERSION 1.0
    LANGUAGES CXX
)

if(_change_MSVC_flags)
  # Modify compile flags to change MSVC runtime from /MD to /MT
  set(_re_match "([\\/\\-]M)D")
  set(_re_replace "\\1T")
  string(REGEX REPLACE ${_re_match} ${_re_replace}
    CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
  string(REGEX REPLACE ${_re_match} ${_re_replace}
    CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
  string(REGEX REPLACE ${_re_match} ${_re_replace}
    CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}")
  string(REGEX REPLACE ${_re_match} ${_re_replace}
    CMAKE_CXX_FLAGS_MINSIZEREL "${CMAKE_CXX_FLAGS_MINSIZEREL}")
  string(REGEX REPLACE ${_re_match} ${_re_replace}
    CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO}")
endif()
find_package(OpenGL REQUIRED)

# resolve dependencies if using vcpkg
if (EXISTS CACHE{VCPKG_MANIFEST_FILE})
    vcpkg_resolve_dependencies()
endif()

# Specify the C++ standard
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# we need glew (except on macOS)
if (NOT APPLE)
    # find_package(GLEW REQUIRED)
    # target_link_libraries(ffgl-sdk PRIVATE GLEW::GLEW)
    link_directories(${CMAKE_CURRENT_SOURCE_DIR}/lib/Glew)
endif()

set(CMAKE_SUPPRESS_REGENERATION true)
set(CMAKE_SKIP_INSTALL_ALL_DEPENDENCY true)

if (WIN32)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
endif()

add_subdirectory(src/plugins)