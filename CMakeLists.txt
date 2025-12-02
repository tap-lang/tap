cmake_minimum_required(VERSION 3.10)
project(4yue)

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


# 添加LLVM相关设置
include_directories(${LLVM_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})

# 获取LLVM库列表
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

# 链接LLVM库
target_link_libraries(4yue ${llvm_libs})

# 安装目标
install(TARGETS 4yue DESTINATION bin)
