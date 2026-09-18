# TODO

先补齐以下能力：
1. [x] string.byte_at()、slice()、内容比较。
2. [x] StringBuilder 和动态 ByteVec。
3. [x] sizeof(T)。
4. [x] 泛型 Vec<T>。
5. [x] parse_f64 和整数转换（`std.parse` 的 `parse_i64` / `parse_f64` / `format_i64`）。
6. [x] 带载荷枚举（tagged union）：构造、`match` 解构、穷尽性检查。
7. [x] 泛型载荷枚举（`Option<T>`、`Result<T, E>`），支持多种实例化共存与嵌套实例化。
8. 构造时显式写类型实参，例如 `Option<i32>.Some(42)`；当前只能由目标类型推断。
9. `match` 作为表达式（`let x = match ...`）、嵌套模式、分支守卫。
10. [x] JSON 转义 API（`std.json` 的 `escape` / `escape_quoted` / `append_escaped` /
    `append_quoted` / `unescape`，覆盖控制字符、`\uXXXX` 和代理对）。
11. [x] format_f64：`std.parse` 的 `format_f64` 通过 Runtime 的 `__tap_format_f64` 调用
    C 库转换，输出最短可往返的十进制字符串（精确展开需要大整数算法）。
12. 递归载荷枚举：成员载荷不能是枚举自身，需要先有指针或 Box 语义。
