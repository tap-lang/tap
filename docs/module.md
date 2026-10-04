# 模块导入

tap 使用 `import` 导入其他 `.tp` 源文件。每个导入在当前文件中建立名称空间，
模块由编译器在代码生成前解析并合并。

## 语法

模块名由点分隔的路径段组成，导入声明以分号结束：

```text
import std.math;                 // 名称空间导入，按 math.square(5) 访问
import modules.helpers as helper; // 名称空间导入并改名
import std.math.*;               // 通配导入，pub 成员按裸名访问
import std.math.square;          // 单成员导入，按裸名 square(...) 访问
import std.math.square as sq;    // 单成员导入并改名，按 sq(...) 访问

fn main(): i32 {
    return math.square(5);
}
```

四种写法的详细语义见[通配导入与单成员导入](#通配导入与单成员导入)。

模块路径段允许使用标识符形式的关键字，例如 `import std.string;`。
模块名中的 `.` 会转换为目录分隔符，并自动追加 `.tp`：

```text
std.math       -> std/math.tp
modules.helper -> modules/helper.tp
```

## 可见性：pub

顶层声明默认是**模块私有**的，只有加了 `pub` 才能被其他模块导入：

```text
// std/math.tp
pub const PI: f64 = 3.141592653589793;
pub fn square<T>(value: T): T { ... }
```

```text
// main.tp
import std.math;

fn main(): i32 {
    return math.square(5);
}
```

`pub` 可以修饰函数、`extern fn`、常量和类型（`struct` / `enum`），只能出现在顶层。
访问没有 `pub` 的符号会被明确拦下，而不是报「找不到」：

```text
error: 'priv_fn' is private to module 'std.net'
```

### 类型的可见性

- **`pub` 类型保持裸名**：其他模块直接写类型名，不需要也不能写 `模块名.类型名`。
- **非 `pub` 类型会改名成模块私有符号**，模块外部完全看不到。

所以两个模块可以各自拥有同名的私有类型、互不干扰；而 `pub` 类型是全局共享的，
两个模块不能定义同名的 `pub` 结构体或枚举。

```text
// modules/a.tp
pub struct Point { x: i32 }      // 外部可以直接写 Point
struct Scratch { value: i32 }    // 只有 a 自己看得到

// modules/b.tp
struct Scratch { value: i32 }    // 和 a 的 Scratch 不冲突
```

### 实现约定

- 非 `pub` 的顶层**函数和常量**同样会改名成 `__tap_module_N.name`。这样它们既不导出、
  也不占用全局名字，其他模块不会因为重名而冲突。
- `extern fn` 的符号名就是 C 符号名，加前缀会链接不到，所以它保持原名。代价是
  **模块里的 `extern` 声明会占住那个全局名字**：导入 `std.net` 之后就不能再声明自己的
  `write`，即使签名不同。要把它暴露出去，写 `pub extern fn`。

## 名称空间与别名

默认名称空间是模块名的最后一段：

```text
import std.math;

fn main(): i32 {
    return math.square(5);
}
```

使用 `as` 可以为模块指定其他名称空间：

```text
import std.math as m;

fn main(): i32 {
    return m.square(5);
}
```

名称空间只在声明该 `import` 的文件中有效。模块自己的导入不会自动暴露给导入者，
模块内部调用依赖时也必须使用该模块文件中声明的名称空间。

同一模块可以使用多个别名导入，所有别名仍指向同一份编译结果：

```text
import modules.math;
import modules.math as calc;

fn main(): i32 {
    return math.double(2) + calc.double(3);
}
```

不同模块不能在同一文件中使用相同名称空间。入口文件的本地函数与模块导出函数可以同名：

```text
import modules.conflict;

fn conflict(): i32 {
    return 2;
}

fn main(): i32 {
    return conflict.conflict() + conflict();
}
```

## 通配导入与单成员导入

除了整模块的名称空间导入，还可以把模块成员直接引入当前文件，**调用时不用模块前缀**：

```text
import std.math.*;             // 通配导入：模块全部 pub 成员
import std.math.square;        // 单成员导入：只引入 square
import std.math.square as sq;  // 单成员导入并改名

fn main(): i32 {
    return square(3) + sq(4);  // 裸名调用，和本地函数一样
}
```

- **通配导入 `import mod.*`**：把模块所有 `pub` 成员引入当前文件，按裸名访问。没有 `pub`
  的成员不会进来（裸名调用会按普通未定义函数报错）。
- **单成员导入 `import mod.member`**：只引入一个 `pub` 成员，按成员名（裸名）访问。
- **单成员导入 + 别名 `import mod.member as name`**：以 `name` 绑定该成员。别名只影响
  本地名字，模块内部仍叫 `member`。

`member` 必须是 `pub` 的：非 `pub` 成员在导入处就报 `'x' is private to module 'y'`，
不存在则报 `module 'y' has no member 'x'`。

裸名必须唯一：通配/单成员导入引入的裸名不能和本文件（或本模块）自己的顶层函数、常量、
结构体、枚举重名，两条导入之间也不能引入同一个裸名 —— 重名一律**编译报错**，不静默偏向
任何一侧：

```text
error: import from module 'modules.math' brings in 'double', which is already defined or imported in this file
--> tests/compile-fail/modules/wildcard_local_conflict.tp:3:8
note: rename one of them, or import the module as a namespace and qualify the call
```

所以 `import modules.math.*` 的同时定义本地 `fn double` 会报错（而不是「本地定义优先」）；
两条通配导入都带 `double` 同样报错（而不是按导入顺序取一个）。名称空间导入不引入裸名，
`import modules.math;` 之后裸调用 `double(...)` 仍然按未定义函数报错。

三种形式可以混用，同一模块也可以既做名称空间导入又做成员导入：

```text
import std.math;         // 按 math.square(3) 访问
import std.math.square;  // 按 square(4) 访问
```

### 成员段与模块路径的区分

`import std.math.square` 里的 `square` 究竟是模块路径的一段还是成员名，语法上无法区分
（两者都是标识符）。编译器按文件系统解析：

- 整条点分路径能解析成模块 → 当作整模块导入（`import std.math` 就是模块 `std.math`）。
- 否则去掉最后一段、前缀能解析成模块 → 最后一段当成员名（`import std.math.square` →
  模块 `std.math` 的成员 `square`）。
- **两者都能解析成模块 → 报歧义错误**，不静默偏向任何一侧。

```text
error: ambiguous import 'modules.ambig.inner': both module 'modules.ambig.inner' and member 'inner' of module 'modules.ambig' exist
--> tests/compile-fail/modules/ambiguous_member.tp:3:8
note: import the namespace form 'import modules.ambig;' and qualify the call, or rename one of them
```

也就是说，成员名和某个模块文件重名时（例如模块 `std.math` 有 pub 成员 `square`，同时又存在
模块文件 `std/math/square.tp`），`import std.math.square;` 会**编译报错**。消歧办法是改用
名称空间导入（`import std.math;` 后写 `math.square(...)`），或者给其中一个改名 —— 注意这种
重名下那个同名**模块**没有语法可以导入，只能改名。

## 名称空间使用说明

名称空间导入的 `pub` 常量和函数按 `别名.名字` 访问；`pub` 类型保持裸名直接写。没有
`pub` 的符号是模块私有的，外部看不到。Prelude 函数和当前文件函数仍使用普通函数名调用；
名称空间导入不引入裸名，所以未限定的 `math.square(...)` 之外的裸调用会按未定义函数报错
（要裸名调用请用上一节的通配/单成员导入）。

顶层常量在模块外通过名称空间访问（`net.AF_INET`），在声明它的模块内部可以直接写裸名
（`AF_INET`）——导出时声明会被改写成内部唯一符号，模块内的引用也跟着改写。裸名改写只
匹配本模块导出的**常量**，函数名和类型名不在其中，所以 `with_capacity(capacity: uint)`
里的参数 `capacity` 不会和同名导出函数混淆。反过来说，如果某个局部变量或参数和本模块的
导出常量重名，它会被当成常量；常量用大写命名就不会撞车。
当函数第一个参数类型与结构体变量类型一致时，也可以使用 `value.method(...)` 形式调用；
语句形式下如果函数返回同一个结构体类型，返回值会自动写回接收者。

## 路径查找

普通模块按以下顺序查找：

1. 导入声明所在文件的目录
2. `TAP_MODULE_PATH`
3. 当前工作目录
4. 编译器可执行文件相邻的源码目录
5. 安装前缀下的 `share/tap`

`TAP_MODULE_PATH` 当前只接受一个模块根目录。例如：

```bash
env TAP_MODULE_PATH=/path/to/project ./build/tap run app/main.tp
```

对于 `std.*` 模块，编译器会先尝试 `TAP_STD_PATH`，并去掉模块名开头的
`std.`。例如 `TAP_STD_PATH=/path/to/std` 时，`import std.math;` 会查找
`/path/to/std/math.tp`。

CMake 安装会把 `std` 下的 `.tp` 文件复制到 `share/tap/std`，因此安装后的
编译器不依赖源码仓库也能导入标准库模块。

## 加载流程

```text
入口源码
  -> Parser 生成 ImportNode
  -> Module Loader 解析模块路径
  -> 递归解析模块及其依赖
  -> 解析名称空间并改写为唯一内部符号
  -> 合并函数链表
  -> 加载 Prelude
  -> 泛型单态化
  -> LLVM Codegen
```

Module Loader 使用规范化绝对路径标识模块，并在解析模块前记录该路径。因此：

- 同一文件被多次导入时只加载一次。
- 不同相对路径指向同一文件时只加载一次。
- 循环导入不会无限递归。
- 递归导入中的函数最终只生成一份 LLVM 定义。
- 同一模块使用多个别名时不会复制函数定义。

模块文件使用与入口文件相同的 Lexer、Parser 和 AST。Codegen 不直接处理
`ImportNode` 或源代码中的名称空间，只接收完成名称解析和合并后的
`ProgramNode.functions`。内部函数名采用源语言无法声明的唯一名称，避免不同模块的
同名函数发生冲突。

模块导出的泛型函数与普通函数使用相同的名称空间规则。调用名称解析完成后，泛型单态化会根据
调用处的具体类型生成内部函数实例；未被调用的泛型模板不会进入 LLVM Codegen。

## 错误诊断

找不到模块时，错误指向导入声明中的模块名：

```text
error: module 'modules.missing' not found
--> tests/compile-fail/modules/missing.tp:1:8
```

两个模块使用同一名称空间时，错误包含两条导入的位置：

```text
error: namespace 'util' is already used for module 'modules.math'
--> tests/compile-fail/modules/alias_conflict.tp:2:8
note: namespace was first imported here
--> tests/compile-fail/modules/alias_conflict.tp:1:8
```

省略名称空间调用且当前文件、Prelude 中均不存在该函数时，会按普通未定义函数报错：

```text
error: undefined function 'double'
--> tests/compile-fail/modules/unqualified_call.tp:4:12
```

通配/单成员导入引入的裸名与本文件顶层定义（或其他导入）重名时报错，不静默偏向：

```text
error: import from module 'modules.math' brings in 'double', which is already defined or imported in this file
--> tests/compile-fail/modules/wildcard_local_conflict.tp:3:8
note: rename one of them, or import the module as a namespace and qualify the call
```

## 调试与测试

`-parse` 只打印指定文件中的 `ImportNode`，不会展开模块内容：

```bash
./build/tap -parse tests/run-pass/modules/imports.tp
```

普通编译、`run`、`-ir` 和 `-run-lli` 都会实际加载模块。项目中的模块回归用例包括：

- [`imports.tp`](../tests/run-pass/modules/imports.tp)：默认名称空间、`as`、多别名、递归导入和标准库模块
- [`wildcard_import.tp`](../tests/run-pass/modules/wildcard_import.tp)：通配导入 `import mod.*`
- [`member_import.tp`](../tests/run-pass/modules/member_import.tp)：单成员导入 `import mod.member`
- [`member_alias_import.tp`](../tests/run-pass/modules/member_alias_import.tp)：单成员导入 + 别名
- [`wildcard_local_conflict.tp`](../tests/compile-fail/modules/wildcard_local_conflict.tp)：通配导入与本地定义重名
- [`member_local_conflict.tp`](../tests/compile-fail/modules/member_local_conflict.tp)：单成员导入与本地定义重名
- [`wildcard_import_conflict.tp`](../tests/compile-fail/modules/wildcard_import_conflict.tp)：两条通配导入引入同名裸名
- [`cycle.tp`](../tests/run-pass/modules/cycle.tp)：循环导入
- [`missing.tp`](../tests/compile-fail/modules/missing.tp)：缺失模块诊断
- [`member_missing.tp`](../tests/compile-fail/modules/member_missing.tp)：单成员导入的成员不存在诊断
- [`member_private.tp`](../tests/compile-fail/modules/member_private.tp)：单成员导入私有成员诊断
- [`wildcard_private.tp`](../tests/compile-fail/modules/wildcard_private.tp)：通配导入不带私有成员
- [`ambiguous_member.tp`](../tests/compile-fail/modules/ambiguous_member.tp)：成员名与模块文件重名的歧义诊断
- [`ambiguous_disambiguated.tp`](../tests/run-pass/modules/ambiguous_disambiguated.tp)：重名时改用名称空间导入消歧
- [`namespace.tp`](../tests/run-pass/modules/namespace.tp)：本地函数与模块函数同名隔离
- [`alias_conflict.tp`](../tests/compile-fail/modules/alias_conflict.tp)：名称空间别名冲突
- [`unqualified_call.tp`](../tests/compile-fail/modules/unqualified_call.tp)：未限定导入函数调用诊断
- [`unknown_namespace.tp`](../tests/compile-fail/modules/unknown_namespace.tp)：未导入名称空间诊断
- [`missing_export.tp`](../tests/compile-fail/modules/missing_export.tp)：模块导出函数不存在诊断
- [`self_import.tp`](../tests/compile-fail/modules/self_import.tp)：入口文件自导入诊断

完整的测试目录结构、用例格式和运行方式见[测试文档](test.md)。

## 当前限制

**命名空间与可见性**

- `pub` 的顶层函数和常量按 `别名.名字` 访问；`pub` 类型保持**裸名**（不支持 `模块名.类型名`），
  所以两个模块不能定义同名的 `pub` 结构体或枚举。
- 没有 `pub` 的顶层函数和常量会改名成 `__tap_module_N.name`，既不导出、也不占全局名字，
  模块之间不会因为重名而冲突。
- `extern fn` 是例外：符号名必须和 C 符号一致，所以它保持原名，**会占住那个全局名字**。
  导入 `std.net` 之后就不能再声明自己的 `write`，即使签名不同。要把它暴露给其他模块，
  写 `pub extern fn`。
- 入口文件不是模块，它的顶层符号不受 `pub` 影响。
- 模块内引用自己导出的常量按裸名改写，所以局部变量和参数不要和导出常量重名（常量用大写命名可避开）。
- 顶层常量只能用字面量初始化，不能写成引用其他模块符号的表达式。
- 支持名称空间、通配（`mod.*`）和单成员（`mod.member [as name]`）三种导入；不支持模块再导出
  （模块不能把导入的符号转出去）。
- 成员名与模块文件重名时（`import a.b.c` 里 `a/b/c.tp` 和 `a/b.tp` 同时存在）报歧义错误，
  不静默偏向；这种重名下同名的那个**模块**没有导入语法，只能改名。
- 裸名重名检查只覆盖「本文件顶层定义 + 其他通配/单成员导入」。Prelude（`print` / `exit` /
  `panic` 等）是模块加载完之后才注入的，不在检查范围内：模块若有一个和 Prelude 同名的 `pub`
  成员并被通配/单成员导入，裸名会**静默指向模块成员**而不是 Prelude。
- 名称空间目前只能用于限定函数调用，不能作为值传递。
- 入口文件不能把自身再次作为模块导入。

**诊断**

- 模块符号被改名成 `__tap_module_N.name` 之后，部分诊断会把这个内部符号名直接打给用户，
  例如 `error: function '__tap_module_0.f' expects 1 arguments, but got 2`。非 `pub` 符号
  现在一律改名，所以这条更容易撞上。
- 两个模块定义同名的 `pub` 类型时报 `duplicate struct 'P'`，不带位置信息；
  `struct expression type mismatch` 同样没有位置信息（后者是通用问题，非模块特有）。
- 访问模块私有的**常量**只报 `error: 'X' is private to module 'Y'`，没有位置信息；
  访问私有的**函数**带位置。
- 入口文件引用了某个模块的传递依赖时，只报 `namespace 'b' was not imported`，不提示
  `b` 其实是 `a` 的依赖。

**其他**

- `TAP_MODULE_PATH` 和 `TAP_STD_PATH` 均只接受单个目录。
- 模块文件后缀接受 `.tp` 和 `.tap`，解析顺序是先 `.tp` 再 `.tap`；同一目录下两者同名时
  以 `.tp` 为准。
