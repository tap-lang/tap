# 控制流

## if / else

条件必须写在圆括号里，分支体是代码块：

```text
if (value < 0) {
    print("negative\n");
} else if (value == 0) {
    print("zero\n");
} else {
    print("positive\n");
}
```

- `else if` 与 `elseif` 是**等价写法**，生成的 AST 完全相同，可以在同一条链上混用。
- 链尾可以没有 `else`，此时条件全为假就什么都不执行。
- `if` 是**语句**，不是表达式——没有 `let x = if (c) { 1 } else { 2 };` 这种写法。

## while

```text
let i: i32 = 0;
while (i < 5) {
    i++;
}
```

## for

```text
for (let i: i32 = 0; i < 10; i++) {
    print("%d\n", i);
}
```

循环头是三段式，由分号分隔：

- **初始化**：变量声明（`let i: i32 = 0`）或对已有变量的赋值（`count = 0`）
- **条件**：可选，省略即恒真
- **更新**：赋值、`++`、`--`

三个部分都可以为空，所以 `for (;;) { ... }` 是合法的无限循环。

## break / continue

`break;` 结束当前循环，`continue;` 执行更新表达式后进入下一轮。嵌套循环中两者
**只作用于最内层循环**。

## 块作用域

`if` 分支、`while` / `for` 循环体、`match` 分支各自引入一个块作用域：

- 块内 `let` 声明的变量出了块就不可见
- 块内声明可以遮蔽同名的外层变量，块结束后外层变量恢复原值
- `for` 的循环变量（`for (let i: ...)`）只在循环体内可见，循环结束后外层同名变量不变
- `match` 的载荷绑定同理，只在对应分支内有效

```text
let index: i32 = 99;
for (let index: i32 = 0; index < 3; index++) {
    // 这里的 index 是循环变量
}
print("%d\n", index);   // 99，不是 3
```
