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

方法接收者也可以写成借用，`impl` 里用 `&self` / `&mut self` 代替「参数名: 类型」：

```text
trait Shape {
    fn area(&self): i32;          // 共享借用接收者
    fn grow(&mut self, d: i32);   // 独占借用接收者
}
```

## 转换规则

| 从 | 到 | 允许 | 说明 |
| --- | --- | --- | --- |
| `&mut T` | `&T` | 是 | 能力收窄：独占降级成共享 |
| `&T` / `&mut T` | `*T` | 是 | **遗忘借用**：放弃检查，之后由调用方负责 |
| `*T` | `&T` / `&mut T` | 否 | 裸指针没有任何承诺，不能凭空升级成受检查的借用 |
| `&T` | `&mut T` | 否 | 共享借用的调用方没承诺独占 |

`&mut T → &T` 在**所有**位置都成立，不只是 `&mut x` 这种字面量：手里已经拿着一个
`&mut T` 变量时（函数参数最常见），一样能当 `&T` 用。

```text
fn read(point: &Point): i32 { return point.x; }

fn use_mut(point: &mut Point): i32 {
    let shared: &Point = point;   // 赋值：可以
    let a: i32 = read(point);     // 传参：可以
    let b: i32 = point.peek();    // 调 &self 方法：可以
    return a + b;
}
```

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

借用藏进结构体字段之后，类型里就没有任何「谁借了谁」的信息（`Holder` 和 `i32`
毫不相干），所以这条规则在**返回结构体 / 数组字面量时逐字段递归检查**：

```text
struct Holder { src: &i32 }

fn make(): Holder {
    let local: i32 = 7;
    return Holder { src: &local };   // error: field 'src' of the returned struct
}                                    //        borrows a value that lives in this function
```

借自引用参数的字段不受限制 —— 那层存储不在本帧，跟着返回没问题：

```text
fn view(pair: &Pair): Holder {
    return Holder { src: &pair.left };   // 合法
}
```

检查只走「值确实由这些字段拼出来」的位置：结构体字段、数组元素。**不进函数调用实参**
（`return wrap(Holder { src: &local })` 里借用能不能逃出去取决于 `wrap` 拿它做什么），
也没有跨语句的追踪 —— 先把结构体存进变量、再返回变量，目前不报（见
[`todo/borrow.md`](todo/borrow.md)）。

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

同一条**语句**里也一样 —— 实参位置的借用不活到语句之外，但彼此之间仍然互斥：

```text
swap(&mut x, &mut x);               // error: 'x' is already mutably borrowed
compare(&x, &mut x);                // error: 'x' is already borrowed
f(&mut x, &mut y);                  // 合法：不同变量
g(&x, &x);                          // 合法：两个共享借用
```

实现上，语句内的临时借用单独记一份标记、语句边界清空，不和「活到作用域结束」的
持久借用混在一起。

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

### 5. 方法调用也是一次借用

调方法时接收者要满足方法声明的借用方式：

| 接收者写法 | 对接收者的要求 |
| --- | --- |
| `&self` | 没有被 `&mut` 借走（共享借用之间可以共存） |
| `&mut self` | 没有被任何借用占着 |
| 按值接收 `self` | 没有被任何借用占着 —— 值被整个拿走，调用方手里的引用会悬垂 |

```text
let held: &Vec<i32> = &values;
values.free();          // error: cannot call method 'free' on 'values'
                        //        while it is borrowed

let w: &i32 = r.width();
r.grow(1);              // error: 'r' is borrowed and cannot be assigned here

let p: Point = Point { x: 1 };
let r: &i32 = p.peek();
p.reset();              // error: 'p' is already borrowed, so it cannot be
                        //        borrowed mutably
```

方法返回借用时，这次借用会登记到接收者身上（`let r = p.peek()` 之后 `p` 就被借走了）。
**只对返回值是引用的方法生效** —— 否则 `let n = vec.len()` 会让后面所有
`vec.data[i] = x` 全部误报。

注意：从**裸指针**绕出去的借用不受这条保护（`&values.data[0]` 里的 `data` 是 `*T`），
那是显式的「遗忘借用」，规则 2 也管不到它。

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
- 方法要读写 `self` 本身、或者要返回借用——接收者写 `&self` / `&mut self`，见下节。
- 结构体要长期保存指针、需要自己管寿命——用 `*T`。
- 和 C 打交道——用 `*T` 或 `c_string`。

## 方法接收者：`&self` / `&mut self`

trait 方法的接收者有三种写法，`&self` 让「方法返回借用」变得可能：

```text
trait Shape {
    fn area(&self): i32;
    fn grow(&mut self, delta: i32);
    fn width(&self): &i32;
}

impl Shape for Rect {
    fn area(&self): i32 { return self.width * self.height; }
    fn grow(&mut self, delta: i32) { self.width = self.width + delta; }
    fn width(&self): &i32 { return &self.width; }   // 借自 self
}
```

实现上 `&self` 被脱糖成「名字是 `self`、类型是 `&T`」的普通参数，所以：

- 方法体里的 `self.x` 和引用变量的字段访问走同一条路径，没有特例；
- `self` 是**引用型参数**，`pointee_rank` 是 -1（指向函数外），所以 `&self.width`
  可以原样返回，而 `&local` 不行；
- `&self` 方法里写 `self.x` 会报 `cannot write through a shared reference 'self'`；
- 规则 4 对方法同样适用：`fn get(&self): &T` 里接收者就是那「唯一的一个引用参数」。

