# Standard Library

tap 标准库由两部分组成：

- `std/*.tp`：用 tap 编写的标准库源码。
- `runtime/`：用 C 实现的 Runtime ABI，提供需要操作系统支持的能力。

编译器会自动加载 `std/prelude.tp`。其他标准库模块通过 `import std.xxx;`
显式导入。

## Prelude

`std/prelude.tp` 会在普通编译、`run`、`-ir`、`-emit-obj`、`-run-lli`
流程中自动加载，用户源码无需导入即可调用其中的公共函数。

当前 Prelude 导出：

| 函数 | 说明 |
| --- | --- |
| `read_key(): i32` | 非阻塞读取真实键值；无按键时返回 `-1` |
| `sleep_ms(milliseconds: i32): i32` | 休眠指定毫秒；成功返回 `0`，失败返回 `-1` |
| `clear_screen(): i32` | 清空终端并将光标移动到左上角；成功返回 `0` |
| `random(maximum: i32): i32` | 当 `maximum > 0` 时返回 `[0, maximum)`，否则返回 `0` |
| `panic(message: string): i32` | 打印 `panic: <message>` 到标准错误并以状态 `1` 终止；不会返回 |
| `exit(code: i32): i32` | 以状态码 `code` 终止程序；不打印任何东西，不会返回 |
| `printf(format: string, ...): i32` | 直接对接 libc 的 `printf`；格式串由调用方负责，返回写出的字符数 |
| `Result<T, E>` | 可恢复错误的类型：`Ok(T)` 或 `Err(E)` |
| `is_ok(value): bool` / `is_err(value): bool` | 判断 `Result` 成功或失败 |
| `unwrap_or(value, fallback): T` | 取成功值；失败时用 `fallback` |
| `unwrap(value): T` | 取成功值；失败时 `panic`（用 `Err` 的信息） |
| `expect(value, message): T` | 取成功值；失败时用 `message` `panic` |

说明：`read_key()` 返回 Runtime 读到的真实键值，不会把方向键映射为 WASD。POSIX 方向键会在一次调用中消费 ANSI 序列，并返回末尾方向字节 `65/66/67/68`；Windows 方向键通常返回 `72/80/77/75`。

`panic` 和 `exit` 都是终止程序的手段，分工在于**要不要说话**：

| | 输出 | 退出码 | 用途 |
| --- | --- | --- | --- |
| `panic(message)` | 标准错误打印 `panic: <message>` | 固定 `1` | 调用方无法合理恢复的情况 |
| `exit(code)` | 什么都不打印 | 由调用方决定 | 正常但需要提前结束，例如命令行参数不对时返回 `2` |

两者都不会返回，都走 libc 的 `exit`，所以已经写进标准输出缓冲区、还没落盘的内容会被刷新出来。

### printf 与 print 的分工

`printf` 是 Prelude 里唯一带 `...` 的函数，直接对接 libc 的同名符号：

```text
extern fn printf(format: string, ...): i32;
```

| | 格式串 | 适用场合 |
| --- | --- | --- |
| `print` 语句 | 第一个参数是字符串字面量时当格式串用；不是字面量时由编译器按类型自动挑 `%d` / `%f` / `%lld` | 日常输出、快速调试 |
| `printf` | 完全由调用方给定，编译器不检查也不改写 | 需要精确控制格式：`%05d`、`%.3f`、`%x`、`%c`、`%%` |

`...` 只在 `extern` 声明上合法，并且必须是参数列表的最后一项、前面至少有一个具名参数。
多出来的实参按 **C 的默认实参提升规则**传递：

| 实参类型 | 传递时 |
| --- | --- |
| 位宽小于 32 的有符号整数（`i8` / `i16`） | 符号扩展成 `i32` |
| 位宽小于 32 的无符号整数（`u8` / `u16`）和 `bool` | 零扩展成 `i32`（`bool` 的 `i1` 变成 `0` / `1`） |
| `f32` | 提升成 `f64`，所以 `%f` 能直接吃 `f32` 变量 |
| `i32` 及以上整数、`f64`、`string`、指针 | 原样传递 |

