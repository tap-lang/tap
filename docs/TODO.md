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
10. JSON 转义 API。
11. format_f64：把 `double` 渲染成十进制字符串，需要大整数算法或 Runtime 的 `snprintf` 支持。
12. 递归载荷枚举：成员载荷不能是枚举自身，需要先有指针或 Box 语义。
