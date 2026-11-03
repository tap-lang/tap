# 示例程序

全部示例都用 `tap run` 直接跑（先按[构建文档](../docs/build.md)编出 `build/tap`）：

```bash
./build/tap run examples/hello.tp
```

| 文件 | 内容 |
|---|---|
| [`hello.tp`](hello.tp) | 最小程序，验证工具链装好了 |
| [`fibonacci.tp`](fibonacci.tp) | 递归求斐波那契，演示函数与 `for` |
| [`multiplication_table.tp`](multiplication_table.tp) | 九九乘法表，演示嵌套循环 |
| [`json_poc.tp`](json_poc.tp) | JSON 解析器与序列化器 |
| [`snake.tp`](snake.tp) | 终端贪吃蛇（交互） |
| [`tetris.tp`](tetris.tp) | 终端俄罗斯方块（交互） |
| [`tcp_echo.tp`](tcp_echo.tp) | `std.net` 写的 TCP echo 服务器 |

## json_poc.tp

只用 tap 和标准库实现的 JSON 解析器与序列化器，覆盖 object / array / string /
number / true / false / null，演示递归结构体、泛型动态数组、字符串构建、字符串转义
和数值格式化的用法：

```bash
./build/tap run examples/json_poc.tp
```

## snake.tp

交互式贪吃蛇，支持 WASD 和终端方向键真实键值，按 `q` 退出：

```bash
./build/tap run examples/snake.tp
```

## tetris.tp

俄罗斯方块：7-bag 随机发牌、软降与硬降、消行计分和按等级加速，方块由彩色 emoji
拼成（同一块内按格子相对位置斜向取色，一块本身就是一组彩色组合），落点提示按 `T`
开关（默认关），WASD / 方向键操作，按 `q` 退出：

```bash
./build/tap run examples/tetris.tp
```

## tcp_echo.tp

监听 `127.0.0.1:18080`，每来一个连接就把收到的字节原样回显。`std.net` 全靠 `extern`
绑 libc，没有往 `runtime.c` 加一行 C 代码；`accept` / `receive` 都是阻塞语义。

```bash
./build/tap run examples/tcp_echo.tp
# 另开一个终端：
nc 127.0.0.1 18080
```

Ctrl-C 结束进程。
