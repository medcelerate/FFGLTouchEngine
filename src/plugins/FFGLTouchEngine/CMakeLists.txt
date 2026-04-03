if (APPLE)
    # On macOS, FFGL plugins must be .bundle (loadable bundles)
    add_library(FFGLTouchEngine MODULE)
    set_target_properties(FFGLTouchEngine PROPERTIES
        BUNDLE TRUE
        BUNDLE_EXTENSION "bundle"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake/BundleInfo.plist.in"
        MACOSX_BUNDLE_BUNDLE_NAME "FFGLTouchEngine"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.evanclark.FFGLTouchEngine"
        MACOSX_BUNDLE_BUNDLE_VERSION "1.0"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0"
    )
else()
    add_library(FFGLTouchEngine SHARED)
endif()

target_sources(FFGLTouchEngine PRIVATE
    TouchEngine.h
    TouchEngine.cpp
    ../../lib/FFGL/FFGLSDK.h
    ../../lib/FFGL/FFGLSDK.cpp
    ../shared/TouchEnginePluginBase.h
    ../shared/TouchEnginePluginBase.cpp
)

if (WIN32)
    target_link_directories(FFGLTouchEngine PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../lib/TouchEngine
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../lib/Glew
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../lib/Spout
    )
    target_link_libraries(FFGLTouchEngine PRIVATE
        Spout_static.lib
        TouchEngine.lib
        glew32s.lib
    )
endif()
if (APPLE)
    # Link TouchEngine.framework from lib/TouchEngine
    set(TOUCHENGINE_FRAMEWORK_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../../../lib/TouchEngine/TouchEngine.framework")
    target_link_libraries(FFGLTouchEngine PRIVATE
        "${TOUCHENGINE_FRAMEWORK_PATH}"
        "-framework OpenGL"
        "-framework Metal"
        "-framework IOSurface"
        "-framework CoreFoundation"
        "-framework QuartzCore"
        "-framework Foundation"
    )
    # Framework search path so the linker/runtime can find TouchEngine.framework
    set_target_properties(FFGLTouchEngine PROPERTIES
        XCODE_ATTRIBUTE_FRAMEWORK_SEARCH_PATHS "${CMAKE_CURRENT_SOURCE_DIR}/../../../lib/TouchEngine"
        BUILD_RPATH "${CMAKE_CURRENT_SOURCE_DIR}/../../../lib/TouchEngine"
        INSTALL_RPATH "@loader_path/../Frameworks"
    )
    target_include_directories(FFGLTouchEngine PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../lib/metal-cpp
    )
    # TEMetal.h uses #import <Metal/Metal.h> which requires Obj-C++
    set_source_files_properties(
        TouchEngine.cpp
        ../shared/TouchEnginePluginBase.cpp
        ../../lib/FFGL/FFGLSDK.cpp
        PROPERTIES COMPILE_FLAGS "-x objective-c++"
    )
endif()

target_include_directories(FFGLTouchEngine PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../../include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../lib>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../plugins/shared>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(FFGLTouchEngine PUBLIC OpenGL::GL)
