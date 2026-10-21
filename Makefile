# Makefile for Tap Lang compiler

# 编译器和编译选项
# CC = clang
CC ?= gcc
LIBS = -lLLVM-21 -lm

# Sanitizers are opt-in because their VM reservation conflicts with macOS nano malloc.
SANITIZE ?= 0
TEST_JOBS ?= 4
ifeq ($(SANITIZE),1)
    SANITIZER_FLAGS = -fsanitize=address -fno-omit-frame-pointer
endif

RUNTIME_INCLUDE = -Iruntime/include
RUNTIME_CFLAGS = -Wall -Wextra -g $(RUNTIME_INCLUDE) -fPIC
RUNTIME_OBJECT = $(BUILD_DIR)/runtime.o
RUNTIME_STATIC = $(BUILD_DIR)/libtap_runtime.a
# Windows 的 winsock 符号在 ws2_32 里，共享 Runtime 需要显式链接。
RUNTIME_SHARED_LIBS =
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
    RUNTIME_SHARED_LIBS = -lws2_32
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
TEST_RUNNER = $(BUILD_DIR)/run-tests$(EXE)

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
	$(CC) $(RUNTIME_SHARED_FLAGS) -o $@ $< $(RUNTIME_SHARED_LIBS)

-include $(DEPS)

# 编译C++源文件
# $(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp | $(BUILD_DIR)
#	$(CXX) $(CXXFLAGS) -c $< -o $@

# 编译版本头文件生成器
$(VERSION_GENERATOR): $(SRC_DIR)/scripts/get_version.c | $(BUILD_DIR)
	$(CC) -Wall -Wextra -g $< -o $@

# C 版测试运行器：负责发现、解析、并发调度和汇总测试用例。
$(TEST_RUNNER): $(SRC_DIR)/scripts/run-tests.c | $(BUILD_DIR)
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
	TEST_JOBS=$(TEST_JOBS) TEST_RUN_MODE=fast sh tests/run-tests.sh $(TARGET)

# 所有运行用例都生成并链接原生可执行文件，用于完整检查目标代码和链接路径。
test-native: $(TARGET) $(RUNTIME_STATIC) $(RUNTIME_SHARED)
	TEST_JOBS=$(TEST_JOBS) TEST_RUN_MODE=native sh tests/run-tests.sh $(TARGET)

# 使用 C 测试运行器执行 fast 模式，避免 shell/awk/sed 的调度开销。
test-by-c: $(TARGET) $(RUNTIME_STATIC) $(RUNTIME_SHARED) $(TEST_RUNNER)
	TEST_JOBS=$(TEST_JOBS) TEST_RUN_MODE=fast $(TEST_RUNNER) $(TARGET)

# 使用 C 测试运行器执行 native 模式。
test-native-by-c: $(TARGET) $(RUNTIME_STATIC) $(RUNTIME_SHARED) $(TEST_RUNNER)
	TEST_JOBS=$(TEST_JOBS) TEST_RUN_MODE=native $(TEST_RUNNER) $(TARGET)

test_hello: $(TARGET)
	$(TARGET) tests/run-pass/basics/hello.tp -o ./build/hello
	./build/hello

# ---------------------------------------------------------------------------
# 安装
#
# 安装布局必须和编译器的搜索顺序对上，否则装完的 tap 找不到 Runtime 和标准库：
#   bin/tap                          编译器自身（搜索起点是它所在目录）
#   lib/libtap_runtime.a             原生可执行文件链接的静态库
#   lib/libtap_runtime.{dylib,so}    fast 模式给 lli --load 用的共享库
#   share/tap/std/*.tp               标准库（prelude 也在里面）
#
# 对应代码：
#   src/run.c    Runtime：TAP_RUNTIME_PATH → 编译器所在目录 → <编译器目录>/../lib
#   src/prelude.c 标准库：TAP_STD_PATH → ./std → <编译器目录>/../std → <编译器目录>/../share/tap/std
#   src/module.c  模块：同上，另外还有 TAP_MODULE_PATH 和当前目录
#
# 也就是说，装到 <prefix>/bin + <prefix>/lib + <prefix>/share/tap/std 之后，
# 不需要设置任何环境变量就能直接用。CMakeLists.txt 里的 install 规则是同一套布局。
#
# 权限不够时用 PREFIX 换个可写的位置，例如：
#   make install PREFIX=$HOME/.local
# 打包做阶段性安装时用 DESTDIR：
#   make install DESTDIR=/tmp/stage PREFIX=/usr
# ---------------------------------------------------------------------------
PREFIX ?= /usr/local
DESTDIR ?=
INSTALL ?= install

BINDIR = $(DESTDIR)$(PREFIX)/bin
LIBDIR = $(DESTDIR)$(PREFIX)/lib
INCLUDEDIR = $(DESTDIR)$(PREFIX)/include/tap
STDDIR = $(DESTDIR)$(PREFIX)/share/tap/std

# Windows 的 DLL 要放在可执行文件旁边才能被找到，其余平台放 lib。
ifeq ($(OS),Windows_NT)
    RUNTIME_SHARED_DEST = $(BINDIR)
else
    RUNTIME_SHARED_DEST = $(LIBDIR)
endif

# 只装运行需要的东西：编译器、两种 Runtime、标准库。头文件一并装上，方便用 C 扩展 Runtime。
install: $(TARGET) $(RUNTIME_STATIC) $(RUNTIME_SHARED)
	mkdir -p $(BINDIR) $(LIBDIR) $(INCLUDEDIR) $(STDDIR)
	$(INSTALL) -m 755 $(TARGET) $(BINDIR)/tap$(EXE)
	$(INSTALL) -m 644 $(RUNTIME_STATIC) $(LIBDIR)/libtap_runtime.a
	$(INSTALL) -m 644 $(RUNTIME_SHARED) $(RUNTIME_SHARED_DEST)/$(notdir $(RUNTIME_SHARED))
	$(INSTALL) -m 644 runtime/include/tap_runtime.h $(INCLUDEDIR)/
	$(INSTALL) -m 644 std/*.tp $(STDDIR)/
	@echo "installed: $(BINDIR)/tap$(EXE)"
	@echo "std:       $(STDDIR)"

# 只删 install 放进去的文件，不动 $(PREFIX) 下的其他内容。
uninstall:
	rm -f $(BINDIR)/tap$(EXE)
	rm -f $(LIBDIR)/libtap_runtime.a
	rm -f $(LIBDIR)/$(notdir $(RUNTIME_SHARED))
	rm -f $(BINDIR)/$(notdir $(RUNTIME_SHARED))
	rm -f $(INCLUDEDIR)/tap_runtime.h
	rm -f $(STDDIR)/*.tp
	rmdir $(INCLUDEDIR) $(STDDIR) 2>/dev/null || true

.PHONY: all clean test test-native test-by-c test-native-by-c test_hello install uninstall
