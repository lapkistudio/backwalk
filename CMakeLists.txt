cmake_minimum_required(VERSION 3.20)
project(backwalk C CXX)
if(NOT WIN32)
    enable_language(ASM)
endif()

enable_testing()

if(WIN32)
    if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
        add_compile_options(/W4 /WX /Zc:preprocessor)
    elseif(CMAKE_C_COMPILER_ID STREQUAL "Clang" AND CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
        add_compile_options(/W4 /WX /clang:-Wall /clang:-Wextra /clang:-Wpedantic)
    else()
        add_compile_options(-Wall -Wextra -Wpedantic -Werror)
    endif()
else()
    add_compile_options(
        -fno-omit-frame-pointer
        -Wall
        -Wextra
        -Wpedantic
        -Werror
    )
endif()

option(BW_ENABLE_UBSAN "Enable Undefined Behavior Sanitizer" OFF)
option(BW_ENABLE_ASAN "Enable Address Sanitizer" OFF)
option(BW_ENABLE_MSAN "Enable Memory Sanitizer" OFF)
option(BW_ENABLE_TSAN "Enable Thread Sanitizer" OFF)

function(enable_sanitizer COMPILER_OPTION SANITIZER_NAME)
    add_compile_options(-fsanitize=${COMPILER_OPTION} -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=${COMPILER_OPTION})
    message("backwalk sanitizer: ${SANITIZER_NAME} enabled")
endfunction()

if(BW_ENABLE_ASAN)
    add_compile_options(
        $<$<C_COMPILER_ID:MSVC>:/fsanitize=address>
        $<$<C_COMPILER_ID:Clang>:-fsanitize=address,undefined -fno-omit-frame-pointer -g>
        $<$<C_COMPILER_ID:AppleClang>:-fsanitize=address,undefined -fno-omit-frame-pointer -g>
        $<$<C_COMPILER_ID:GNU>:-fsanitize=address -fno-omit-frame-pointer -g>
    )
    add_link_options(
        $<$<C_COMPILER_ID:MSVC>:/fsanitize=address>
        $<$<C_COMPILER_ID:Clang>:-fsanitize=address,undefined>
        $<$<C_COMPILER_ID:AppleClang>:-fsanitize=address,undefined>
        $<$<C_COMPILER_ID:GNU>:-fsanitize=address>
    )
    message("backwalk sanitizer: ASan enabled")
elseif(BW_ENABLE_UBSAN)
    enable_sanitizer(undefined UBSan)
elseif(BW_ENABLE_MSAN)
    enable_sanitizer(memory MSan)
elseif(BW_ENABLE_TSAN)
    enable_sanitizer(thread TSan)
else()
    message("backwalk sanitizer: No sanitizers enabled")
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Directories
set(BACKWALK_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src)
set(BACKWALK_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include)
set(BACKWALK_TEST_DIR ${CMAKE_CURRENT_SOURCE_DIR}/test)

# Project version
file(READ VERSION BACKWALK_PROJECT_VERSION)
string(STRIP ${BACKWALK_PROJECT_VERSION} BACKWALK_PROJECT_VERSION)

# and commit
execute_process(COMMAND git describe --always --dirty
    OUTPUT_VARIABLE BACKWALK_GIT_COMMIT_HASH
    ERROR_VARIABLE BACKWALK_GIT_COMMIT_ERROR
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE BACKWALK_GIT_COMMIT_RESULT
)
if (NOT ${BACKWALK_GIT_COMMIT_RESULT} STREQUAL 0)
    message(WARNING "Could not retrieve git commit: " ${BACKWALK_GIT_COMMIT_ERROR})
    set(BACKWALK_GIT_COMMIT_HASH unknown)
endif()

message("backwalk version information: " ${BACKWALK_PROJECT_VERSION} " - " ${BACKWALK_GIT_COMMIT_HASH})
configure_file(${BACKWALK_SRC_DIR}/version.c.in ${CMAKE_CURRENT_BINARY_DIR}/gen/version.c @ONLY)

# Source files
set(BACKWALK_SRC_LIST
    ${BACKWALK_SRC_DIR}/backwalk.c
    ${BACKWALK_SRC_DIR}/debug.c
    ${CMAKE_CURRENT_BINARY_DIR}/gen/version.c
)
if(WIN32)
    list(APPEND BACKWALK_SRC_LIST ${BACKWALK_SRC_DIR}/win.c)
else()
    list(APPEND BACKWALK_SRC_LIST
        ${BACKWALK_SRC_DIR}/common.c
        ${BACKWALK_SRC_DIR}/context.c
        ${BACKWALK_SRC_DIR}/asm/context_aarch64.S
        ${BACKWALK_SRC_DIR}/asm/context_arm.S
        ${BACKWALK_SRC_DIR}/asm/context_x64.S
    )
endif()

file(GLOB_RECURSE ASM_FILES "${BACKWALK_SRC_DIR}/asm/*")
if (BW_IWYU)
    list(REMOVE_ITEM BACKWALK_SRC_LIST ${ASM_FILES})
endif()


add_library(backwalk ${BACKWALK_SRC_LIST})
target_include_directories(backwalk PUBLIC ${BACKWALK_INCLUDE_DIR})
target_compile_features(backwalk PUBLIC c_std_99)
if(WIN32)
    target_link_libraries(backwalk PUBLIC dbghelp)
else()
    target_link_libraries(backwalk PUBLIC dl)
endif()
if (BW_DEBUG_ENABLED)
    target_compile_definitions(backwalk PRIVATE BW_DEBUG_ENABLED)
endif()

install(TARGETS backwalk ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY ${BACKWALK_INCLUDE_DIR}/ DESTINATION include)

function(bw_test TEST_NAME)
    file(GLOB TEST_FILE "${BACKWALK_TEST_DIR}/${TEST_NAME}.c*")
    add_executable(${TEST_NAME} ${TEST_FILE})
    if (BW_DEBUG_ENABLED)
        target_compile_definitions(${TEST_NAME} PRIVATE BW_DEBUG_ENABLED)
    endif()
    target_include_directories(${TEST_NAME} PRIVATE ${BACKWALK_SRC_DIR} ${BACKWALK_TEST_DIR})
    target_link_libraries(${TEST_NAME} PRIVATE backwalk)
    if(WIN32)
        if(MSVC)
            target_link_options(${TEST_NAME} PRIVATE /DEBUG)
        else()
            target_link_options(${TEST_NAME} PRIVATE "LINKER:/DEBUG")
        endif()
    else()
        target_link_options(${TEST_NAME} PRIVATE -rdynamic)
    endif()
    add_test(${TEST_NAME} ${TEST_NAME})
endfunction()

function(bw_disable_sibling_calls TARGET_NAME)
    if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
        # cl has no -fno-optimize-sibling-calls; /Od keeps the tested frames.
        target_compile_options(${TARGET_NAME} PRIVATE /Od)
    elseif(CMAKE_C_COMPILER_ID STREQUAL "Clang" AND CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
        target_compile_options(${TARGET_NAME} PRIVATE /clang:-fno-optimize-sibling-calls)
    else()
        target_compile_options(${TARGET_NAME} BEFORE PRIVATE -fno-optimize-sibling-calls)
    endif()
endfunction()

bw_test(backtrace_test)
bw_test(edge_cases_test)
bw_test(stress_test)
bw_test(version_test)
bw_disable_sibling_calls(backtrace_test)
bw_disable_sibling_calls(edge_cases_test)

bw_test(cpp_test)
bw_disable_sibling_calls(cpp_test)

bw_test(threading_test)
if(NOT WIN32)
    target_link_libraries(threading_test PRIVATE pthread)
endif()

if(NOT WIN32)
    bw_test(signal_test)
    bw_disable_sibling_calls(signal_test)
    target_link_libraries(signal_test PRIVATE pthread)
endif()

file(GLOB_RECURSE 
    HDR_FILES
    "${BACKWALK_SRC_DIR}/*.h"
    "${BACKWALK_INCLUDE_DIR}/*.h"
    "${BACKWALK_TEST_DIR}/*.h"
)
add_custom_target(lint
  COMMAND /usr/bin/clang-tidy -p "${CMAKE_BINARY_DIR}"  ${HDR_FILES}
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tools/run-clang-tidy.py -allow-no-checks -p=${CMAKE_BINARY_DIR}
  COMMENT "Running clang-tidy..."
)

add_custom_target(fmt
  COMMAND git clang-format --diff
  COMMENT "Running clang-format..."
)

add_custom_target(iwyu
  COMMAND iwyu_tool -p ${CMAKE_BINARY_DIR} --
  -Xiwyu --check_also=${BACKWALK_SRC_DIR}/*.h
  -Xiwyu --check_also=${BACKWALK_TEST_DIR}/*.h
  -Xiwyu --update_comments
  -Xiwyu --error=1
  COMMENT "Running iwyu..."
)

add_custom_target(iwyu-fix
  COMMAND iwyu_tool -p ${CMAKE_BINARY_DIR} --
  -Xiwyu --check_also=${BACKWALK_SRC_DIR}/*.h
  -Xiwyu --check_also=${BACKWALK_TEST_DIR}/*.h
  -Xiwyu --update_comments
  | fix_include --update_comments --nosafe_headers --noreorder
  COMMENT "Running iwyu and applying suggestions..."
)
