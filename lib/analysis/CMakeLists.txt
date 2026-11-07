target_sources(libdragon
        PRIVATE
        SliceExpr.cpp
        BackwardSlicer.cpp
        FiniteValues.cpp
        TableContents.cpp
        LeafCallEffects.cpp
        SwitchDetector.cpp
        GuardProof.cpp
        ArgumentRecovery.cpp
)

if(DRAGON_USE_Z3)
    target_sources(libdragon PRIVATE SliceExprZ3.cpp ValueEnumerator.cpp)
    target_link_libraries(libdragon PRIVATE z3::libz3)
    target_compile_definitions(libdragon PUBLIC DRAGON_WITH_Z3)
endif()