`i64` 不提升，格式串要自己对齐（`%lld`）；`string` 在 LLVM 里就是 `i8*`，直接用 `%s`。

```text
fn main(): i32 {
    let name: string = "tap";
    let small: i8 = -3;
    printf("name=%s small=%d hex=%x\n", name, small, 255);
    return 0;
}
```

覆盖用例见 [`printf.tp`](../tests/run-pass/stdlib/printf.tp)；自己声明变参 `extern`
的写法见 [`variadic_extern.tp`](../tests/run-pass/types/variadic_extern.tp)。

`-lex` 和 `-parse` 只处理传入的源文件，不加载 Prelude。

Prelude 中的函数名会进入全局函数集合。用户源码不能重复定义同名函数，
否则编译器会报重复定义错误。

## Modules

非 Prelude 标准库模块需要显式导入。模块导入使用点分隔路径，默认名称空间为
最后一段模块名：

```text
import std.math;

fn main(): i32 {
    return math.square(5);
}
```

当前标准库模块：

| 模块 | 函数 | 说明 |
| --- | --- | --- |
| `std.env` | `argc(): i32` | 返回当前程序的命令行参数数量，包含程序路径自身 |
| `std.env` | `arg(index: i32): string` | 返回指定位置的命令行参数；越界时返回空字符串 |
| `std.env` | `args(): Vec<string>` | 返回全部命令行参数；用完要 `vec.free` |
| `std.env` | `var(name: string): string` | 返回指定环境变量的值；不存在时返回空字符串 |
| `std.env` | `varc(): i32` | 返回当前进程环境变量数量 |
| `std.env` | `vars(): Vec<string>` | 返回全部环境变量条目（`NAME=VALUE`）；用完要 `vec.free` |
| `std.math` | `PI: f64` | 圆周率常量，值为 `3.141592653589793` |
| `std.math` | `square<T>(value: T): T` | 返回平方值；`T` 由实参推断，也可显式写 `square<f64>(...)` |
| `std.math` | `abs<T>(value: T): T` | 返回绝对值；无符号类型原样返回 |
| `std.math` | `pow<T>(base: T, exponent: u32): T` | 整数次幂，快速幂；指数必须是非负整数 |
| `std.math` | `pow_f64(base: f64, exponent: f64): f64` | 浮点幂，指数可为小数或负数 |
| `std.memory` | `malloc<T>(size: uint): *T` | 分配指定字节数的堆内存 |
| `std.memory` | `realloc<T>(pointer: *T, size: uint): *T` | 调整已有堆内存大小 |
| `std.memory` | `free<T>(pointer: *T): i32` | 释放堆内存，成功返回 `0` |
| `std.vec` | `Vec<T>` | 可扩容的泛型动态数组 |
| `std.vec` | `new<T>()/with_capacity<T>(capacity)` | 创建空动态数组 |
| `std.vec` | `len/capacity/reserve/push/get/set/clear` | 容量管理、追加、读写和清空元素 |
| `std.vec` | `free<T>(vec): i32` | 释放内部缓冲区，不递归释放元素资源 |
| `std.byte_vec` | `ByteVec` | 可扩容的 `u8` 字节容器 |
| `std.byte_vec` | `new()/with_capacity(capacity)` | 创建字节容器 |
| `std.byte_vec` | `push/extend/get/set/clear` | 追加、读写和清空字节 |
| `std.byte_vec` | `to_string(): string` | 复制有效字节为 Runtime 管理的字符串 |
| `std.string_builder` | `StringBuilder` | 基于 `ByteVec` 的可增量字符串构建器 |
| `std.string_builder` | `append/append_byte/clear` | 追加字符串或字节并复用容量 |
| `std.string_builder` | `to_string(): string` | 获取与后续 Builder 修改无关的字符串快照 |
| `std.parse` | `parse_i64(text: string): Result<i64, string>` | 解析十进制整数，失败返回 `Err` |
| `std.parse` | `parse_f64(text: string): Result<f64, string>` | 解析十进制浮点，失败返回 `Err` |
| `std.parse` | `format_i64(value: i64): string` | 把整数格式化为十进制字符串 |
| `std.parse` | `format_f64(value: f64): string` | 把浮点数格式化为最短可往返的十进制字符串 |
| `std.json` | `escape(text: string): string` | 转义为 JSON 字符串内容，不含两侧引号 |
| `std.json` | `escape_quoted(text: string): string` | 转义并补上两侧引号，结果可直接放进 JSON |
| `std.json` | `append_escaped(builder, text): StringBuilder` | 把转义结果追加进构建器，不含两侧引号 |
| `std.json` | `append_quoted(builder, text): StringBuilder` | 追加转义结果并补上两侧引号 |
| `std.json` | `unescape(text: string): Result<string, string>` | 反转义 JSON 字符串内容，非法转义返回 `Err` |
| `std.fs` | `File` | 已打开文件的句柄（`handle` 是不透明指针） |
| `std.fs` | `open(path, mode): File` | 打开文件；`mode` 与 C 的 `fopen` 一致，务必带 `b`（`rb` / `wb` / `ab`） |
| `std.fs` | `is_open(file): bool` | 句柄是否有效；已关闭或打开失败时返回 `false` |
| `std.fs` | `read(file, count): string` | 读取最多 `count` 个字节；到末尾或失败返回空字符串 |
| `std.fs` | `write(file, data): i64` | 写入 `data` 的全部字节，返回写入数；失败返回 `-1` |
| `std.fs` | `read_all(file): string` | 读到末尾，把剩余内容拼成一个字符串 |
| `std.fs` | `eof(file): bool` | 是否已经读到文件末尾 |
| `std.fs` | `close(file): i32` | 关闭文件，成功返回 `0` |
| `std.fs` | `remove(path): i32` | 删除文件，成功返回 `0` |
| `std.fs` | `read_text(path): Result<string, string>` | 整个文件读成字符串，打不开返回 `Err` |
| `std.fs` | `write_text(path, data): i64` | 覆盖写入，返回写入字节数；打不开返回 `-1` |
| `std.fs` | `append_text(path, data): i64` | 追加到末尾，返回写入字节数；打不开返回 `-1` |
| `std.fs` | `exists(path): bool` | 能否以只读方式打开；不存在或无权限返回 `false` |
| `std.net` | `Socket` | TCP 套接字句柄；`fd` 小于 `0` 表示已关闭或创建失败 |
| `std.net` | `parse_ipv4(host): Result<[u8; 4], string>` | 解析 `a.b.c.d` 字面量；不做 DNS，格式非法返回 `Err` |
| `std.net` | `build_address(host, port): Result<[u8; 16], string>` | 构造 16 字节 `sockaddr_in`，端口按大端拆字节 |
| `std.net` | `listen_on(host, port, backlog): Result<Socket, string>` | 创建、绑定并监听；`port` 传 `0` 由内核分配 |
| `std.net` | `connect_to(host, port): Result<Socket, string>` | 阻塞连接服务器，失败返回 `Err` |
| `std.net` | `accept_from(server): Result<Socket, string>` | 取一个已完成握手的连接，阻塞等待 |
| `std.net` | `local_port(socket): u16` | 返回实际绑定的端口；句柄无效或查询失败返回 `0` |
| `std.net` | `receive(socket, buffer, capacity): i64` | 收数据写入缓冲区；`0` 表示对端关闭，`-1` 出错 |
| `std.net` | `send_text(socket, data): i64` | 发送字符串，返回实际发出的字节数 |
| `std.net` | `send_bytes(socket, buffer, length): i64` | 发送缓冲区里的裸字节；和 `receive` 配对 |
| `std.net` | `close_socket(socket): Socket` | 关闭并返回失效句柄，用法是 `socket = close_socket(socket)` |
| `std.net` | `is_open(socket): bool` | 句柄是否有效；创建失败或关闭后返回 `false` |
| `std.net` | `AF_INET` / `SOCK_STREAM` / `ADDRESS_LENGTH` | 地址族、套接字类型常量和 `sockaddr_in` 长度 |

