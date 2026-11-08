cmake_minimum_required(VERSION 3.20)
project(luamura LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

find_package(LLVM REQUIRED CONFIG)
llvm_map_components_to_libnames(LLVM_LIBS core support passes)

# Lua, statically: compiled code uses its internal symbols.
file(GLOB LUA_SOURCES CONFIGURE_DEPENDS src/*.c)
list(REMOVE_ITEM LUA_SOURCES
    ${PROJECT_SOURCE_DIR}/src/lua.c ${PROJECT_SOURCE_DIR}/src/luac.c)
add_library(liblua STATIC ${LUA_SOURCES})
set_target_properties(liblua PROPERTIES OUTPUT_NAME lua C_STANDARD 99)
target_include_directories(liblua PUBLIC src)
target_compile_definitions(liblua PUBLIC
    $<$<PLATFORM_ID:Linux>:LUA_USE_LINUX>)
target_link_libraries(liblua PUBLIC m ${CMAKE_DL_LIBS})

# The stock interpreter, built from the same tree: the tests' reference.
add_executable(lua src/lua.c)
target_link_libraries(lua PRIVATE liblua)
set_target_properties(lua PROPERTIES ENABLE_EXPORTS ON)

add_executable(luamura
    src/luamura/compiler/main.cpp src/luamura/compiler/emit.cpp
    src/luamura/compiler/ssa.cpp)
target_include_directories(luamura PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_definitions(luamura PRIVATE ${LLVM_DEFINITIONS})
target_link_libraries(luamura PRIVATE ${LLVM_LIBS} liblua)

add_library(luamura_rt STATIC
    src/luamura/runtime/main.c src/luamura/runtime/ops.c)
set_target_properties(luamura_rt PROPERTIES C_STANDARD 11)
target_link_libraries(luamura_rt PUBLIC liblua)

enable_testing()

list(TRANSFORM CMAKE_DL_LIBS PREPEND -l OUTPUT_VARIABLE dl_flags)

# Upstream files check themselves and print timings and random seeds, so
# only their exit status is compared. all.lua drives the others, main.lua
# tests lua.c's command line and heavy.lua exhausts memory.
set(compare_internal output)
set(compare_lua-test status)
foreach(suite internal lua-test)
    file(GLOB tests CONFIGURE_DEPENDS
        ${PROJECT_SOURCE_DIR}/tests/${suite}/*.lua)
    list(FILTER tests EXCLUDE REGEX "/lua-test/(all|main|heavy)\\.lua$")
    foreach(test ${tests})
        get_filename_component(name ${test} NAME_WE)
        add_test(NAME ${suite}_${name}
            COMMAND ${PROJECT_SOURCE_DIR}/tests/run_case.sh
                $<TARGET_FILE:luamura> $<TARGET_FILE:lua> ${test}
                $<TARGET_FILE:luamura_rt> $<TARGET_FILE:liblua>
                -Wl,-E ${dl_flags})
        set_tests_properties(${suite}_${name} PROPERTIES
            LABELS "e2e;${suite}"
            ENVIRONMENT "LUAMURA_COMPARE=${compare_${suite}}"
            TIMEOUT 30)
    endforeach()
endforeach()
