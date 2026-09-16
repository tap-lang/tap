# 一些命令

## 查看程序返回值
```sh
./build/tap tests/run-pass/basics/hello.tp -o ./build/hello
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