`std.env` 示例：

```text
import std.env as env;
import std.vec as vec;

fn main(): i32 {
    let values: Vec<string> = env.args();
    print("%d\n", env.argc());
    print("%s\n", values.get(1));
    print("%s\n", env.var("PATH"));

    let entries: Vec<string> = env.vars();
    print("%s\n", entries.get(0));

    // 容器在堆上，用完要自己释放。
    vec.free(values);
    vec.free(entries);
    return 0;
}
```

使用 `run` 传递被编译程序的参数时，用 `--` 分隔编译器参数和程序参数：

```sh
tap run app.tp -- alpha beta
```

`std.memory` 示例：

```text
import std.memory as memory;

fn main(): i32 {
    // 显式指定 T 为 string。
    let values: *string = memory.malloc<string>(16);
    values[0] = "hello";
    values = memory.realloc(values, 32);
    values[1] = "tap";

    // 根据变量的 *i32 类型推断 T。
    let numbers: *i32 = memory.malloc(8);

    memory.free(values);
    return memory.free(numbers);
}
```

泛型类型实参直接写在函数名后的尖括号中，不使用 `::`。`malloc` 可以通过
`memory.malloc<i32>(8)` 显式指定元素类型，也可以根据变量声明中的 `*i32` 推断类型。
`realloc` 和 `free` 会从传入的指针推断类型。分配大小和扩容大小的单位都是字节，调用方需要
按元素大小正确计算容量；释放后不能继续访问原指针。

