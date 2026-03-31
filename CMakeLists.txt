cmake_minimum_required(VERSION 3.10)
project(4yue)

# 设置编译器
# set(CMAKE_CXX_COMPILER "clang++")
message(STATUS "C_COMPILER: ${CMAKE_C_COMPILER}")
message(STATUS "CXX_COMPILER: ${CMAKE_CXX_COMPILER}")

# 设置项目选项
option(BUILD_TESTS "Build tests" ON)
option(BUILD_EXAMPLES "Build examples" ON)
option(BUILD_SHARED_LIBS "Build shared libraries" OFF)

# 设置C++标准
# set(CMAKE_CXX_STANDARD 17)
# set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 添加可执行文件
add_executable(4yue
    src/main.c   
    src/ast.c   
    src/parser.c   
    src/lexer.c   
    src/codegen.c
)

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
if(EXISTS "/etc/arch-release") 
    # on Archlinux
    set(llvm_libs LLVM)
else()
    llvm_map_components_to_libnames(llvm_libs
        Core
        ExecutionEngine
        MC
        MCJIT
        Object
        Support
        Target
        TargetParser
        Analysis
        BitWriter
        IRReader
        AArch64AsmParser
        AArch64CodeGen
        AArch64Desc
        AArch64Info
        AArch64Utils
    )
endif()

include_directories(${LLVM_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})

# 生成版本文件
if(WIN32)
    add_custom_target(generate_version
        COMMAND ${CMAKE_SOURCE_DIR}/src/scripts/version.bat
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        DEPENDS ${CMAKE_SOURCE_DIR}/src/version.h.ini
        COMMENT "Generating version.h file using version.bat"
    )
else()
    add_custom_target(generate_version
        COMMAND ${CMAKE_SOURCE_DIR}/src/scripts/version.sh
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        DEPENDS ${CMAKE_SOURCE_DIR}/src/version.h.ini
        COMMENT "Generating version.h file using version.sh"
    )
endif()

# 确保版本文件在构建前生成
add_dependencies(4yue generate_version)

# 链接LLVM库
#target_link_libraries(4yue ${llvm_libs})
target_link_libraries(4yue ${llvm_libs} m)

# 安装目标
install(TARGETS 4yue DESTINATION bin)
