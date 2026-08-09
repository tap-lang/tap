# 模块导入

4yue 使用 `import` 导入其他 `.tp` 源文件。每个导入在当前文件中建立名称空间，
模块由编译器在代码生成前解析并合并。

## 语法

模块名由点分隔的路径段组成，导入声明以分号结束：

```text
import std.math;
import modules.helpers as helper;

fn main(): i32 {
    return math.square(5);
}
```

模块路径段允许使用标识符形式的关键字，例如 `import std.string;`。
模块名中的 `.` 会转换为目录分隔符，并自动追加 `.tp`：

```text
std.math       -> std/math.tp
modules.helper -> modules/helper.tp
```

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

未限定的导入函数调用会报错；Prelude 函数和当前文件函数仍使用普通函数名调用。

## 路径查找

普通模块按以下顺序查找：

1. 导入声明所在文件的目录
2. `4YUE_MODULE_PATH`
3. 当前工作目录
4. 编译器可执行文件相邻的源码目录
5. 安装前缀下的 `share/4yue`

`4YUE_MODULE_PATH` 当前只接受一个模块根目录。例如：

```bash
4YUE_MODULE_PATH=/path/to/project ./build/4yue run app/main.tp
```

对于 `std.*` 模块，编译器会先尝试 `4YUE_STD_PATH`，并去掉模块名开头的
`std.`。例如 `4YUE_STD_PATH=/path/to/std` 时，`import std.math;` 会查找
`/path/to/std/math.tp`。

CMake 安装会把 `std` 下的 `.tp` 文件复制到 `share/4yue/std`，因此安装后的
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

## 错误诊断

找不到模块时，错误指向导入声明中的模块名：

```text
tests/test_module_missing.tp:1:8: 错误: 找不到模块 'modules.missing'
```

两个模块使用同一名称空间时，错误包含两条导入的位置：

```text
tests/test_module_alias_conflict.tp:2:8: 错误: 名称空间 'util' 已用于模块 'modules.math'
tests/test_module_alias_conflict.tp:1:8: 提示: 名称空间首次在此导入
```

省略名称空间调用且当前文件、Prelude 中均不存在该函数时，会按普通未定义函数报错：

```text
错误：未定义的函数 'double'
```

## 调试与测试

`-parse` 只打印指定文件中的 `ImportNode`，不会展开模块内容：

```bash
./build/4yue -parse tests/test_modules.tp
```

普通编译、`run`、`-ir` 和 `-run-lli` 都会实际加载模块。项目中的模块回归用例包括：

- [`test_modules.tp`](../tests/test_modules.tp)：默认名称空间、`as`、多别名、递归导入和标准库模块
- [`test_module_cycle.tp`](../tests/test_module_cycle.tp)：循环导入
- [`test_module_missing.tp`](../tests/test_module_missing.tp)：缺失模块诊断
- [`test_module_namespace.tp`](../tests/test_module_namespace.tp)：本地函数与模块函数同名隔离
- [`test_module_alias_conflict.tp`](../tests/test_module_alias_conflict.tp)：名称空间别名冲突
- [`test_module_unqualified.tp`](../tests/test_module_unqualified.tp)：未限定导入函数调用诊断
- [`test_module_unknown_namespace.tp`](../tests/test_module_unknown_namespace.tp)：未导入名称空间诊断
- [`test_module_missing_export.tp`](../tests/test_module_missing_export.tp)：模块导出函数不存在诊断
- [`test_module_self_import.tp`](../tests/test_module_self_import.tp)：入口文件自导入诊断

运行全部测试：

```bash
make test
```

## 当前限制

- 模块只导出顶层函数，没有可见性控制。
- 不支持选择性导入和模块再导出。
- 名称空间目前只能用于限定函数调用，不能作为值传递。
- 入口文件不能把自身再次作为模块导入。
- `4YUE_MODULE_PATH` 和 `4YUE_STD_PATH` 均只接受单个目录。
- 模块文件扩展名固定为 `.tp`。
