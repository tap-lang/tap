cmake_minimum_required(VERSION 3.10)
project(tap C)

# Homebrew GCC uses its own fixed macOS headers; CMake's SDK sysroot mixes in
# incompatible CommandLineTools headers and breaks standard includes.
if(APPLE AND CMAKE_C_COMPILER_ID STREQUAL "GNU")
    set(CMAKE_OSX_SYSROOT "" CACHE PATH "macOS SDK sysroot" FORCE)
endif()

# 设置编译器
# set(CMAKE_C_COMPILER "clang")
# set(CMAKE_CXX_COMPILER "clang++")
message(STATUS "C_COMPILER: ${CMAKE_C_COMPILER}")
# message(STATUS "CXX_COMPILER: ${CMAKE_CXX_COMPILER}")

# 设置项目选项
option(BUILD_TESTS "Build tests" ON)
option(BUILD_EXAMPLES "Build examples" ON)
option(BUILD_SHARED_LIBS "Build shared libraries" OFF)

# Build both Runtime forms: native executables use the archive and lli loads the shared library.
add_library(tap_runtime STATIC runtime/src/runtime.c)
target_include_directories(tap_runtime PUBLIC runtime/include)
set_target_properties(tap_runtime PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(tap_runtime_shared SHARED runtime/src/runtime.c)
target_include_directories(tap_runtime_shared PUBLIC runtime/include)
target_compile_definitions(tap_runtime_shared PRIVATE TAP_RUNTIME_SHARED=1)
set_target_properties(tap_runtime_shared PROPERTIES
    OUTPUT_NAME tap_runtime
    ARCHIVE_OUTPUT_NAME tap_runtime_shared
)

# 设置C++标准
# set(CMAKE_CXX_STANDARD 17)
# set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 添加可执行文件
add_executable(get_version src/scripts/get_version.c)

add_executable(tap
    src/main.c
    src/helpers.c
    src/ast.c
    src/parser.c
    src/module.c
    src/generics.c
    src/prelude.c
    src/lexer.c
    src/token.c
    src/codegen.c
    src/run.c
    src/version.c
)
set_target_properties(tap PROPERTIES LINKER_LANGUAGE C)
add_dependencies(tap tap_runtime tap_runtime_shared)

# 查找LLVM 限制版本大于21
find_package(LLVM REQUIRED CONFIG)
if(LLVM_PACKAGE_VERSION VERSION_LESS "21")
    message(FATAL_ERROR "LLVM version must be greater than 21")
endif()
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

# 获取 LLVM 相关信息
message(STATUS "LLVM Include Dir: ${LLVM_INCLUDE_DIRS}")
message(STATUS "LLVM Library Dir: ${LLVM_LIBRARY_DIRS}")
message(STATUS "LLVM Definitions: ${LLVM_DEFINITIONS}")


# 获取LLVM库列表
set(llvm_libs LLVM)

include_directories(${LLVM_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})

# 生成版本文件
add_custom_command(
    OUTPUT ${CMAKE_SOURCE_DIR}/src/version.h
    COMMAND $<TARGET_FILE:get_version>
            ${CMAKE_SOURCE_DIR}/src/version.h.ini
            ${CMAKE_SOURCE_DIR}/src/version.h
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    DEPENDS get_version ${CMAKE_SOURCE_DIR}/src/version.h.ini
    COMMENT "Generating version.h file using get_version"
)
add_custom_target(generate_version DEPENDS ${CMAKE_SOURCE_DIR}/src/version.h)

# 确保版本文件在构建前生成
add_dependencies(tap generate_version)

# 链接LLVM库
if(WIN32)
    target_link_libraries(tap ${llvm_libs})
    if(MSVC)
        target_compile_definitions(tap PRIVATE strdup=_strdup)
    endif()
else()
    target_link_libraries(tap ${llvm_libs} m)
endif()

if(BUILD_TESTS)
    enable_testing()
    add_test(
        NAME language-tests
        COMMAND sh ${CMAKE_SOURCE_DIR}/tests/run-tests.sh $<TARGET_FILE:tap>
    )
    set_tests_properties(language-tests PROPERTIES
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
endif()

# 安装目标
install(TARGETS tap DESTINATION bin)
install(TARGETS tap_runtime ARCHIVE DESTINATION lib)
install(TARGETS tap_runtime_shared
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)
install(DIRECTORY runtime/include/ DESTINATION include/tap)
install(DIRECTORY std/ DESTINATION share/tap/std FILES_MATCHING PATTERN "*.tp")
