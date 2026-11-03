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
        lifters/TestLLVMFunctionFlow.cpp
        lifters/TestLLVMDispatch.cpp
        lifters/TestLLVMGuestReturns.cpp
        lifters/TestLLVMImports.cpp
        lifters/TestLLVMStatic.cpp
        loader/TestLoader.cpp
)

if(DRAGON_USE_Z3)
    target_sources(dragon_tests PRIVATE
        analysis/TestSliceExprZ3.cpp
        obfush/TestObfushZ3.cpp
        symbolic/TestSymbolicExecutor.cpp
        symbolic/TestSymbolicCFG.cpp)
    target_sources(dragon_tests PRIVATE lifters/TestIGNILFloat.cpp lifters/TestX87.cpp)
endif()

if(DRAGON_USE_UNICORN AND DRAGON_USE_Z3)
    target_sources(dragon_tests PRIVATE semantic/TestX86Strings.cpp)
endif()

if(DRAGON_USE_UNICORN)
    target_sources(dragon_tests PRIVATE
        semantic/TestX86SemanticRoundTrip.cpp
        semantic/TestX86Div128.cpp
        semantic/TestLLVMDispatchExecution.cpp
        toy-vm/TestToyVM.cpp
        toy-vm/TestToyVMHandlerRecovery.cpp
    )
endif()

