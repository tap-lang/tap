# 借用与引用

tap 有两种指向别处的类型：

| 类型 | 名字 | 承诺 | 检查 |
| --- | --- | --- | --- |
| `*T` | 裸指针 | 无 | 不检查。寿命、别名、非空全由调用方负责 |
| `&T` | 共享借用 | 借用期内源对象有效，且不会被写 | 编译期检查 |
| `&mut T` | 独占借用 | 借用期内源对象有效，且只有我能写 | 编译期检查 |

`*T` 面向 C 互操作和底层库（`std.memory`、`std.vec`），`&T` / `&mut T` 面向普通代码。
两者在机器码里是同一个指针 —— 借用检查是纯编译期的，不产生任何运行时开销。

```text
fn increment(value: &mut i32) {
    value[0] = value[0] + 1;
}

fn length_of(text: &String): uint {
    return text.len();
}

fn main(): i32 {
    let count: i32 = 41;
    increment(&mut count);
    print("%d\n", count);      // 42

    let name: String = "tap";
    print("%llu\n", length_of(&name));
    return 0;
}
```

## 语法

取地址表达式和类型都带 `&`：

```text
&value          // 共享借用，类型 &T
&mut value      // 独占借用，类型 &mut T

let a: &i32 = &value;
let b: &mut i32 = &mut value;
```

`mut` 只出现在借用语法里，**不是变量修饰符** —— tap 的局部变量本来就可写，不需要
`let mut x`。

可以取地址的目标和 `*T` 一致：变量、结构体字段（`&point.x`）、数组或指针元素
（`&items[1]`）。`const` 常量不能取地址（没有只读存储）。

## 转换规则

| 从 | 到 | 允许 | 说明 |
| --- | --- | --- | --- |
| `&mut T` | `&T` | 是 | 能力收窄：独占降级成共享 |
| `&T` / `&mut T` | `*T` | 是 | **遗忘借用**：放弃检查，之后由调用方负责 |
| `*T` | `&T` / `&mut T` | 否 | 裸指针没有任何承诺，不能凭空升级成受检查的借用 |
| `&T` | `&mut T` | 否 | 共享借用的调用方没承诺独占 |

`&T` 退化成 `*T` 是显式的，写出来就是「我知道我在干什么」：

```text
let reference: &i32 = &value;
let raw: *i32 = reference;     // 从这一刻起编译器不再管它
```

## 检查规则

规则实现见 [`src/borrow.c`](../src/borrow.c)，在 Sema 之后、Codegen 之前跑一遍，
只走 AST，不生成任何 IR。

### 1. 借用不能逃出本函数

```text
fn bad(): &i32 {
    let local: i32 = 7;
    return &local;             // error: cannot return a reference to a value
}                              //        that lives in this function
```

引用型参数指向的是函数外，可以原样返回：

```text
fn identity(value: &i32): &i32 {
    return value;              // 合法
}
```

### 2. 借用不能比被借的值活得久

```text
let held: &i32 = &first;
if (first == 1) {
    let second: i32 = 2;
    held = &second;            // error: borrowed value does not live long enough
}
```

### 3. 独占借用期间，源对象不能被碰

```text
let exclusive: &mut i32 = &mut value;
let other: &mut i32 = &mut value;   // error: already mutably borrowed
print("%d\n", value);               // error: mutably borrowed and cannot be used
```

共享借用之间可以并存，但和独占借用互斥：

```text
let a: &i32 = &value;
let b: &i32 = &value;               // 合法
let c: &mut i32 = &mut value;       // error: already borrowed
```

通过共享引用写也是禁止的 —— 那是 `&T` 唯一的承诺：

```text
fn peek(value: &i32) {
    value[0] = 5;                    // error: cannot write through a shared reference
}

let left: &mut i32 = &mut pair.left; // pair 是 &Pair 时报错：
                                     // cannot take a mutable borrow through a shared reference
```

### 4. 返回借用的函数必须只有一个引用参数

函数体是单独检查的，调用方看不到函数内部用了哪个参数，所以需要一条**签名约定**。
tap 采用 Rust 省略规则的子集：**返回借用时，函数必须恰好有一个引用型参数，返回值就
借自它。**

```text
fn first(values: &Vec<i32>): &i32 {
    return &values.data[0];      // 借自 values
}

fn pick(left: &i32, right: &i32): &i32 {
    return left;                 // error: 两个引用参数，无法确定借自哪个
}
```

调用点据此把借用的寿命继续传下去，所以穿过函数调用也一样会被查出来：

