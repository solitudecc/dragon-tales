add_executable(dragon_tests
        TestMain.cpp
        disassemblers/TestDisassembler.cpp
        disassemblers/TestX86Analysis.cpp
        disassemblers/TestX86IGNILLifter.cpp
        disassemblers/TestARMAnalysis.cpp
        disassemblers/TestARMIGNILLifter.cpp
        disassemblers/TestTraceLifter.cpp
        assemblers/TestAssembler.cpp
        analysis/TestBackwardSlicer.cpp
        analysis/TestSwitchDetector.cpp
        analysis/TestIGNILSimplify.cpp
        analysis/TestDeadStoreElimination.cpp
        analysis/TestArgumentRecovery.cpp
        sigs/TestPatternParser.cpp
        sigs/TestSignatureMatcher.cpp
        obfush/TestObfush.cpp
        lifters/TestLLVMLifter.cpp
        loader/TestLoader.cpp
)

if(DRAGON_USE_Z3)
    target_sources(dragon_tests PRIVATE
        analysis/TestSliceExprZ3.cpp
        obfush/TestObfushZ3.cpp
        symbolic/TestSymbolicExecutor.cpp
        symbolic/TestSymbolicCFG.cpp)
endif()

if(DRAGON_USE_UNICORN)
    target_sources(dragon_tests PRIVATE
        semantic/TestX86SemanticRoundTrip.cpp
        semantic/TestX86Div128.cpp
        toy-vm/TestToyVM.cpp
        toy-vm/TestToyVMHandlerRecovery.cpp
    )
endif()

target_link_libraries(dragon_tests
        PRIVATE
        dragon::libdragon
        Catch2::Catch2WithMain
)

# The test sources call LLVM APIs directly (raw_ostream, Module, verifyFunction,
# etc.) and are compiled regardless of the optional features. libdragon links
# LLVM PRIVATEly, so on Windows those symbols are not re-exported through
# dragon::libdragon and the test binary must link the LLVM components itself.
# (On Linux the shared libdragon re-exports them, so this was never needed.)
# Mirror libdragon's component set.
llvm_map_components_to_libnames(TEST_LLVM_LIBS
        Core
        MC
        MCDisassembler
        MCParser
        Object
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
target_link_libraries(dragon_tests PRIVATE ${TEST_LLVM_LIBS})

if(DRAGON_USE_Z3)
    target_link_libraries(dragon_tests PRIVATE z3::libz3)
endif()

if(DRAGON_USE_UNICORN)
    # orcjit/native: only the semantic round-trip tests JIT-execute lifted
    # IR, so this stays test-only rather than a libdragon dependency.
    llvm_map_components_to_libnames(LLVM_JIT_LIBS orcjit native)
    target_link_libraries(dragon_tests PRIVATE PkgConfig::UNICORN ${LLVM_JIT_LIBS})

    # MSVC lacks the compiler-rt i128 helpers (__udivti3, ...) that the
    # 64-bit DIV/IDIV tests' JIT code calls.
    if(MSVC)
        find_file(DRAGON_COMPILER_RT_BUILTINS clang_rt.builtins-x86_64.lib
            HINTS "${LLVM_LIBRARY_DIR}/clang/${LLVM_VERSION_MAJOR}/lib/windows"
            DOC "clang_rt.builtins-x86_64.lib for the 64-bit DIV/IDIV semantic tests"
        )
        if(DRAGON_COMPILER_RT_BUILTINS)
            target_compile_definitions(dragon_tests PRIVATE
                DRAGON_TEST_COMPILER_RT_BUILTINS="${DRAGON_COMPILER_RT_BUILTINS}")
        else()
            message(STATUS "clang_rt.builtins-x86_64.lib not found - the 64-bit "
                           "DIV/IDIV semantic tests cannot link; set "
                           "DRAGON_COMPILER_RT_BUILTINS to it")
        endif()
    endif()
endif()

target_include_directories(dragon_tests
        PRIVATE
        ${PROJECT_SOURCE_DIR}/include
)

include(CTest)
include(Catch)
catch_discover_tests(dragon_tests)
# Python API tests. Optional like Unicorn: they need a Python 3 with cffi,
# and are simply not registered without one.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    execute_process(COMMAND ${Python3_EXECUTABLE} -c "import cffi"
                    RESULT_VARIABLE _dragon_cffi_missing OUTPUT_QUIET ERROR_QUIET)
    if(_dragon_cffi_missing EQUAL 0)
        add_test(NAME "Python API - borrowed views keep their Graph alive"
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/python/test_lifetimes.py)
        set_tests_properties("Python API - borrowed views keep their Graph alive"
                PROPERTIES ENVIRONMENT "DRAGON_C_LIB=$<TARGET_FILE:libdragon_c>")
        add_test(NAME "Python API - names that are not UTF-8 decode"
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/python/test_robustness.py)
        set_tests_properties("Python API - names that are not UTF-8 decode"
                PROPERTIES ENVIRONMENT "DRAGON_C_LIB=$<TARGET_FILE:libdragon_c>")
    else()
        message(STATUS "Python cffi not found - Python API tests disabled")
    endif()
else()
    message(STATUS "Python 3 not found - Python API tests disabled")
endif()
