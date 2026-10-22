# TODO

先补齐以下能力：
1. [x] String.byte_at()、slice()、内容比较。
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

## `pub` 类型的模块前缀（已完成，只剩限定类型名）

两个包各写 `pub struct Point` 曾经会报 `duplicate struct 'Point'`，**不解决第三方包生态
就不存在**。**这一条已经解决**：所有类型现在都改名成 `__tap_module_N.Name`，引用处按
三级查找解析：

1. 本模块自己声明的类型（pub 与否都在这一级）
2. 裸名导入明确引入的类型（`import m.T;` / `import m.*;`）
3. 命名空间导入的模块里唯一导出这个名字的那个（`import std.vec as vec;` → 裸名 `Vec`）

第①②级是明确来源，第③级是兜底——所以「一个包定义 `Point`、又依赖另一个也定义 `Point`
的包」完全正常，只有第③级多个来源时才报歧义。详见
[`module.md`](module.md) 的「类型名的解析」一节。

**还剩一件事：限定类型名（`a.Point`）不支持。** `parse_type` 只读一个标识符，写
`a.Point` 会报 `expected token '='`。这是唯一能显式消歧的写法，缺了它，「同时导入两个都
导出 `Point` 的模块、又确实需要写出类型名」就只能靠改名绕过（不写类型名、靠推断也可以）。
改动不大（照 `parse_primary` 的点号循环），但属于独立一期。

## 已知问题

剩下的缺陷和设计缺口统一记在 [`bug.md`](bug.md)，**目前全部已修**：

- **B1~B5**（结构体字面量字段类型 / 实参和 return 的标量类型 / bool 隐式转换 /
  match 泛型载荷调方法 / 通配分支让编译器段错误）。
- **D1/D2**：整数除零、`MIN / -1`、移位量越界改成运行时检查（报错退出，和数组越界一致）。
- **D3**：有符号/无符号混算按 C 的 `usual arithmetic conversions` 对齐，不加诊断。
  唯一的差异是两边都比 `int` 窄时不做整型提升（`i8 / u8` 按 `u8` 算），见
  [`data-types.md`](data-types.md)。

### match 绑定泛型载荷（已修，2026-10-17）

`process_statements` / `process_expression` 原先都没把分支的载荷绑定登记进泛型阶段的
类型环境，`inner.len()` 找不到接收者类型，泛型方法 `len` 没有被特化，最后误报
「method 'len' is only available on String values」。

原以为难点是执行时机（字段类型实参要到后半程才解析），实际不需要——函数参数走的是同一条
路。修法是两个 match 分支都先 `bind_match_payloads`，载荷类型取自降级后的内部结构体，
泛型枚举再按 `type_arguments` 做替换。细节见 [`bug.md`](bug.md) 的 B4。
