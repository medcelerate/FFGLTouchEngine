add_library(FFGLTouchEngine SHARED)
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
    target_link_libraries(FFGLTouchEngine
    "-framework TouchEngine"
    "-framework Foundation"
    "-framework QuartzCore"
    "-framework Metal")
    set_target_properties(FFGLTouchEngine PROPERTIES
        XCODE_ATTRIBUTE_FRAMEWORK_SEARCH_PATHS ${CMAKE_CURRENT_SOURCE_DIR}/../../../lib/TouchEngine 
    )
    target_include_directories(FFGLTouchEngine PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/../../lib/metal-cpp
    )
endif()

target_include_directories(FFGLTouchEngine PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../../include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../lib>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../plugins/shared>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(FFGLTouchEngine PUBLIC OpenGL::GL)
