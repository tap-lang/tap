# TODO

先补齐以下能力：
1. [x] string.byte_at()、slice()、内容比较。
2. [x] StringBuilder 和动态 ByteVec。
3. [x] sizeof(T)。
4. [x] 泛型 Vec<T>。
5. [x] parse_f64 和整数转换（`std.parse` 的 `parse_i64` / `parse_f64` / `format_i64`）。
6. 带载荷枚举或 tagged union。
7. JSON 转义 API。
8. format_f64：把 `double` 渲染成十进制字符串，需要大整数算法或 Runtime 的 `snprintf` 支持。
