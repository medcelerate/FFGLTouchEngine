add_library(FFGLTouchEngineFX SHARED)
target_sources(FFGLTouchEngineFX PRIVATE
    TouchEngineFX.h
    TouchEngineFX.cpp
    ../../lib/FFGL/FFGLSDK.h
    ../../lib/FFGL/FFGLSDK.cpp
    ../shared/TouchEnginePluginBase.h
    ../shared/TouchEnginePluginBase.cpp
)


if (WIN32)
    target_link_directories(FFGLTouchEngineFX PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../lib/TouchEngine
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../lib/Glew
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../lib/Spout
    )
    target_link_libraries(FFGLTouchEngineFX PRIVATE
        Spout_static.lib
        TouchEngine.lib
        glew32s.lib
    )
endif()
if (APPLE)
    target_link_libraries(FFGLTouchEngineFX PRIVATE
         "-framework TouchEngine"
         "-framework Foundation"
         "-framework QuartzCore"
         "-framework Metal")
    set_target_properties(FFGLTouchEngineFX PROPERTIES
        XCODE_ATTRIBUTE_FRAMEWORK_SEARCH_PATHS ${CMAKE_CURRENT_SOURCE_DIR}/../../../lib/TouchEngine 
    )
    target_include_directories(FFGLTouchEngineFX PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/../../lib/metal-cpp
    )
endif()
target_include_directories(FFGLTouchEngineFX PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../../include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../lib>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../plugins/shared>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(FFGLTouchEngineFX PUBLIC OpenGL::GL)
