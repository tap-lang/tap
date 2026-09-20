# TODO

先补齐以下能力：
1. [x] string.byte_at()、slice()、内容比较。
2. [x] StringBuilder 和动态 ByteVec。
3. [x] sizeof(T)。
4. [x] 泛型 Vec<T>。
5. [x] parse_f64 和整数转换（`std.parse` 的 `parse_i64` / `parse_f64` / `format_i64`）。
6. [x] 带载荷枚举（tagged union）：构造、`match` 解构、穷尽性检查。
7. [x] 泛型载荷枚举（`Option<T>`、`Result<T, E>`），支持多种实例化共存与嵌套实例化。
8. [x] 构造时显式写类型实参（`Option<i32>.Some(42)`），并校验实参数量与逐项类型。
9. [x] `match` 作为表达式（`let x = match ...`），分支值可以是标量或结构体，支持嵌套。
   仍缺嵌套模式（`Shape.Dot(Point { x: 0, y: 0 })`）和分支守卫。
10. [x] JSON 转义 API（`std.json` 的 `escape` / `escape_quoted` / `append_escaped` /
    `append_quoted` / `unescape`，覆盖控制字符、`\uXXXX` 和代理对）。
11. [x] format_f64：`std.parse` 的 `format_f64` 通过 Runtime 的 `__tap_format_f64` 调用
    C 库转换，输出最短可往返的十进制字符串（精确展开需要大整数算法）。
12. 递归载荷枚举：成员载荷不能是枚举自身，需要先有指针或 Box 语义。

## 已知问题

### match 绑定遇到泛型结构体载荷时无法调用方法

```text
enum Holder {
    Item(Vec<i32>),
    Empty,
}

match (h) {
    Holder.Item(inner) => { print("%llu\n", inner.len()); }   // 报错
    ...
}
```

`inner.len()` 报 `method 'len' is only available on string values`。字段访问（`inner.x`）和
非泛型结构体载荷（如 `StringBuilder`）都正常，函数参数形式（`fn f(v: Vec<i32>)` 里的
`v.len()`）也正常。

**原因**：泛型单态化阶段的两个语句遍历器（`materialize_statements` 和 `process_statements`）
原先都没处理 `NODE_MATCH_STATEMENT`，match 臂体被整段跳过；补上遍历之后仍然失败，因为
`process_statements` 没有把分支的载荷绑定登记进它自己的类型环境，`inner.len` 找不到接收者
类型，泛型方法 `len` 就没有被特化成 `len$Vec$i32`，Codegen 的 `find_method_function` 因此
匹配不上。

**修复方向**：在 `process_statements` 的 match 分支里按「被匹配类型 → 枚举 → 成员载荷字段」
登记绑定类型。难点在**执行时机**：`specialize_generics` 里结构体字段的类型实参要到后半程才
解析，而 `process_statements` 跑在前半程，此时拿到的是模板结构体的字段类型（形如 `T`）而不是
实例化后的类型。需要先解决这个顺序问题，否则登记进去的是错的类型。