`std.vec` 示例：

```text
import std.vec as vec;

fn main(): i32 {
    let values: Vec<i32> = vec.new();
    values.push(10);
    values.push(20);
    values.set(1, 22);
    print("%llu %d\n", values.len(), values.get(1));
    return values.free();
}
```

`Vec<T>` 的 `len` 和 `capacity` 使用 `uint`，索引也使用 `uint`。`get()` 和 `set()` 会在
索引越界时终止程序。`clear()` 只把长度归零并保留容量；`free()`
只释放 Vec 自己的元素缓冲区，不会递归释放元素持有的资源。`push()`、`set()`、`clear()`
和 `reserve()` 作为方法语句调用时会自动把返回的新结构体写回接收者。

字符串动态数组直接用 `Vec<string>`：

```text
import std.vec as vec;

fn main(): i32 {
    let values: Vec<string> = vec.new();
    values.push("hello");
    values.push("tap");

    print("%llu\n", values.len());
    print("%s %s\n", values.get(0), values.get(1));

    values.free();
    return 0;
}
```

`Vec` 是值类型。直接写 `values.push("hello");` 或 `values.set(0, "HELLO");`
时，编译器会把返回的结构体自动写回 `values`。`free` 只释放内部缓冲区，
不会清空原结构体字段；释放后不要继续访问同一个容器。

`std.byte_vec` 示例：

```text
import std.byte_vec as bytes;

fn main(): i32 {
    let buffer: ByteVec = bytes.with_capacity(4);
    buffer.extend("hello");
    buffer.push(33);
    print("%s\n", buffer.to_string());
    return buffer.free();
}
```

`ByteVec` 容量不足时按倍数扩容。`clear()` 只将长度置零并保留容量；
`free()` 后不能继续访问容器。`to_string()` 会复制当前字节，因此之后清空或修改
`ByteVec` 不会改变已返回的字符串。由于 tap `string` 使用结尾 NUL，包含 `0` 字节的
`ByteVec` 不能转换为字符串。

`std.string_builder` 示例：

```text
import std.string_builder as builder;

fn main(): i32 {
    let output: StringBuilder = builder.new();
    output.append("{\"name\":\"");
    output.append("tap");
    output.append("\"}");
    print("%s\n", output.to_string());
    return output.free();
}
```

`StringBuilder` 内部使用 `ByteVec`。`append()` 追加字符串的 UTF-8 字节，
`append_byte()` 追加单个字节，`to_string()` 返回独立快照。

`std.parse` 示例：

