add_library(libdragon_c SHARED
    dragon_c.cpp
)

set_target_properties(libdragon_c PROPERTIES
    OUTPUT_NAME dragon_c
    # On Windows a DLL exports nothing unless marked; the shim header uses plain
    # extern "C" (no __declspec(dllexport)). Auto-export all its symbols so the
    # Python/cffi bindings can resolve the dragon_* C API (Linux does this by
    # default). Safe here: the shim only defines its own small C API.
    WINDOWS_EXPORT_ALL_SYMBOLS ON
)

target_include_directories(libdragon_c
    PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
)

target_link_libraries(libdragon_c
    PRIVATE
    dragon::libdragon
)

if(DRAGON_USE_Z3)
    target_link_libraries(libdragon_c PRIVATE z3::libz3)
endif()
