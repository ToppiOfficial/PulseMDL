add_library(minicollision STATIC
    ConvexHull.cpp
    PhysicsCollision.cpp
    PhysicsSurfaceProps.cpp
)

target_include_directories(minicollision
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}           # MiniVPhysics.h, ivp_compact.h, etc.
    PRIVATE ${CMAKE_SOURCE_DIR}/include           # vphysics_interface.h, mathlib/, etc.
            ${CMAKE_SOURCE_DIR}/libs/mathlib
            ${CMAKE_SOURCE_DIR}/libs/tier0
            ${CMAKE_SOURCE_DIR}/libs/tier1
)

target_link_libraries(minicollision PRIVATE mathlib tier0 vhacd)

target_compile_definitions(minicollision PRIVATE
    ${PLATFORM_DEFINES}
    RAD_TELEMETRY_DISABLED
    _CRT_SECURE_NO_WARNINGS
    USE_MEM_DEBUG=0
)
