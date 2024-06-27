# 编译器和编译选项
CC=gcc
CFLAGS=-Wall -Wextra -pedantic
LDFLAGS=

# 源文件目录和目标文件目录
SRC_DIR=src
OBJ_DIR=obj

# 获取所有的源文件
SRCS=$(wildcard $(SRC_DIR)/*.c)

# 从源文件生成对象文件列表
OBJS=$(SRCS:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)

# 最终的可执行文件名
TARGET=4yue

# 默认目标：构建可执行文件
all: $(TARGET)

# 编译单个对象文件
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	$(CC) $(CFLAGS) -c $< -o $@

# 链接对象文件生成可执行文件
$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) $^ -o $@

# 清理生成的文件
clean:
	rm -f $(OBJ_DIR)/*.o $(TARGET)

run: 4yue
	./4yue

# 创建对象文件目录（如果不存在）
$(shell mkdir -p $(OBJ_DIR))