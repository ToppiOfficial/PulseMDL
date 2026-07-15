cmake_minimum_required(VERSION 3.22)
project(PulseMDL)

set(CMAKE_CXX_STANDARD 14)
if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    if (CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(PLATFORM_DEFINES COMPILER_MSVC64 COMPILER_MSVC _AMD64_)
    elseif (CMAKE_SIZEOF_VOID_P EQUAL 4)
        set(PLATFORM_DEFINES COMPILER_MSVC32 COMPILER_MSVC _X86_)
    endif ()
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    if (CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(PLATFORM_DEFINES LINUX COMPILER_GCC POSIX _AMD64_)
    elseif (CMAKE_SIZEOF_VOID_P EQUAL 4)
        set(PLATFORM_DEFINES LINUX COMPILER_GCC POSIX _X86_)
    endif ()
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    # Do something on macOS
else()
    # Do something on other operating systems
endif()

add_compile_definitions(${PLATFORM_DEFINES} RAD_TELEMETRY_DISABLED _CRT_SECURE_NO_WARNINGS USE_MEM_DEBUG=0 _DLL_EXT=.dll)

add_subdirectory(libs/appframework)
add_subdirectory(libs/meshoptimizer)
add_subdirectory(libs/tier0)
add_subdirectory(libs/tier1)
add_subdirectory(libs/tier2)
add_subdirectory(libs/vstdlib)
add_subdirectory(libs/mathlib)
add_subdirectory(libs/datamodel)
add_subdirectory(libs/movieobjects)
add_subdirectory(libs/mdlobjects)
add_subdirectory(libs/dmserializers)
add_subdirectory(libs/dmeutils)
add_subdirectory(libs/utils)
add_subdirectory(libs/bonesetup)
add_subdirectory(libs/meshutils)
add_subdirectory(libs/interfaces)
add_subdirectory(libs/filesystem)
add_subdirectory(libs/vhacd)
add_subdirectory(libs/minicollision)


# Windows: embed the application icon + version info into the .exe.
# The .rc is always compiled (for version metadata). The icon line inside it is
# guarded by PULSEMDL_HAS_ICON, which we only define when the .ico is present, so
# the build keeps working until studiomdl/resources/pulsemdl.ico is added.
# set(PULSEMDL_RESOURCES "")
# if(WIN32)
#     enable_language(RC)
#     set(PULSEMDL_RC "${CMAKE_CURRENT_SOURCE_DIR}/studiomdl/resources/pulsemdl.rc")
#     if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/studiomdl/resources/pulsemdl.ico")
#         set_source_files_properties(${PULSEMDL_RC} PROPERTIES COMPILE_DEFINITIONS PULSEMDL_HAS_ICON)
#     else()
#         message(STATUS "pulsemdl: studiomdl/resources/pulsemdl.ico not found; building without an embedded icon (version info still embedded).")
#     endif()
#     set(PULSEMDL_RESOURCES ${PULSEMDL_RC})
# endif()

add_executable(pulsemdl
        studiomdl/dmxsupport.cpp
        studiomdl/mrmsupport.cpp
        studiomdl/v1support.cpp
        studiomdl/perfstats.cpp
        studiomdl/hardwarematrixstate.cpp
        studiomdl/hardwarevertexcache.cpp
        studiomdl/objsupport.cpp
        studiomdl/optimize.cpp
        studiomdl/optimize_subd.cpp
        studiomdl/simplify.cpp
        studiomdl/tristrip.cpp
        studiomdl/UnifyLODs.cpp
        studiomdl/write.cpp
        studiomdl/studiomdl.cpp
        studiomdl/filesystem_init.cpp
        studiomdl/studiomdl_commands.cpp
        studiomdl/studiomdl_errors.cpp
        studiomdl/studiomdl_app.cpp
        studiomdl/collisionmodel.cpp
        studiomdl/collisionmodelsource.cpp
        studiomdl/convexdecompose.cpp
        studiomdl/studiomdl_app.h
        ${PULSEMDL_RESOURCES}
)
target_compile_definitions(pulsemdl PRIVATE -DSTATIC_TIER0)
target_include_directories(pulsemdl PUBLIC include libs/minicollision)
target_link_libraries(pulsemdl PRIVATE
        tier0
        tier1
        tier2
        mathlib
        utils
        datamodel
        dmserializers
        movieobjects
        bonesetup
        mdlobjects
        dmeutils
        meshutils
        appframework
        meshoptimizer
        shlwapi
        filesystem_stdio_v2_static
        minicollision
        vhacd
        )

set_property(TARGET pulsemdl PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)

