# TF Physics, an SFSE plugin for Starfield. Windows x64 with MSVC (Visual Studio 2022 or newer). No dependencies.
#   cmake -S . -B build/cmake -A x64
#   cmake --build build/cmake --config Release
#   ctest --test-dir build/cmake -C Release --output-on-failure
# The .bat scripts in the root and in tests\ do the same without CMake.
cmake_minimum_required(VERSION 3.21)
project(TFPhysics LANGUAGES CXX)

if(NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "TF Physics builds with MSVC for x64 only (Starfield is a 64-bit Windows game)")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")   # /MT, as the .bat builds

# One DLL per build variant, each named TFPhysics.dll in its own folder under the build tree
function(tfp_plugin target outdir)
    add_library(${target} SHARED src/plugin.cpp)
    target_compile_options(${target} PRIVATE /W4 /EHsc)
    target_compile_definitions(${target} PRIVATE ${ARGN})
    target_link_libraries(${target} PRIVATE shell32 ole32)
    set_target_properties(${target} PROPERTIES OUTPUT_NAME TFPhysics
                          RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/${outdir}/$<0:>")
endfunction()

# (folder names that can't meet the build configurations' own Debug/Release folders: Windows ignores case)
tfp_plugin(TFPhysics dll_logging)                                                            # the logging build
tfp_plugin(TFPhysics_release dll_release TFP_RELEASE)                                        # gravity, springs, body shape
tfp_plugin(TFPhysics_release_collisions dll_release_col TFP_RELEASE TFP_RELEASE_COLLISIONS)  # what A-Body 1.5 ships

option(TFP_TESTS "Build the offline tests" ON)
if(TFP_TESTS)
    enable_testing()
    # The tests find tests\cases and their INI from where the exe sits, so they go to <source>\build\tests as with
    # tests\run_tests.bat.
    set(tfp_test_dir "${CMAKE_SOURCE_DIR}/build/tests")
    add_custom_target(tfp_test_ini ALL
        COMMAND ${CMAKE_COMMAND} -E make_directory "${tfp_test_dir}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/config/TFPhysics.ini" "${tfp_test_dir}/TFPhysics.ini")
    foreach(t collide_test gravity_test shape_test dent_test addrlib_test stress_test)
        add_executable(${t} tests/${t}.cpp)
        target_compile_options(${t} PRIVATE /W4 /EHsc)
        target_link_libraries(${t} PRIVATE shell32 ole32)
        set_target_properties(${t} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${tfp_test_dir}/$<0:>")
        add_dependencies(${t} tfp_test_ini)
        add_test(NAME ${t} COMMAND ${t} WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    endforeach()
    # how a TFPhysics.ini and its tuning files are read (run here on the shipped INI)
    add_executable(tune_dump tests/tune_dump.cpp)
    target_compile_options(tune_dump PRIVATE /W4 /EHsc)
    target_link_libraries(tune_dump PRIVATE shell32 ole32)
    set_target_properties(tune_dump PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${tfp_test_dir}/$<0:>")
    add_test(NAME tune_dump COMMAND tune_dump config/TFPhysics.ini WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
endif()
