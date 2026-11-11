add_library(libdragon SHARED
        Architecture.cpp
        Instruction.cpp
        BasicBlock.cpp
        Function.cpp
        Graph.cpp
        IGNIL.cpp
        IGNILSimplify.cpp
        PassPipeline.cpp
        FiniteDispatch.cpp
        LLVMLifter.cpp
        LLVMDispatchSpecialization.cpp
        StateGuardEdges.cpp
        LLVMDeflattening.cpp
        CallEffectSummary.cpp
        GuestMemorySSA.cpp
        GuestMemoryEdges.cpp
        LLVMPreparedRegion.cpp
        LLVMLiftProvenance.cpp
        LLVMPredicateFolding.cpp
        LLVMMBASimplification.cpp
        LLVMExecution.cpp
        LLVMImportMetadata.cpp
)

add_library(dragon::libdragon ALIAS libdragon)

set_target_properties(libdragon PROPERTIES
        OUTPUT_NAME dragon
        WINDOWS_EXPORT_ALL_SYMBOLS ON
)

target_compile_features(libdragon PUBLIC cxx_std_20)

# LLVM includes
target_include_directories(libdragon
        PUBLIC
        $<INSTALL_INTERFACE:include>
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        ${LLVM_INCLUDE_DIRS}
        PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

# LLVM definitions (e.g. _DEBUG, NDEBUG)
target_compile_definitions(libdragon PRIVATE ${LLVM_DEFINITIONS})

# LLVM components needed
llvm_map_components_to_libnames(LLVM_LIBS
        MC
        MCDisassembler
        MCParser
        Object
        DebugInfoDWARF
        Support
        Passes
        TransformUtils
        X86
        X86Desc
        X86Info
        ARM
        ARMDesc
        ARMInfo
        AArch64
        AArch64Desc
        AArch64Info
        RISCV
        RISCVDesc
        RISCVInfo
)

target_link_libraries(libdragon
        PRIVATE
        ${LLVM_LIBS}
)

if(DRAGON_USE_Z3)
    # IGNIL symbolic execution: Z3 is not optional for it, unlike the rest of
    # libdragon, so the whole translation unit is gated the same way
    # analysis/SliceExprZ3.cpp is.
    target_sources(libdragon PRIVATE SymbolicExecutor.cpp)
endif()

add_subdirectory(disassemblers)
add_subdirectory(assemblers)
add_subdirectory(analysis)
add_subdirectory(sigs)
add_subdirectory(loader)
