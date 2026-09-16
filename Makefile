# Makefile for Tap Lang compiler

# 编译器和编译选项
# CC = clang
CC = gcc
LIBS = -lLLVM-21 -lm

# Sanitizers are opt-in because their VM reservation conflicts with macOS nano malloc.
SANITIZE ?= 0
ifeq ($(SANITIZE),1)
    SANITIZER_FLAGS = -fsanitize=address -fno-omit-frame-pointer
endif

RUNTIME_INCLUDE = -Iruntime/include
RUNTIME_CFLAGS = -Wall -Wextra -g $(RUNTIME_INCLUDE) -fPIC
RUNTIME_OBJECT = $(BUILD_DIR)/runtime.o
RUNTIME_STATIC = $(BUILD_DIR)/libtap_runtime.a
VERSION_GENERATOR = $(BUILD_DIR)/get_version

# 根据操作系统类型设置不同的CFLAGS和LDFLAGS
ifeq ($(OS),Windows_NT)
    # Windows系统设置
#     CFLAGS = -Wall -Wextra -g -I"C:/Program Files/LLVM/include"
    CFLAGS = -Wall -Wextra -g
    # CXXFLAGS = $(CFLAGS)
#     LDFLAGS = -L"C:/Program Files/LLVM/lib"
    LDFLAGS = 
    RUNTIME_SHARED = $(BUILD_DIR)/tap_runtime.dll
    RUNTIME_SHARED_FLAGS = -shared
    # Windows下不使用address sanitizer
else
    # 非Windows系统
    UNAME_S := $(shell uname -s)
    ifeq ($(UNAME_S),Linux)
        # Linux系统设置
        CFLAGS = -Wall -Wextra -g $(SANITIZER_FLAGS) -I/usr/lib/llvm-21/include
        # CXXFLAGS = $(CFLAGS)
        LDFLAGS = -L/usr/lib/llvm-21/lib -Wl,-rpath,/usr/lib $(SANITIZER_FLAGS)
        RUNTIME_SHARED = $(BUILD_DIR)/libtap_runtime.so
        RUNTIME_SHARED_FLAGS = -shared
    else ifeq ($(UNAME_S),Darwin)
        # macOS系统设置
        CFLAGS = -Wall -Wextra -g $(SANITIZER_FLAGS) -I/opt/homebrew/opt/llvm/include
        # CXXFLAGS = $(CFLAGS)
        LDFLAGS = -L/opt/homebrew/opt/llvm/lib -Wl,-rpath,/opt/homebrew/opt/llvm/lib $(SANITIZER_FLAGS)
        RUNTIME_SHARED = $(BUILD_DIR)/libtap_runtime.dylib
        RUNTIME_SHARED_FLAGS = -dynamiclib
    else
        # 其他系统，使用默认设置
        CFLAGS = -Wall -Wextra -g $(SANITIZER_FLAGS)
        # CXXFLAGS = $(CFLAGS)
        LDFLAGS = $(SANITIZER_FLAGS)
        RUNTIME_SHARED = $(BUILD_DIR)/libtap_runtime.so
        RUNTIME_SHARED_FLAGS = -shared
    endif
endif

# 可执行文件扩展名（Windows 下为 .exe）
ifeq ($(OS),Windows_NT)
    EXE = .exe
    VERSION_GENERATOR := $(VERSION_GENERATOR).exe
else
    EXE =
endif

# 源文件目录
SRC_DIR = src
BUILD_DIR = build

# 查找所有源文件
C_SOURCES = $(wildcard $(SRC_DIR)/*.c)
# CPP_SOURCES = $(wildcard $(SRC_DIR)/*.cpp)
SOURCES = $(C_SOURCES) $(CPP_SOURCES)

# 生成目标文件
# OBJECTS = $(patsubst $(SRC_DIR)/%.c, $(BUILD_DIR)/%.o, $(C_SOURCES)) $(patsubst $(SRC_DIR)/%.cpp, $(BUILD_DIR)/%.o, $(CPP_SOURCES))
OBJECTS = $(patsubst $(SRC_DIR)/%.c, $(BUILD_DIR)/%.o, $(C_SOURCES))
DEPS = $(OBJECTS:.o=.d)

# 目标可执行文件
TARGET = ./build/tap

# 默认目标
all: $(TARGET)

# 创建build目录
$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# 编译C源文件
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c $(SRC_DIR)/version.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

# Runtime objects are position independent so the same object can back both library forms.
$(RUNTIME_OBJECT): runtime/src/runtime.c runtime/include/tap_runtime.h | $(BUILD_DIR)
	$(CC) $(RUNTIME_CFLAGS) -c $< -o $@

# The generated native program links this archive only when Runtime symbols are referenced.
$(RUNTIME_STATIC): $(RUNTIME_OBJECT)
	$(AR) rcs $@ $<

# lli loads this library to resolve extern Runtime declarations.
$(RUNTIME_SHARED): $(RUNTIME_OBJECT)
	$(CC) $(RUNTIME_SHARED_FLAGS) -o $@ $<

-include $(DEPS)

# 编译C++源文件
# $(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp | $(BUILD_DIR)
#	$(CXX) $(CXXFLAGS) -c $< -o $@

# 编译版本头文件生成器
$(VERSION_GENERATOR): $(SRC_DIR)/scripts/get_version.c | $(BUILD_DIR)
	$(CC) -Wall -Wextra -g $< -o $@

# 生成版本头文件
$(SRC_DIR)/version.h: $(SRC_DIR)/version.h.ini $(VERSION_GENERATOR)
	$(VERSION_GENERATOR) $(SRC_DIR)/version.h.ini $(SRC_DIR)/version.h

# 链接目标文件
$(TARGET): $(OBJECTS) $(RUNTIME_STATIC) $(RUNTIME_SHARED)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJECTS) $(LDFLAGS) $(LIBS)

# 清理生成的文件
clean:
	rm -rf $(BUILD_DIR) $(TARGET) $(SRC_DIR)/version.h
	rm -rf output *.ll hello *.exe tests/*.exe *.dSYM

# 运行测试
test: $(TARGET) $(RUNTIME_STATIC) $(RUNTIME_SHARED)
	sh tests/run-tests.sh $(TARGET)

test_hello: $(TARGET)
	$(TARGET) tests/run-pass/basics/hello.tp -o ./build/hello
	./build/hello

.PHONY: all clean test test_hello
