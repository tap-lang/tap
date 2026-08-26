# Makefile for 4yue Lang compiler

# 编译器和编译选项
# CC = clang
CC = gcc
# CXX = clang++
LIBS = -lLLVM-21 -lm

# 根据操作系统类型设置不同的CFLAGS和LDFLAGS
ifeq ($(OS),Windows_NT)
    # Windows系统设置
#     CFLAGS = -Wall -Wextra -g -I"C:/Program Files/LLVM/include"
    CFLAGS = -Wall -Wextra -g
    # CXXFLAGS = $(CFLAGS)
#     LDFLAGS = -L"C:/Program Files/LLVM/lib"
    LDFLAGS = 
    # Windows下不使用address sanitizer
else
    # 非Windows系统
    UNAME_S := $(shell uname -s)
    ifeq ($(UNAME_S),Linux)
        # Linux系统设置
        CFLAGS = -Wall -Wextra -g -fsanitize=address -fno-omit-frame-pointer -I/usr/lib/llvm-21/include
        # CXXFLAGS = $(CFLAGS)
        LDFLAGS = -L/usr/lib/llvm-21/lib -Wl,-rpath,/usr/lib -fsanitize=address
    else ifeq ($(UNAME_S),Darwin)
        # macOS系统设置
        CC_IS_CLANG := $(findstring clang,$(shell $(CC) --version 2>/dev/null))
        ifneq ($(CC_IS_CLANG),)
            SANITIZER_FLAGS = -fsanitize=address -fno-omit-frame-pointer
        endif
        CFLAGS = -Wall -Wextra -g $(SANITIZER_FLAGS) -I/opt/homebrew/opt/llvm/include
        # CXXFLAGS = $(CFLAGS)
        LDFLAGS = -L/opt/homebrew/opt/llvm/lib -Wl,-rpath,/opt/homebrew/opt/llvm/lib $(SANITIZER_FLAGS)
    else
        # 其他系统，使用默认设置
        CFLAGS = -Wall -Wextra -g -fsanitize=address -fno-omit-frame-pointer
        # CXXFLAGS = $(CFLAGS)
        LDFLAGS = -fsanitize=address
    endif
endif

# 可执行文件扩展名（Windows 下为 .exe）
ifeq ($(OS),Windows_NT)
    EXE = .exe
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
TARGET = ./build/4yue

# 默认目标
all: $(TARGET)

# 创建build目录
$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# 编译C源文件
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c $(SRC_DIR)/version.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

-include $(DEPS)

# 编译C++源文件
# $(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp | $(BUILD_DIR)
#	$(CXX) $(CXXFLAGS) -c $< -o $@

# 判断是否为Windows系统
ifeq ($(OS),Windows_NT)
$(SRC_DIR)/version.h: $(SRC_DIR)/version.h.ini $(SRC_DIR)/scripts/version.bat
	$(SRC_DIR)/scripts/version.bat
else
$(SRC_DIR)/version.h: $(SRC_DIR)/version.h.ini $(SRC_DIR)/scripts/version.sh
	sh $(SRC_DIR)/scripts/version.sh
endif

# 链接目标文件
$(TARGET): $(OBJECTS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJECTS) $(LDFLAGS) $(LIBS)
#	$(CXX) $(CXXFLAGS) -o $(TARGET) $(OBJECTS) $(LDFLAGS) $(LIBS)

# 清理生成的文件
clean:
	rm -rf $(BUILD_DIR) $(TARGET) $(SRC_DIR)/version.h
	rm -rf output *.ll hello *.exe tests/*.exe *.dSYM

# 运行测试
test: $(TARGET)
	sh tests/run-tests.sh $(TARGET)

test_hello: $(TARGET)
	$(TARGET) tests/run-pass/basics/hello.tp -o ./build/hello
	./build/hello

.PHONY: all clean test test_hello
