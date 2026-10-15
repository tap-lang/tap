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

## trait v1 的后续

已经落地的是「纯约束 + 显式 impl」：`trait` 声明方法签名，`impl Trait for Type` 提供实现，
`fn f<T: Trait>` 在单态化时查 impl 表；impl 方法与 trait 声明的签名一致性（方法齐全 + 参数个数
/ 类型 / 返回类型 / 接收者）在模块加载完统一校验。后续可以补：

- **关联类型**（`type Item;`）：要改 `unify_type`，让约束参与类型实参推断。
- **默认方法**：trait 里给方法体，impl 可以覆盖 —— 需要引入「覆盖」规则。
- **`dyn Trait` 运行时多态**：要引入胖指针和 vtable，和现在「编译期单态化、运行时不保存
  泛型信息」的方针冲突，是另一个量级的工程。
- **运算符重载**：`+` / `==` 现在由 `scalar_types_compatible()` 统一管隐式转换，让 trait
  接管要改类型检查入口。
- **泛型类型的 impl**（`impl Show for Vec<T>`）：现在解析阶段就拒绝。

## 包管理的拦路虎：`pub` 类型是全局裸名

两个包各写 `pub struct Point` 会报 `duplicate struct 'Point'`（且不带位置信息）。
**不解决，第三方包生态不存在**——每个包都得给类型起全局唯一名。

机制其实已经有了：非 `pub` 类型会被改名成 `__tap_module_N.Point`，两个包各自定义私有
`Point` 完全正常。只是 `pub` 类型跳过了这一步。

修法：让 `pub` 类型也改名，由 `import` 把类型名带进当前文件作用域，并支持命名空间限定
类型（`a.Point`，现在明确不支持）。动的是类型名解析，是编译器最大的单项工程。

详见 tapm 仓库的 `docs/design.md`。

## 已知问题

### 顶层 `const` 不能用字符串初始化

```text
const VERSION: string = "0.1.0";     // error: global constant 'VERSION' initializer type mismatch
```

对照实测：

| 写法 | 结果 |
|---|---|
| 顶层 `const X: string = "..."` | ✗ `initializer type mismatch` |
| **函数内** `const X: string = "..."` | ✓ 正常 |
| 顶层 `const X: i32 = 3` / `f64` | ✓ 正常 |

模块里的 `pub const` 同样失败（诊断会带上改名后的符号名，如
`global constant '__tap_module_6.TEMPLATE_VERSION' ...`）。

**临时绕法**：改成函数返回字符串，或把字面量内联。`tapm/src/init.tp` 里的版本号就是这么绕的。

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