```text
import std.parse;

fn main(): i32 {
    let parsed: Result<i64, string> = parse.parse_i64("-456");
    print("value=%lld text=%s\n",
        unwrap_or(parsed, 0), parse.format_i64(unwrap_or(parsed, 0)));

    let ratio: Result<f64, string> = parse.parse_f64("1.5e-3");
    print("ratio=%f\n", unwrap_or(ratio, 0.0));

    // format_f64 取最短的、能往返的表示，不补齐到固定小数位。
    let tenth: Result<f64, string> = parse.parse_f64("0.1");
    print("text=%s\n", parse.format_f64(unwrap_or(tenth, 0.0)));
    return 0;
}
```

`parse_i64` 接受可选正负号，`parse_f64` 额外接受小数部分和 `e` / `E` 指数。两者都要求整个
输入被完整消费：空串、只有符号、含有非数字字符或末尾有多余字符时返回 `Err`，错误信息说明
原因。浮点的数字要求与 C 的 `strtod` 一致，整数部分和小数部分至少一侧要有数字，因此 `.5` 和
`1.` 都合法。

不想处理分支时用 `result.unwrap_or` 给个兜底值：

```text
let parsed: Result<i64, string> = parse.parse_i64(text);
print("%lld\n", unwrap_or(parsed, 0));
```

`format_f64` 取最短的、能往返的表示：`0.1` 输出 `0.1`，圆周率输出 `3.141592653589793`，
极大或极小的值用指数形式。它不提供定点位数、宽度或对齐控制。`inf` / `-inf` / `nan` 直接
输出字面文本——JSON 没有这三种写法，需要写出 JSON 的调用方要自行处理。NaN 的符号位会被
丢弃（各平台默认 NaN 的符号不一致），正负都输出 `nan`。