target_compile_definitions(dragon_tests PRIVATE
    DRAGON_FUNCTION_FLOW_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/llvm-function-flow.json")

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
# Keep C-shim tests separate: the main suite links LLVM static components
# directly, while libdragon_c already carries its own LLVM dependencies.
add_executable(dragon_c_tests TestCAnalysisHints.cpp)
target_link_libraries(dragon_c_tests PRIVATE libdragon_c Catch2::Catch2WithMain)
catch_discover_tests(dragon_c_tests)
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
        add_test(NAME "Python API - analysis hints and calling convention"
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/python/test_analysis_hints.py)
        set_tests_properties("Python API - analysis hints and calling convention"
                PROPERTIES ENVIRONMENT "DRAGON_C_LIB=$<TARGET_FILE:libdragon_c>")
        add_test(NAME "Validation - jump-table bounds follow the actual index"
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/python/test_table_bounds.py)
        add_test(NAME "Validation - static corpus summary rejects incomplete results"
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/python/test_static_corpus_summary.py)
        add_test(NAME "Validation - LLVM V2 C and Python mirror"
                 COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/python/test_llvm_v2.py)
        add_test(NAME "Validation - LLVM static C and Python mirror"
                 COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/python/test_llvm_static.py)
        set_tests_properties("Validation - LLVM static C and Python mirror" PROPERTIES
            ENVIRONMENT "DRAGON_C_LIB=$<TARGET_FILE:libdragon_c>")
        # Function execution retains machine branches, loops and Guest CALL/RET.
        # A/B are ordinary V2 regressions; Legacy keeps its explicit controls.
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            find_program(DRAGON_FLOW_LLC NAMES llc-21)
            find_program(DRAGON_FLOW_OPT NAMES opt-21)
            find_program(DRAGON_FLOW_CC NAMES gcc)
            execute_process(COMMAND ${Python3_EXECUTABLE} -c "import unicorn"
                            RESULT_VARIABLE _dragon_flow_unicorn_missing OUTPUT_QUIET ERROR_QUIET)
            if(DRAGON_FLOW_LLC AND DRAGON_FLOW_OPT AND DRAGON_FLOW_CC AND _dragon_flow_unicorn_missing EQUAL 0)
                add_test(NAME "Validation - LLVM whole-function positive controls"
                    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/validation/function_llvm_diff.py
                        --case control_store --case control_branch --case control_loop
                        --trials 2 --llc ${DRAGON_FLOW_LLC} --opt ${DRAGON_FLOW_OPT} --cc ${DRAGON_FLOW_CC}
                        --out ${CMAKE_CURRENT_BINARY_DIR}/function-flow-controls)
                add_test(NAME "Validation - LLVM V2 call state execution"
                    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/validation/function_call_diff.py
                        --trials 2 --llc ${DRAGON_FLOW_LLC} --opt ${DRAGON_FLOW_OPT} --cc ${DRAGON_FLOW_CC}
                        --out ${CMAKE_CURRENT_BINARY_DIR}/function-call-controls)
                set_tests_properties("Validation - LLVM V2 call state execution" PROPERTIES
                    ENVIRONMENT "DRAGON_C_LIB=$<TARGET_FILE:libdragon_c>" TIMEOUT 120)
                add_test(NAME "Validation - LLVM static external call state execution"
                    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/validation/function_static_diff.py
                        --trials 2 --out ${CMAKE_CURRENT_BINARY_DIR}/function-static-controls)
                set_tests_properties("Validation - LLVM static external call state execution" PROPERTIES
                    ENVIRONMENT "DRAGON_C_LIB=$<TARGET_FILE:libdragon_c>" TIMEOUT 120)
                add_test(NAME "Validation - LLVM static Guest calls and boundaries"
                    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/validation/function_static_guest_diff.py
                        --trials 2 --out ${CMAKE_CURRENT_BINARY_DIR}/function-static-guest-controls)
                add_test(NAME "Validation - LLVM static Exit contract"
                    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/validation/static_exit_contract.py
                        --trials 2 --out ${CMAKE_CURRENT_BINARY_DIR}/static-exit-controls)
                set_tests_properties("Validation - LLVM static Guest calls and boundaries" "Validation - LLVM static Exit contract" PROPERTIES
                    ENVIRONMENT "DRAGON_C_LIB=$<TARGET_FILE:libdragon_c>" TIMEOUT 180)
                add_test(NAME "Validation - generated function semantics and CFG truth"
                    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/validation/generated_function_diff.py
                        --seeds 1 --trials 8 --out ${CMAKE_CURRENT_BINARY_DIR}/generated-function-controls)
                add_test(NAME "Validation - generated functions detect wrong branch"
                    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/validation/generated_function_diff.py
                        --seeds 1 --trials 8 --control invert-branch --expect-control
                        --out ${CMAKE_CURRENT_BINARY_DIR}/generated-branch-negative)
                add_test(NAME "Validation - generated functions detect stale callee state"
                    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/validation/generated_function_diff.py
                        --seeds 1 --trials 8 --control stale-call --expect-control
                        --out ${CMAKE_CURRENT_BINARY_DIR}/generated-call-negative)
                set_tests_properties("Validation - generated function semantics and CFG truth"
                    "Validation - generated functions detect wrong branch"
                    "Validation - generated functions detect stale callee state" PROPERTIES
                    ENVIRONMENT "DRAGON_C_LIB=$<TARGET_FILE:libdragon_c>" TIMEOUT 180)
                add_test(NAME "Validation - complete Guest CALL and RET"
                    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/validation/function_guest_diff.py
                        --trials 2 --llc ${DRAGON_FLOW_LLC} --opt ${DRAGON_FLOW_OPT} --cc ${DRAGON_FLOW_CC}
                        --out ${CMAKE_CURRENT_BINARY_DIR}/function-guest-controls)
                set_tests_properties("Validation - complete Guest CALL and RET" PROPERTIES
                    ENVIRONMENT "DRAGON_C_LIB=$<TARGET_FILE:libdragon_c>" TIMEOUT 180)
                add_test(NAME "Validation - LLVM V2 branch and loop execution"
                    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/validation/function_dispatch_diff.py
                        --trials 2 --llc ${DRAGON_FLOW_LLC} --opt ${DRAGON_FLOW_OPT} --cc ${DRAGON_FLOW_CC}
                        --out ${CMAKE_CURRENT_BINARY_DIR}/function-dispatch-controls)
                set_tests_properties("Validation - LLVM V2 branch and loop execution" PROPERTIES
                    ENVIRONMENT "DRAGON_C_LIB=$<TARGET_FILE:libdragon_c>" TIMEOUT 120)
                set_tests_properties("Validation - LLVM whole-function positive controls" PROPERTIES
                    ENVIRONMENT "DRAGON_C_LIB=$<TARGET_FILE:libdragon_c>" TIMEOUT 120)
            else()
                message(STATUS "Function-flow execution controls need Python Unicorn plus llc-21/opt-21/gcc; standalone harness remains available")
            endif()
        endif()
    else()
        message(STATUS "Python cffi not found - Python API tests disabled")
    endif()
else()
    message(STATUS "Python 3 not found - Python API tests disabled")
endif()
