add_library(vhacd STATIC
    vhacd_impl.cpp
)

target_include_directories(vhacd
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}      # VHACD.h, vhacd_hull.h
)

target_compile_definitions(vhacd PRIVATE
    ${PLATFORM_DEFINES}
    _CRT_SECURE_NO_WARNINGS
)