```text
fn relay(values: &Vec<i32>): &i32 {
    return first(values);        // 合法：仍然借自 values
}

fn bad(values: &Vec<i32>): &i32 {
    let numbers: Vec<i32> = vec.new();
    return first(&numbers);      // error: 返回的是局部变量的借用
}
```

### 「在栈帧里」是怎么判定的

规则 1 需要区分 `&local.field`（栈帧）和 `&param.field`（别处）。判据是**左值路径
有没有穿过指针或引用**：

```text
fn left_of(pair: &Pair): &i32 {
    return &pair.left;           // 合法：pair 是借用，字段在别处
}

fn bad(pair: &Pair): &i32 {
    let local: Pair = Pair { left: 1, right: 2 };
    return &local.left;          // error: 字段就在本函数的栈帧里
}

fn bad_array(values: &i32): &i32 {
    let items: [i32; 3] = [1, 2, 3];
    return &items[0];            // error: 数组元素在栈帧里
}
```

同样地，`&values.data[0]`（`data` 是 `*T`）算作函数外 —— 元素在堆上，`Vec` 活着
就一直有效。

## 词法语义：借用活到作用域结束

这一版**不做活跃点分析**（Rust 的 NLL）。`&mut x` 一旦绑定到变量，就独占 `x`
直到该变量所在的作用域结束，和「后面还用不用这个引用」无关：

```text
let exclusive: &mut i32 = &mut value;
exclusive[0] = 1;
print("%d\n", value);      // 报错，虽然 exclusive 之后不再使用
```

这是**有意的保守**：报错位置永远等于 `&mut` 所在的作用域，不会因为后面有没有用到
引用而漂移，行为可预测、实现简单。

**临时借用不受影响。** 直接传给函数、不当变量保存的借用，在语句结束时就消失：

```text
increment(&mut count);     // 借用到此为止
print("%d\n", count);      // 合法
```

所以推荐的写法是**把 `&mut` 交给函数，别在局部变量里长期持有**。需要长期持有时，
用 `*T` 显式放弃检查。

## 什么时候用哪个

- 普通函数参数、需要「借来看一眼」或「借来改一下」——用 `&T` / `&mut T`。
- 返回值需要指向参数内部（`fn first(v: &Vec<T>): &T`）——用 `&T`，见规则 4。
- 结构体要长期保存指针、需要自己管寿命——用 `*T`。
- 和 C 打交道——用 `*T` 或 `c_string`。

## 当前限制（L1 的边界）

这些是**明确没做**的，不是 bug：

- **引用参数最多一个**才能返回借用。需要「借自哪个参数」更细粒度时得等生命周期参数。
- **结构体不能存借用。** `struct Parser { src: &String }` 需要生命周期参数，属于 L2。
- **方法暂时不能返回借用。** trait 里的 `self` 表示「按值接收」，写不出 `&self`，
  所以 `impl` 的接收者只能是 `T`，而按值接收的参数取地址一定是本帧的。等接收者支持
  `&self` 之后，`src/borrow.c` 里的方法解析路径就能直接用。
- **同一条语句里的两次 `&mut` 不查。** `f(&mut x, &mut x)` 目前放行 —— 临时借用不
  登记别名。
- **方法调用不会结束对源对象的借用。** `let r = &v; v.free();` 不会报错，因为
  `v.free()` 不是对 `v` 的直接使用。
- **借用活到作用域结束**，不做 NLL。见上一节。
- **不做自动 drop。** 借用检查只保证「引用不会悬垂」，不负责释放。`Vec` 仍然手工
  `free()`，字符串仍然由 Runtime 统一在进程退出时释放。

## 测试

- [`references.tp`](../tests/run-pass/types/references.tp)：`&x` 传给 `*T` 参数（遗忘借用）。
- [`reference_borrows.tp`](../tests/run-pass/types/reference_borrows.tp)：`&mut` 参数、
  共享借用、结构体字段借用、降级成裸指针。
- [`reference_scopes.tp`](../tests/run-pass/types/reference_scopes.tp)：借用随作用域结束，
  之后可以重新借用。
- [`borrow_return.tp`](../tests/run-pass/types/borrow_return.tp)：返回借用的函数
  （`fn first(v: &Vec<T>): &T`）、借用穿过函数调用、`&param.field`。
- `tests/compile-fail/borrow/` 下每条规则一个用例。

只跑借用相关的用例：

```bash
TEST_FILTER=borrow/ sh tests/run-tests.sh build/tap
TEST_FILTER=types/reference sh tests/run-tests.sh build/tap
TEST_FILTER=types/borrow sh tests/run-tests.sh build/tap
```

`TAP_NO_BORROW=1` 可以跳过整个借用检查，用来确认某段代码到底是被借用检查还是别的
阶段拦下的。
