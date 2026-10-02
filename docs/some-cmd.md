# 一些命令

## 查看程序返回值
```sh
./build/hello
echo $?
```

## 查看可执行文件依赖的动态库
```sh
# MacOS
otool -L ./build/hello

# Linux
ldd ./build/hello
```

## clang 生成 LLVM IR
```sh
clang -S -emit-llvm tmp/1.c -o tmp/1.c.ll
```