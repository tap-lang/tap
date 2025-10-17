## 目标 triple
Clang 使用 target triple 来指定目标平台，格式为：`架构-供应商-操作系统-ABI`

例如：
- `x86_64-unknown-linux-gnu`：64 位 Linux 平台，使用 glibc 库
- `aarch64-unknown-linux-gnu`：64 位 ARM 平台，使用 glibc 库
- `arm64-apple-darwin`：64 位 macOS 平台
- `x86-64-apple-darwin`：64 位 macOS 平台

### 查看支持的 Target
```sh
# 查看 Clang 支持的所有 target
clang -print-targets

# 查看支持的架构
clang -print-supported-cpus

# 查看详细的 target 信息
clang -print-target-triple
```

### 编译示例
````sh
clang --target=arm64-apple-darwin -Wall -Wextra -g -o hello hello.c
```

