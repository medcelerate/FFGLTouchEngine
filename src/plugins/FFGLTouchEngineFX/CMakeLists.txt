add_library(FFGLTouchEngineFX SHARED)
target_sources(FFGLTouchEngineFX PRIVATE
    TouchEngineFX.h
    TouchEngineFX.cpp
    ../../lib/FFGL/FFGLSDK.h
    ../../lib/FFGL/FFGLSDK.cpp
)
target_link_directories(FFGLTouchEngineFX PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../lib/Glew
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../lib/Spout
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../lib/TouchEngine
)
target_link_libraries(FFGLTouchEngineFX PRIVATE
    Spout_static.lib
    TouchEngine.lib
    glew32s.lib
)
target_include_directories(FFGLTouchEngineFX PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../../include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../lib>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(FFGLTouchEngineFX PRIVATE OpenGL::GL)
