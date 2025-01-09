add_library(FFGLTouchEngineFX SHARED)
target_sources(FFGLTouchEngineFX PRIVATE
    TouchEngineFX.h
    TouchEngineFX.cpp
    ../../lib/FFGL/FFGLSDK.h
    ../../lib/FFGL/FFGLSDK.cpp
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
    target_link_libraries(FFGLTouchEngineFX "-framework TouchEngine")
    set_target_properties(FFGLTouchEngineFX PROPERTIES
        XCODE_ATTRIBUTE_FRAMEWORK_SEARCH_PATHS ${CMAKE_CURRENT_SOURCE_DIR}/../../../lib/TouchEngine 
    )
endif()
target_include_directories(FFGLTouchEngineFX PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../../include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../lib>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(FFGLTouchEngineFX PUBLIC OpenGL::GL)
