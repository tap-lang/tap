# Makefile for 4yue Lang compiler

# 编译器和编译选项
CC = clang
# CXX = clang++
CFLAGS = -Wall -Wextra -g -fsanitize=address -fno-omit-frame-pointer -I/opt/homebrew/opt/llvm/include
CXXFLAGS = $(CFLAGS)
LDFLAGS = -L/opt/homebrew/opt/llvm/lib -Wl,-rpath,/opt/homebrew/opt/llvm/lib -fsanitize=address
LIBS = -lLLVM-21

# 源文件目录
SRC_DIR = src
BUILD_DIR = build

# 查找所有源文件
C_SOURCES = $(wildcard $(SRC_DIR)/*.c)
CPP_SOURCES = $(wildcard $(SRC_DIR)/*.cpp)
SOURCES = $(C_SOURCES) $(CPP_SOURCES)

# 生成目标文件
OBJECTS = $(patsubst $(SRC_DIR)/%.c, $(BUILD_DIR)/%.o, $(C_SOURCES)) $(patsubst $(SRC_DIR)/%.cpp, $(BUILD_DIR)/%.o, $(CPP_SOURCES))

# 目标可执行文件
TARGET = 4yue

# 默认目标
all: $(TARGET)

# 创建build目录
$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# 编译C源文件
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# 编译C++源文件（因为LLVM主要是C++接口）
# $(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp | $(BUILD_DIR)
#	$(CXX) $(CXXFLAGS) -c $< -o $@

# 链接目标文件
$(TARGET): $(OBJECTS)
#	$(CXX) $(CXXFLAGS) -o $(TARGET) $(OBJECTS) $(LDFLAGS) $(LIBS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJECTS) $(LDFLAGS) $(LIBS)

# 清理生成的文件
clean:
	rm -rf $(BUILD_DIR) $(TARGET)
	rm -rf output output.ll

# 运行测试
test: $(TARGET)
	./$(TARGET) tests/1.ta
	./output

test_hello: $(TARGET)
	./$(TARGET) tests/hello.ta -o hello
	./hello

.PHONY: all clean run