注意 tap 的浮点字面量默认是 `f32`（见[数据类型](data-types.md#标量类型)），所以
`format_f64(0.1)` 会先把 `0.1` 按 `f32` 取近似、再放宽到 `f64`，输出
`0.10000000149011612`。要拿到 `f64` 精度的输入，用 `f64` 类型的变量、运算结果，或者
`parse.parse_f64("0.1")` 的 `value`。

`std.json` 示例：

```text
import std.json;
import std.string_builder as sb;

fn main(): i32 {
    // 输出：转义并补上两侧引号，结果可以直接放进 JSON
    print("%s\n", json.escape_quoted("say \"hi\"\n"));

    // 增量拼接时用 append_quoted，省掉每段字符串的临时对象
    let out: StringBuilder = sb.new();
    out = json.append_quoted(out, "key");
    out.append(":");
    out = json.append_quoted(out, "va\\lue");
    print("%s\n", sb.to_string(out));

    // 输入：反转义，非法转义序列通过 ok 报告
    let decoded: Result<string, string> = json.unescape("a\\u0041\\uD83D\\uDE00b");
    // 字段访问要先绑定到变量，不能在字段链上直接调方法。
    let text: string = unwrap_or(decoded, "");
    print("len=%llu\n", text.len());
    return 0;
}
```

输出侧转义 `"`、`\` 以及所有小于 `0x20` 的字符：`\b` `\f` `\n` `\r` `\t` 用短形式，其余控制
字符写成 `\u00XX`。`0x7F` 及以上不是控制字符，按 UTF-8 字节原样透传。

输入侧识别 `\"` `\\` `\/` `\b` `\f` `\n` `\r` `\t` 和 `\uXXXX`。相邻的高/低代理对会合成一个
码点再按 UTF-8 编码；孤立代理、被截断的 `\uXXXX`、未知转义和结尾的裸反斜杠都让 `ok` 为
`false`，此时 `text` 是出错前已经解出的部分。

`escape` 和 `unescape` 都不含两侧引号——引号由 `escape_quoted` / `append_quoted` 补上。

`std.fs` 示例：

```text
import std.fs as fs;

fn main(): i32 {
    let out: File = fs.open("notes.txt", "wb");
    if (!fs.is_open(out)) {
        print("cannot open\n");
        return 1;
    }
    fs.write(out, "hello tap\n");
    fs.close(out);

    let input: File = fs.open("notes.txt", "rb");
    let first: string = fs.read(input, 5);
    let rest: string = fs.read_all(input);
    print("%s|%s eof=%d\n", first, rest, fs.eof(input));
    print("close=%d\n", fs.close(input));
    return fs.remove("notes.txt");
}
```

两件必须知道的事：

- **`File` 是值类型**，`close()` 拿到的是副本，没法把调用方手里的句柄置空。所以 Runtime
  维护了一张打开句柄的登记表（上限 64）：关闭之后 `is_open` 返回 `false`，继续读写也只会
  安全地返回空结果或 `-1`，不会变成 use-after-close。
- **`mode` 务必带 `b`**（`rb` / `wb` / `ab`）。少了 `b`，Windows 上会把 `\n` 翻译成 `\r\n`，
  文本和二进制都会出错。

`read` 到末尾和出错都返回空字符串，用 `eof` 区分。

`read_text` / `write_text` / `append_text` / `exists` 是流式接口的组合，没有额外 Runtime ABI。
`read_text` 返回带 `ok` 的结构体而不是空字符串——否则「文件打不开」和「文件是空的」就分不
开了：

```text
let loaded: Result<string, string> = fs.read_text("notes.txt");
let notes: string = unwrap_or(loaded, "");
```

模块导入、别名、递归加载和错误规则见 [module.md](module.md)。

`std.net` 示例（回环上自连自收）：

```text
import std.net as net;

fn main(): i32 {
    // 端口 0 交给内核挑一个空闲的，再用 local_port 取回来。
    let server: Socket = unwrap(net.listen_on("127.0.0.1", 0, 8));
    let port: u16 = net.local_port(server);

    let client: Socket = unwrap(net.connect_to("127.0.0.1", port));
    let peer: Socket = unwrap(net.accept_from(server));

    net.send_text(client, "ping");
    let buffer: [u8; 64] = [0; 64];
    let count: i64 = net.receive(peer, &buffer[0], 64);
    print("got %lld bytes\n", count);

    // 关闭的效果只能靠返回值传出去，必须写回。
    client = net.close_socket(client);
    peer = net.close_socket(peer);
    server = net.close_socket(server);
    return 0;
}
```

`std.net` 是标准库里唯一**完全不依赖 Runtime** 的模块：全部通过 `extern fn` 直接绑定 libc
的 BSD socket 接口，`runtime.c` 一行都不用加。代价是 `extern` 的名字必须和 C 符号一致，
所以公开 API 避开了 `listen` / `accept` / `connect` / `recv` / `send` / `close` 这些名字，
改用 `listen_on` / `accept_from` / `connect_to` / `receive` / `send_text` / `close_socket`。

四件必须知道的事：

- **`Socket` 是值类型**，和 `File` 一样 `close_socket()` 只能改到副本。区别在于这里没有
  Runtime 侧的句柄登记表，关闭的效果只能靠返回值传出来，**必须写回**：
  `socket = net.close_socket(socket);`。忘了赋值，旧副本的 `fd` 仍是正数，`is_open` 会
  误判成还开着，而那个 `fd` 可能已经被系统分配给了别的连接，继续用就是操作别人的连接。
- **全程阻塞，没有超时**。`accept_from` / `connect_to` / `receive` 都会一直等下去，
  需要超时得自己配非阻塞模式，本模块还没做。
- **只认 IPv4 字面量，没有 DNS**。`"localhost"` 之类的域名会直接返回 `Err`。
- **只支持 POSIX**（macOS / Linux / Cygwin）。Windows 的 winsock 要先 `WSAStartup`，
  而且 `SOCKET` 是 64 位句柄，当前不支持。

`receive` 收到的是裸字节，直接写进调用方给的缓冲区，不经过 Runtime 的「字节转字符串」，
所以内容里含 NUL 也不会终止程序；配 `send_bytes` 就能把收到的内容原样回显或转发。要发
字符串用 `send_text`。两者都不保证把数据一次发完，返回值小于请求长度时调用方要自己接着发
剩下的部分（tap 还没有指针算术，切不出「后半段缓冲区」，所以没有提供自动重发的 `send_all`）。

## 错误处理约定

标准库统一用 `Result<T, E>` 表示**可恢复错误**。`E` 实践中多用 `string`，用来说明失败原因。
`Result` 和下面几个辅助函数都在 Prelude 里，**不需要导入**。

| 场景 | 做法 |
|---|---|
| 可恢复错误（解析、文件打开、转义非法） | 返回 `Result<T, string>`，调用方自行处理或给兜底值 |
| 程序员错误（索引越界、字符串越界） | 打印诊断并终止程序，不返回 `Result` |

区分这类的标准是**能不能在调用处合理恢复**：文件不存在是运行环境的正常情况，调用方能换路径
或用默认值；而 `get(i)` 越界几乎总是逻辑错误，返回 `Result` 只会让每次随机访问都变啰嗦。

**向上传播**用后缀 `?`：内层求值为 `Result`，成功时整个表达式取载荷，失败时从当前函数
提前返回 `Err`，错误信息原样带出、不做包装。它要求所在函数的返回类型也是载荷枚举：

```text
fn pipeline(value: i32): Result<i32, string> {
    let a: i32 = step1(value)?;
    let b: i32 = step2(a)?;
    return Result<i32, string>.Ok(step3(b)?);
}
```

没有 `?` 时每个调用点都要「声明结果变量 + 占位变量 + match 两个分支 + 判失败 + 提前返回」，
三步链要 29 行；有了它是 6 行。`?` 用在 `return` 里、函数实参、`match` 分支、泛型函数里
都可以。

和 `match` 表达式、结构体字面量一样，`?` **需要目标类型**（变量声明的类型标注、返回值类型
等）。没有目标类型时编译器不知道成功那条路按什么类型取载荷，会直接报错。注意 `main` 不能
返回结构体，所以 `main` 里用不了 `?`——在辅助函数里传播，最后在 `main` 处理。

就地取值用 `match` 的表达式形式，或者用 `unwrap_or` 给兜底值：

```text
let parsed: Result<i64, string> = parse.parse_i64(text);
let value: i64 = match (parsed) {
    Result.Ok(number) => number,
    Result.Err(message) => 0,
};
print("%lld\n", unwrap_or(parsed, 0));
```

**失败就终止**用 `panic`（Prelude 提供，无需导入）：打印 `panic: <message>` 到标准错误，
并以状态 `1` 结束进程。它不会返回。

`Result` 上有两个顺势 panic 的快捷方式，只在 `Result<T, string>` 上可用——tap 还没有
「把任意类型转成字符串」的手段，所以错误类型必须是 `string` 才报得出有用的信息：

```text
let number: Result<i64, string> = parse.parse_i64(text);
print("%lld\n", unwrap(number));                       // 失败时 panic，用 Err 的信息
print("%lld\n", expect(number, "config must be int")); // 失败时 panic，用指定的信息
```

辅助函数也可以写成**方法调用**：它们在 Prelude 里是无限定名的，而接收者类型匹配第一个
参数，所以 `parsed.is_ok()` 会解析到 `is_ok(parsed)`。

```text
print("%d\n", parsed.is_ok());
print("%lld\n", parsed.unwrap_or(0));
print("%lld\n", parsed.unwrap());
```

注意 `unwrap` / `expect` 只对 `Result<T, string>` 生效；其他错误类型请自己 `match`。

另外 `assert(condition, message)` 是**内建语句**（不是 Prelude 函数）：失败时打印
`Assertion failed at 文件:行:列: message` 并终止，比 `panic` 多了位置信息。

## Runtime ABI

Runtime 提供 Prelude 无法用纯 tap 表达的能力。Runtime 函数使用 `__tap_`
前缀，避免和用户函数或 libc 符号冲突：

```text
extern fn __tap_read_key(): i32;
extern fn __tap_sleep_ms(milliseconds: i32): i32;
extern fn __tap_clear_screen(): i32;
extern fn __tap_random(maximum: i32): i32;
extern fn __tap_argc(): i32;
extern fn __tap_arg(index: i32): string;
extern fn __tap_env_var(name: string): string;
extern fn __tap_envc(): i32;
extern fn __tap_env(index: i32): string;
// 字符串 ABI 由内建方法和比较运算符调用。
extern fn __tap_string_byte_at(value: string, index: i64): u8;
extern fn __tap_string_slice(value: string, start: i64, end: i64): string;
extern fn __tap_string_compare(left: string, right: string): i32;
extern fn __tap_string_copy_bytes(value: string, destination: *u8, length: uint): i32;
extern fn __tap_bytes_to_string(data: *u8, length: uint): string;
extern fn __tap_malloc(size: uint): *i8;
extern fn __tap_realloc(pointer: *i8, size: uint): *i8;
extern fn __tap_free(pointer: *i8): i32;
// 内存 ABI 由 std.memory 统一声明和封装。
```

普通程序应使用内建字符串操作、Prelude 封装后的 `read_key`、`sleep_ms`、
`clear_screen` 和 `random`，以及 `std.env`、`std.memory` 封装后的函数，不要直接调用
`__tap_` 前缀函数。

Runtime 的 C ABI 声明位于
[`runtime/include/tap_runtime.h`](../runtime/include/tap_runtime.h)，实现位于
[`runtime/src/runtime.c`](../runtime/src/runtime.c)。构建与加载细节见
[runtime.md](runtime.md)。

## Lookup Paths

编译器查找标准库时按以下顺序定位 `std` 目录：

1. 环境变量 `TAP_STD_PATH`。
2. 当前工作目录下的 `std`。
3. 编译器可执行文件相邻的源码目录或安装目录。

原生可执行文件链接 Runtime 静态库；`-run-lli` 运行临时 IR 时加载 Runtime
共享库。Runtime 库目录可以通过 `TAP_RUNTIME_PATH` 指定。

## Adding APIs

新增标准库 API 时优先使用 tap 源码实现：

1. 如果函数可以用现有语言能力表达，放入 `std/*.tp`。
2. 如果函数应默认可用，放入 `std/prelude.tp`。
3. 如果函数需要操作系统、终端、时间、随机数、文件、内存等底层能力，先在
   Runtime 中增加 `__tap_` 前缀 C ABI，再在 Prelude 或模块中提供公共包装。
4. 如果该能力在 libc 里已经有稳定符号（socket、数学函数之类），也可以像 `std.net` 那样
   用 `extern fn` 直接绑定，不碰 Runtime。注意 `extern` 的名字就是链接时的 C 符号名，
   不能加 `__tap_` 前缀，公开 API 要另起名字避开冲突。

新增或修改标准库函数后，需要同步：

- 相关 `.tp` 源文件。
- `docs/std.md` 和必要时的 `docs/runtime.md`。
- `tests/run-pass/stdlib/` 或对应失败测试。

## Current Limits

- 大部分标准库函数仍只覆盖 `i32` 相关能力；`std.parse` 使用 `i64` 和 `f64`。
- `string` 已提供内建 `.len()`、`.byte_at()`、`.slice()` 和内容比较，动态构建可使用
  `std.string_builder`，字符串与数值互转可使用 `std.parse`；尚无 `+` 拼接、查找和
  Unicode 码点级 API。
- `std.parse` 只解析十进制，不识别十六进制、下划线分隔符或 `inf` / `nan`。
- `format_f64` 通过 Runtime 调用 C 库的十进制转换（`__tap_format_f64`），是 `std.parse`
  里唯一不纯 tap 的函数：精确的十进制展开需要大整数算法。
- 内建数组是固定长度类型，没有动态数组字面量；需要可变长度容器请用 `std.vec`（泛型）
  或 `std.byte_vec`。字符串数组用 `Vec<string>`，`std.env.args()` / `vars()` 就返回它。
- 结构体没有析构函数，`Vec` / `ByteVec` 用完要自己调 `free`。
  不释放只是进程退出前一直占着，不会出错。
- `std.net` 只有 TCP over IPv4，只支持 POSIX，且全程阻塞、没有超时；域名解析、UDP、
  非阻塞模式和 `send_all` 都还没有。
- 模块会导出顶层常量、结构体、枚举和函数，但没有可见性控制；导入的类型名当前进入全局类型命名空间。
- Prelude 是自动注入的全局函数集合，不支持按需选择导入。
