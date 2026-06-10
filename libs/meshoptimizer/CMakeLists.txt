add_library(meshoptimizer
    vcacheoptimizer.cpp
    allocator.cpp
    simplifier.cpp
)
target_include_directories(meshoptimizer PUBLIC .)
