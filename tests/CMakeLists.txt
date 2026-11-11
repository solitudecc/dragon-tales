add_executable(dragon_tests
        TestMain.cpp
        disassemblers/TestDisassembler.cpp
        disassemblers/TestX86Analysis.cpp
        disassemblers/TestDecodedEntries.cpp
        disassemblers/TestTableContents.cpp
        disassemblers/TestInferredBranches.cpp
        disassemblers/TestARMTableClosure.cpp
        disassemblers/TestARMCaseMembership.cpp
        disassemblers/TestTableContentBudgets.cpp
        disassemblers/TestX86DecodeClosure.cpp
        disassemblers/TestFiniteDispatch.cpp
        disassemblers/TestCallEffectSafety.cpp
        disassemblers/TestX86IGNILLifter.cpp
        disassemblers/TestX86Segments.cpp
        disassemblers/TestX86Addressing.cpp
        disassemblers/TestARMAnalysis.cpp
        disassemblers/TestARMIGNILLifter.cpp
        disassemblers/TestTraceLifter.cpp
        assemblers/TestAssembler.cpp
        analysis/TestBackwardSlicer.cpp
        analysis/TestFiniteValues.cpp
        analysis/TestFiniteExpressionFacts.cpp
        analysis/TestFiniteStoreBudget.cpp
        analysis/TestFiniteWriteFootprints.cpp
        analysis/TestFiniteXorCost.cpp
        analysis/TestFiniteXorSpan.cpp
        analysis/TestValueSetAccumulator.cpp
        analysis/TestFiniteMemory.cpp
        analysis/TestFiniteInitialState.cpp
        analysis/TestCallerContext.cpp
        analysis/TestCallMemoryEffects.cpp
        analysis/TestCallMemorySafety.cpp
        analysis/TestCallSnapshotIdentity.cpp
        analysis/TestSwitchDetector.cpp
        analysis/TestConstantValue.cpp
        analysis/TestIGNILSimplify.cpp
        analysis/TestDeadStoreElimination.cpp
        analysis/TestArgumentRecovery.cpp
        sigs/TestPatternParser.cpp
        sigs/TestSignatureMatcher.cpp
        obfush/TestObfush.cpp
        lifters/TestLLVMLifter.cpp
        lifters/TestLLVMLegacyUnlifted.cpp
        lifters/TestSystemTransitionBoundaries.cpp
        lifters/TestLLVMPredicateFolding.cpp
        lifters/TestLLVMMBA.cpp
        lifters/TestLLVMFunctionFlow.cpp
        lifters/TestLLVMDispatch.cpp
        lifters/TestLLVMGuestReturns.cpp
        lifters/TestLLVMImports.cpp
        lifters/TestLLVMStatic.cpp
        lifters/TestPassPipeline.cpp
        lifters/TestGuestMemoryPipeline.cpp
        lifters/TestLLVMControlFlow.cpp
        lifters/TestLLVMDispatchSpecialization.cpp
        lifters/TestLLVMDispatchExpressionBudget.cpp
        lifters/TestLLVMSameFunctionDeflattening.cpp
        lifters/TestLLVMConditionalState.cpp
        lifters/TestLLVMStateGuardEdges.cpp
        lifters/TestLLVMGuardedTransition.cpp
        lifters/TestLLVMStateContextCorridors.cpp
        lifters/TestLLVMLoopCarriedStateContexts.cpp
        lifters/TestLLVMFutureRegionStateContexts.cpp
        lifters/TestLLVMStoreStateContexts.cpp
        lifters/TestLLVMGuestMemoryHubs.cpp
        lifters/TestLLVMGuestMemoryHubPriority.cpp
        lifters/TestLLVMGuestLoopMembership.cpp
        lifters/TestLLVMGuestJoinOrder.cpp
        lifters/TestLLVMGuardedEntry.cpp
        lifters/TestLLVMEntryPrefixLiveOut.cpp
        lifters/TestLLVMEntryPrefix.cpp
        lifters/TestLLVMGuardedTransitions.cpp
        lifters/TestLLVMStaticRegion.cpp
        lifters/TestLLVMPreparedRegion.cpp
        lifters/TestLLVMLiftProvenance.cpp
        lifters/TestLLVMGuestMemorySSA.cpp
        lifters/TestLLVMGuestMemoryJoinEdges.cpp
        lifters/TestLLVMGuestIndexProofCache.cpp
        lifters/TestLLVMGuestAddressWidths.cpp
        lifters/TestLLVMGuestMemoryAlias.cpp
        lifters/TestLLVMGuestMemoryEdges.cpp
        lifters/TestLLVMGuestMemoryHistory.cpp
        lifters/TestLLVMGuestPointerPHI.cpp
        lifters/TestLLVMGuestPointerPHIPriority.cpp
        lifters/TestLLVMGuestEdgeWorklist.cpp
        lifters/TestLLVMCallStateBarriers.cpp
        lifters/TestLLVMReachingStore.cpp
        lifters/TestLLVMCrossCallIsolation.cpp
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
    target_sources(dragon_tests PRIVATE semantic/TestX86Strings.cpp semantic/TestIGNILOptimizer.cpp semantic/TestX86Addressing.cpp)
endif()

if(DRAGON_USE_UNICORN)
    set_property(SOURCE disassemblers/TestX86DecodeClosure.cpp APPEND
            PROPERTY COMPILE_DEFINITIONS DRAGON_WITH_UNICORN)
    set_property(SOURCE analysis/TestCallerContext.cpp APPEND
            PROPERTY COMPILE_DEFINITIONS DRAGON_WITH_UNICORN)
    set_property(SOURCE analysis/TestCallMemorySafety.cpp APPEND
            PROPERTY COMPILE_DEFINITIONS DRAGON_WITH_UNICORN)
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

# A normal ARM build can produce the expected bits despite signed C++ UB.
# Instrument the actual optimizer source in a small standalone regression
# target, so these failures stay visible without sanitizing LLVM/the library.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND NOT WIN32)
    include(CheckCXXSourceCompiles)
    include(CMakePushCheckState)
    cmake_push_check_state(RESET)
    set(CMAKE_REQUIRED_FLAGS "-fsanitize=undefined -fno-sanitize-recover=undefined")
    set(CMAKE_REQUIRED_LINK_OPTIONS -fsanitize=undefined -fno-sanitize-recover=undefined)
    check_cxx_source_compiles("int main(int argc, char**) { volatile int x = argc; return 1 / x; }"
            DRAGON_TEST_HAS_UNDEFINED_SANITIZER)
    cmake_pop_check_state()
    if(DRAGON_TEST_HAS_UNDEFINED_SANITIZER)
        add_executable(dragon_optimizer_ubsan semantic/IGNILOptimizerUBSan.cpp ../lib/IGNILSimplify.cpp)
        target_link_libraries(dragon_optimizer_ubsan PRIVATE dragon::libdragon)
        target_compile_options(dragon_optimizer_ubsan PRIVATE -fsanitize=undefined -fno-sanitize-recover=undefined)
        target_link_options(dragon_optimizer_ubsan PRIVATE -fsanitize=undefined -fno-sanitize-recover=undefined)
        foreach(case IN ITEMS neg div mod zero sweep)
            add_test(NAME "Validation - IGNIL sanitizer ${case}" COMMAND dragon_optimizer_ubsan ${case})
            set_tests_properties("Validation - IGNIL sanitizer ${case}" PROPERTIES TIMEOUT 60)
        endforeach()
        # Exercise the real memory-analysis translation unit under UBSan.
        # High modular Guest offsets are analyzed, never host-dereferenced.
        add_executable(dragon_memory_alias_ubsan semantic/LLVMGuestMemoryAliasUBSan.cpp ../lib/GuestMemorySSA.cpp)
        target_link_libraries(dragon_memory_alias_ubsan PRIVATE dragon::libdragon)
        target_compile_options(dragon_memory_alias_ubsan PRIVATE -fsanitize=undefined -fno-sanitize-recover=undefined)
        target_link_options(dragon_memory_alias_ubsan PRIVATE -fsanitize=undefined -fno-sanitize-recover=undefined)
        foreach(case IN ITEMS distance sweep)
            add_test(NAME "Validation - Guest memory alias sanitizer ${case}" COMMAND dragon_memory_alias_ubsan ${case})
            set_tests_properties("Validation - Guest memory alias sanitizer ${case}" PROPERTIES TIMEOUT 60)
        endforeach()
    endif()
endif()

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
add_executable(dragon_c_tests TestCAnalysisHints.cpp TestCInitialState.cpp TestCGuardedEntry.cpp)
target_link_libraries(dragon_c_tests PRIVATE libdragon_c Catch2::Catch2WithMain)
catch_discover_tests(dragon_c_tests)
# Python API tests. Optional like Unicorn: they need a Python 3 with cffi,
# and are simply not registered without one.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    execute_process(COMMAND ${Python3_EXECUTABLE} -c "import cffi"
                    RESULT_VARIABLE _dragon_cffi_missing OUTPUT_QUIET ERROR_QUIET)
    if(_dragon_cffi_missing EQUAL 0)
        add_test(NAME "Python API - guarded entry views"
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/python/test_guarded_entry.py)
        set_tests_properties("Python API - guarded entry views"
                PROPERTIES ENVIRONMENT "DRAGON_C_LIB=$<TARGET_FILE:libdragon_c>")
        add_test(NAME "Python API - entry prefix specialization"
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/python/test_entry_prefix.py)
        set_tests_properties("Python API - entry prefix specialization"
                PROPERTIES ENVIRONMENT "DRAGON_C_LIB=$<TARGET_FILE:libdragon_c>")
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
        add_test(NAME "Python API - conditional initial state analysis"
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/python/test_finite_initial_state.py)
        set_tests_properties("Python API - conditional initial state analysis"
                PROPERTIES ENVIRONMENT "DRAGON_C_LIB=$<TARGET_FILE:libdragon_c>")
        add_test(NAME "Validation - Legacy unlifted C and Python"
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/python/test_llvm_legacy_unlifted.py)
        set_tests_properties("Validation - Legacy unlifted C and Python"
                PROPERTIES ENVIRONMENT "DRAGON_C_LIB=$<TARGET_FILE:libdragon_c>")
        add_test(NAME "Validation - Legacy unlifted inventory controls"
                 COMMAND ${Python3_EXECUTABLE}
                         ${PROJECT_SOURCE_DIR}/scripts/validation/legacy_unlifted_inventory.py --self-test)
        add_test(NAME "Validation - table content Graph controls"
                 COMMAND ${Python3_EXECUTABLE}
                         ${PROJECT_SOURCE_DIR}/scripts/validation/table_content_diff.py --self-test)
        add_test(NAME "Validation - table content native controls"
                 COMMAND ${Python3_EXECUTABLE}
                         ${PROJECT_SOURCE_DIR}/scripts/validation/table_content_native_diff.py --self-test)
        add_test(NAME "Validation - Legacy unlifted oracle controls"
                 COMMAND ${Python3_EXECUTABLE}
                         ${PROJECT_SOURCE_DIR}/scripts/validation/legacy_unlifted_diff.py --self-test)
        add_test(NAME "Validation - system transition boundary controls"
                 COMMAND ${Python3_EXECUTABLE}
                         ${PROJECT_SOURCE_DIR}/scripts/validation/system_transition_diff.py --self-test)
        add_test(NAME "Validation - jump-table bounds follow the actual index"
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/python/test_table_bounds.py)
        add_test(NAME "Validation - static corpus summary rejects incomplete results"
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/python/test_static_corpus_summary.py)
        add_test(NAME "Validation - Tigress diagnostics and exact campaign coverage"
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/python/test_campaign_diagnostics.py)
        add_test(NAME "Validation - unreachable switch defaults require exact domain proofs"
                 COMMAND ${Python3_EXECUTABLE}
                         ${PROJECT_SOURCE_DIR}/scripts/validation/test_ir_unreachable.py)
        add_test(NAME "Validation - LLVM V2 C and Python mirror"
                 COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/python/test_llvm_v2.py)
        set_tests_properties("Validation - LLVM V2 C and Python mirror" PROPERTIES
            ENVIRONMENT "DRAGON_C_LIB=$<TARGET_FILE:libdragon_c>")
        add_test(NAME "Validation - LLVM static C and Python mirror"
                 COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/python/test_llvm_static.py)
        set_tests_properties("Validation - LLVM static C and Python mirror" PROPERTIES
            ENVIRONMENT "DRAGON_C_LIB=$<TARGET_FILE:libdragon_c>")
        add_test(NAME "Validation - LLVM static function entry C and Python"
                 COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/python/test_llvm_static_entry.py)
        set_tests_properties("Validation - LLVM static function entry C and Python" PROPERTIES
            ENVIRONMENT "DRAGON_C_LIB=$<TARGET_FILE:libdragon_c>")
        add_test(NAME "Validation - LLVM static regions C and Python"
                 COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/python/test_static_region.py)
        set_tests_properties("Validation - LLVM static regions C and Python" PROPERTIES
            ENVIRONMENT "DRAGON_C_LIB=$<TARGET_FILE:libdragon_c>")
        add_test(NAME "Validation - pass pipeline C and Python"
                 COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/python/test_pass_pipeline.py)
        set_tests_properties("Validation - pass pipeline C and Python" PROPERTIES
            ENVIRONMENT "DRAGON_C_LIB=$<TARGET_FILE:libdragon_c>")
        set(_dragon_predicate_args)
        if(NOT DRAGON_USE_Z3)
            list(APPEND _dragon_predicate_args --no-z3)
        endif()
        add_test(NAME "Validation - LLVM predicate folding C and Python"
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/python/test_llvm_predicates.py
                ${_dragon_predicate_args})
        set_tests_properties("Validation - LLVM predicate folding C and Python" PROPERTIES
            ENVIRONMENT "DRAGON_C_LIB=$<TARGET_FILE:libdragon_c>")
        add_test(NAME "Validation - LLVM MBA C and Python"
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/python/test_llvm_mba.py
                ${_dragon_predicate_args})
        set_tests_properties("Validation - LLVM MBA C and Python" PROPERTIES
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
                set(_dragon_predicate_native_args)
                if(NOT DRAGON_USE_Z3)
                    list(APPEND _dragon_predicate_native_args --expect-no-z3)
                endif()
                foreach(predicate_control IN ITEMS normal result memory)
                    set(_dragon_predicate_control_args)
                    if(NOT predicate_control STREQUAL "normal")
                        list(APPEND _dragon_predicate_control_args --control ${predicate_control} --expect-control)
                    endif()
                    add_test(NAME "Validation - LLVM predicates ${predicate_control}"
                        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/validation/predicate_fold_diff.py
                            --case even64 --case square64 --trials 2
                            --out ${CMAKE_CURRENT_BINARY_DIR}/predicate-${predicate_control}
                            ${_dragon_predicate_native_args} ${_dragon_predicate_control_args})
                    set_tests_properties("Validation - LLVM predicates ${predicate_control}" PROPERTIES
                        ENVIRONMENT "DRAGON_C_LIB=$<TARGET_FILE:libdragon_c>" TIMEOUT 180)
                endforeach()
                foreach(mba_control IN ITEMS normal result memory)
                    set(_dragon_mba_control_args)
                    if(NOT mba_control STREQUAL "normal")
                        list(APPEND _dragon_mba_control_args --control ${mba_control} --expect-control)
                    endif()
                    add_test(NAME "Validation - LLVM MBA ${mba_control}"
                        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/validation/mba_function_diff.py
                            --case a64-add32 --case x64-add64 --case a64-alias32 --case x64-alias32 --trials 2
                            --out ${CMAKE_CURRENT_BINARY_DIR}/mba-${mba_control}
                            ${_dragon_predicate_native_args} ${_dragon_mba_control_args})
                    set_tests_properties("Validation - LLVM MBA ${mba_control}" PROPERTIES
                        ENVIRONMENT "DRAGON_C_LIB=$<TARGET_FILE:libdragon_c>" TIMEOUT 180)
                endforeach()
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
                find_program(DRAGON_IMAGE_CLANG NAMES clang-21)
                find_program(DRAGON_IMAGE_LD NAMES ld.lld-21)
                execute_process(COMMAND ${Python3_EXECUTABLE} -c "import elftools"
                    RESULT_VARIABLE _dragon_image_elf_missing OUTPUT_QUIET ERROR_QUIET)
                if(DRAGON_IMAGE_CLANG AND DRAGON_IMAGE_LD AND _dragon_image_elf_missing EQUAL 0)
                    # Always validate the compiled-image harness and both oracle
                    # layers. Real Tigress campaigns are explicit standalone jobs;
                    # absence of that proprietary tool is never a semantic pass.
                    add_test(NAME "Validation - compiled C image semantics"
                        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/validation/tigress_function_diff.py
                            --profile plain --seeds 1 --trials 16
                            --clang ${DRAGON_IMAGE_CLANG} --ld ${DRAGON_IMAGE_LD}
                            --out ${CMAKE_CURRENT_BINARY_DIR}/compiled-c-semantics)
                    foreach(control c-result llvm-register llvm-memory)
                        add_test(NAME "Validation - compiled C detects ${control}"
                            COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/validation/tigress_function_diff.py
                                --profile plain --seeds 1 --trials 16 --control ${control} --expect-control
                                --clang ${DRAGON_IMAGE_CLANG} --ld ${DRAGON_IMAGE_LD}
                                --out ${CMAKE_CURRENT_BINARY_DIR}/compiled-c-negative-${control})
                    endforeach()
                    set_tests_properties("Validation - compiled C image semantics"
                        "Validation - compiled C detects c-result"
                        "Validation - compiled C detects llvm-register"
                        "Validation - compiled C detects llvm-memory" PROPERTIES
                        ENVIRONMENT "DRAGON_C_LIB=$<TARGET_FILE:libdragon_c>" TIMEOUT 180)
                    add_test(NAME "Validation - complete SHA256 semantics"
                        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/validation/tigress_sha256.py
                            --profile plain --c-level O0 --strategy 0 --quick
                            --clang ${DRAGON_IMAGE_CLANG} --ld ${DRAGON_IMAGE_LD}
                            --out ${CMAKE_CURRENT_BINARY_DIR}/sha256-semantics)
                    foreach(control c-digest llvm-register llvm-memory)
                        add_test(NAME "Validation - SHA256 detects ${control}"
                            COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/validation/tigress_sha256.py
                                --profile plain --c-level O0 --strategy 0 --quick --control ${control} --expect-control
                                --clang ${DRAGON_IMAGE_CLANG} --ld ${DRAGON_IMAGE_LD}
                                --out ${CMAKE_CURRENT_BINARY_DIR}/sha256-negative-${control})
                    endforeach()
                    set_tests_properties("Validation - complete SHA256 semantics"
                        "Validation - SHA256 detects c-digest"
                        "Validation - SHA256 detects llvm-register"
                        "Validation - SHA256 detects llvm-memory" PROPERTIES
                        ENVIRONMENT "DRAGON_C_LIB=$<TARGET_FILE:libdragon_c>" TIMEOUT 180)
                    add_test(NAME "Validation - external API images and boundaries"
                        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/validation/tigress_external_api.py
                            --profile plain --c-level O0 --strategy 0
                            --clang ${DRAGON_IMAGE_CLANG} --ld ${DRAGON_IMAGE_LD}
                            --out ${CMAKE_CURRENT_BINARY_DIR}/external-api-images)
                    foreach(control c-result stale-register stale-memory)
                        if(control STREQUAL "stale-memory")
                            set(_dragon_external_case write)
                        else()
                            set(_dragon_external_case return)
                        endif()
                        add_test(NAME "Validation - external APIs detect ${control}"
                            COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/validation/tigress_external_api.py
                                --profile plain --c-level O0 --strategy 0 --form got --trials 16
                                --case ${_dragon_external_case} --control ${control} --expect-control
                                --clang ${DRAGON_IMAGE_CLANG} --ld ${DRAGON_IMAGE_LD}
                                --out ${CMAKE_CURRENT_BINARY_DIR}/external-api-negative-${control})
                    endforeach()
                    set_tests_properties("Validation - external API images and boundaries"
                        "Validation - external APIs detect c-result"
                        "Validation - external APIs detect stale-register"
                        "Validation - external APIs detect stale-memory" PROPERTIES
                        ENVIRONMENT "DRAGON_C_LIB=$<TARGET_FILE:libdragon_c>" TIMEOUT 180)
                endif()
                add_test(NAME "Validation - complete Guest CALL and RET"
                    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/validation/function_guest_diff.py
                        --trials 2 --llc ${DRAGON_FLOW_LLC} --opt ${DRAGON_FLOW_OPT} --cc ${DRAGON_FLOW_CC}
                        --out ${CMAKE_CURRENT_BINARY_DIR}/function-guest-controls)
                set_tests_properties("Validation - complete Guest CALL and RET" PROPERTIES
                    ENVIRONMENT "DRAGON_C_LIB=$<TARGET_FILE:libdragon_c>" TIMEOUT 180)
                add_test(NAME "Validation - import API calls through Guest code"
                    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/validation/external_guest_diff.py
                        --trials 2 --llc ${DRAGON_FLOW_LLC} --opt ${DRAGON_FLOW_OPT} --cc ${DRAGON_FLOW_CC}
                        --out ${CMAKE_CURRENT_BINARY_DIR}/external-guest)
                add_test(NAME "Validation - import API static boundaries"
                    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/validation/external_guest_diff.py
                        --static --trials 2 --llc ${DRAGON_FLOW_LLC} --opt ${DRAGON_FLOW_OPT} --cc ${DRAGON_FLOW_CC}
                        --out ${CMAKE_CURRENT_BINARY_DIR}/external-guest-static)
                set_tests_properties("Validation - import API calls through Guest code"
                    "Validation - import API static boundaries" PROPERTIES
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