方法调用同时也是对接收者的一次借用，所以下面两行都会报错：

```text
let w: &i32 = r.width();
r.grow(1);              // error: 'r' is borrowed and cannot be assigned here

let p: Point = Point { x: 1 };
let r: &i32 = p.peek();
p.reset();              // error: 'p' is already borrowed, so it cannot be borrowed mutably
```

## 诊断：一次报完

借用检查是**报完继续查**的，一个文件里的多处借用错误会一次全报：

```tap
fn main(): i32 {
    let x: i32 = 1;
    let a = &mut x;
    let b = &mut x;   // error: 'x' is already mutably borrowed
    let c = &x;       // error: 'x' is mutably borrowed, so it cannot be borrowed again
    x = 2;            // error: 'x' is mutably borrowed and cannot be assigned here
    return 0;
}
```

三条诊断、各带位置，退出码 1。报够 10 条就停（`error: too many errors (10), stopping
now`），免得一个系统性错误刷屏。

能做到这一点是因为借用检查是**纯 AST 走查**：不生成 IR、不碰 LLVM，报错之后继续走最坏
只是多几条诊断。Codegen 那边要麻烦得多（它一边检查一边生成 IR，报完继续会拿着不一致的
类型去调 `LLVMBuild*`），做法是让报错的语句自己收尾成合法 IR，见
[`src.md`](src.md#类型错误的语句级隔离codegen)。

两条实现上的注意点（改这里容易踩）：

- **一条语句只报一条。** `check_write_target` 里「被独占借用」「被共享借用」两个分支
  报完要 `return`，否则 `&x` 和 `&mut x` 同时挂着时同一行会出两条。
- **`let r = &mut x;` 的冲突检查只做一次。** 遍历表达式时已经检查过（位置是 `&mut x`
  本身，比整条语句精确），绑定之后再检查一遍就会为同一个错误报两条 —— 现在后者只调
  `mark_persistent_borrow` 登记状态、不报诊断。

报过错之后 `check_borrows` 返回 1，`main.c` 直接返回，**不进 Codegen** —— 有错时的 AST
状态不再可信。

## 当前限制（L1 的边界）

这些是**明确没做**的，不是 bug：

- **引用参数最多一个**才能返回借用。需要「借自哪个参数」更细粒度时得等生命周期参数。
- **结构体存借用：只能检查「从函数里带出去」这一种情况，寿命本身仍然不跟踪。**
  `struct Holder { src: &i32 }` 是合法的（引用在机器码里就是指针），返回时逐字段检查
  挡住最常见、也最危险的那种悬垂：

  ```text
  struct Holder { src: &i32 }

  fn make(): Holder {
      let local: i32 = 7;
      return Holder { src: &local };   // error: field 'src' of the returned struct
  }                                    //        borrows a value that lives in this function
  ```

  但类型里没有寿命参数，所以「结构体活得比源长」的其他路径仍然漏：借用先装进变量
  再返回（`let h = Holder { src: &local }; return h;`）、存进 `Vec`、跨函数传递。
  要做对需要生命周期参数（L2），代价是单态化的 key 从「类型」变成「类型 + 区域」。
  在那之前，**别让结构体长期持有借用**，需要带出函数时用 `*T` 并自己管寿命。
- **接收者只能是 `&self` / `&mut self` 这一层。** `&&self`、按值 + 借用的组合都不支持；
  方法签名里也不能出现生命周期标注。
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
- [`reference_narrowing.tp`](../tests/run-pass/types/reference_narrowing.tp)：
  `&mut T` 当 `&T` 用的三条路径（赋值、传参、调 `&self` 方法）。
- [`struct_borrow.tp`](../tests/run-pass/types/struct_borrow.tp)：结构体保存借用
  （借自引用参数可以返回、函数内临时使用、降级成裸指针带出去）。
- [`self_reference.tp`](../tests/run-pass/traits/self_reference.tp)：`&self` / `&mut self`
  接收者、方法返回借用。
- [`self_by_value.tp`](../tests/run-pass/traits/self_by_value.tp)：impl 里裸 `self`
  （按值接收者，改的是副本）。
- [`self_reference_generic.tp`](../tests/run-pass/traits/self_reference_generic.tp)：
  泛型约束下的 `&self` 调用、引用接收者、方法互相调用。
- `tests/compile-fail/borrow/` 和 `tests/compile-fail/traits/` 下每条规则一个用例；
  [`multiple_errors.tp`](../tests/compile-fail/borrow/multiple_errors.tp) 断言三处错误
  一次全报，[`error_limit.tp`](../tests/compile-fail/borrow/error_limit.tp) 断言第 10 条
  之后停下，[`return_nested_struct_holding_local_borrow.tp`](../tests/compile-fail/borrow/return_nested_struct_holding_local_borrow.tp)
  断言结构体字面量的字段检查会按层递归、也能穿过返回引用的调用。

只跑借用相关的用例：

```bash
TEST_FILTER=borrow/ sh tests/run-tests.sh build/tap
TEST_FILTER=types/reference sh tests/run-tests.sh build/tap
TEST_FILTER=types/borrow sh tests/run-tests.sh build/tap
TEST_FILTER=traits/self sh tests/run-tests.sh build/tap
```

`TAP_NO_BORROW=1` 可以跳过整个借用检查，用来确认某段代码到底是被借用检查还是别的
阶段拦下的。
