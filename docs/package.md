# 包管理

这份文档是 tap 包管理的设计规范。

**状态：阶段 1 全部实现（含 `tapm`），可以端到端跑通。**

## 结论

tap 的 `import` 是路径式的（`import a.b` 解析到 `a/b.tp`），所以**「包」天然就是一棵目录树**。
要补的不是模块系统，而是两件事：**找得到**（搜索路径）和**声明得了**（清单）。

分工：

| 命令 | 职责 | 用什么写 |
|---|---|---|
| `tap` | 编译器 + **构建**（`tap build` / `tap run`） | C |
| `tapm` | **依赖管理**（填充 `packages/`、维护锁文件） | tap |

这是 dune / opam 那套分工，不是 cargo 那套。**关键好处：`tap build` 完全独立于 `tapm`**，
`tapm` 本身有 bug 也不影响用户构建自己的项目。

## 目录布局

```
myapp/
├─ package.toml                 name = "myapp", entry = "src/main.tp"
├─ src/                         本包源码
│   ├─ main.tp
│   └─ util.tp
├─ packages/                    依赖（tapm 负责填充）
│   ├─ foo/
│   │   ├─ package.toml         name = "foo"
│   │   └─ src/
│   │       ├─ lib.tp                import foo.lib
│   │       └─ internal/util.tp      import foo.internal.util
│   └─ bar/
│       ├─ package.toml         name = "bar"
│       └─ src/
└─ build/                       构建产物
```

`packages/` 是**单层平铺**的：每个依赖直接是 `packages/` 下的一个目录，不嵌套。

里面的目录有两种来源：

- **远程依赖**：`tapm` 下载到 `packages/<name>/`
- **path 依赖**：`tapm` **符号链接** `packages/<name>` → 清单里写的路径（见下节）

两种都是「按 `name` 命名目录」，编译器不需要知道区别，只按目录名解析。

### 哪些进版本控制

```gitignore
packages/
build/
```

**`packages/` 不进版本控制**——它是 `tapm install` 生成的。代价是换台机器必须重跑一次
`tapm install`，所以**锁文件必须提交**（见下节），否则没法复现。

## package.toml

```toml
[package]
name = "myapp"
version = "0.1.0"
entry = "src/main.tp"          # 相对 package.toml 所在目录

[dependencies]
bar = { path = "../bar" }      # 本地路径依赖
foo = "1.0.0"                  # 精确版本，来自注册表（阶段 3）
```

- `entry` 只在 `tap` 编译**当前包**时使用。
- 依赖包自己的 `package.toml` 里也可以写 `entry`，但作为依赖被导入时用不上。

### 编译器怎么读 `entry`

**只做极简扫描**，不引入通用 TOML 解析器：

- 只在 `[package]` 段内查找 `entry`
- 只接受 `entry = "双引号字符串"` 这一种写法
- 注释、其他键、空行一律跳过
- 找不到时给出明确诊断，而不是静默回退

**只在没给文件参数时才走这条路**（即 `tap build` / `tap run` 无参数）。
`tap run main.tp` 的现有行为完全不变。

## 锁文件：package.lock.toml

格式参考 Cargo.lock，**由 `tapm` 生成，不要手工编辑**：

```toml
# 自动生成，请勿手工修改
version = 1

[[package]]
name = "myapp"
version = "0.1.0"
dependencies = ["bar", "foo"]

[[package]]
name = "bar"
version = "0.1.0"
source = "path+../bar"

[[package]]
name = "foo"
version = "1.0.0"
source = "registry+https://example.com/index"
checksum = "..."
```

要点：

- **必须提交进版本控制。** `packages/` 被 gitignore 了，锁文件是唯一的可复现来源。
- `tapm install` **按锁文件重建 `packages/`，不重新求解**；要重新求解走 `tapm update`。
- 锁文件里的 `name` 决定 `packages/<name>/` 的目录名——这是「甲方案」的契约，
  `tapm` 必须保证目录名和 `name` 一致。
- path 依赖记 `source = "path+../bar"`，不记 checksum。

## 模块解析规则

这是整个设计的核心。以 `myapp/src/main.tp` 里的 `import foo.lib` 为例：

1. 依次遍历搜索根（顺序见下）
2. **在每个搜索根里**：
   - 先按原路径试 → `<root>/foo/lib.tp`
   - 再按包边界试 → 若 `<root>/foo/package.toml` 存在，则源码根是
     `<root>/foo/src/`，解析 `<root>/foo/src/lib.tp`
   - 两条都命中 → **报歧义错**（和现有「成员名与模块文件重名」的处理一致）
   - 都不命中 → 换下一个搜索根

搜索根顺序（沿用现有，新增一条）：

1. 导入声明所在文件的目录
2. `TAP_MODULE_PATH`（**改为支持多目录**，Unix 用 `:`，Windows 用 `;`）
   —— 注意 `TAP_STD_PATH` **不跟着改**，保持单目录：标准库天然只有一个根，
   而模块根有多个（项目 + 依赖）
3. `<包根>/packages` ← **新增**，包根 = 入口文件所在目录向上找到 `package.toml` 的那一层
4. 当前工作目录
5. 编译器可执行文件相邻的源码目录
6. 安装前缀下的 `share/tap`

**包边界只对第一段判定**：`foo` 是包名，`internal/util` 不再做边界探测。这样每层不会都去
探测 `package.toml`。

### 为什么需要隐式的 `<包根>/packages`

`tap build` 是独立命令，不能依赖 `tapm` 先设好环境变量。如果 `packages/` 要靠环境变量传给
编译器，`tapm` 就成了必需前置，上面说的「独立性」就没了。所以编译器自己认这个目录。

## 编译器改动面

改动集中在 `src/module.c` 和 CLI，**不动 codegen、类型系统、泛型**：

| 改动 | 位置 |
|---|---|
| 包边界识别（`package.toml` → `src/` 源码根） | `module.c` 的 `resolve_module_path_with_extension` |
| `TAP_MODULE_PATH` 支持多目录 | `module.c`（现只有一处读它） |
| 隐式搜索根 `<包根>/packages` | `module.c` |
| `[package] entry` 极简扫描 | 新增，小函数 |
| `tap build` 子命令；`tap run` 无参走包模式 | `src/main.c` |
| 构建产物写到 `./build/<name>` | `src/run.c` |

## 命令面

### `tap`（编译器 + 构建）

```text
tap run                 # 包模式：读 entry，编译到 ./build/<name>，然后运行
tap run main.tp         # 文件模式：现有行为，不变
tap build               # 包模式：只编译不运行
tap <file> -o <out>     # 现有行为，不变
tap -ir / -parse / -lex # 现有行为，不变
```

**无参数才走包模式。** 给了文件就是文件级，用户不会困惑，现有用法零影响。

**不支持 `tap build <file>`**：构建是包级概念，文件级编译用 `tap <file> -o <out>`。

### `tapm`（依赖管理，tap 写）

```text
tapm install            # 按 package.toml + 锁文件填充 packages/
tapm add <包名>         # 加依赖并写回 package.toml
tapm remove <包名>      # 去依赖
tapm update             # 刷新锁文件
```

阶段 1 只有 path 依赖，`tapm` 的工作是：读清单 → 展开依赖 → **按 `name` 在 `packages/`
下建目录** → 平铺传递依赖。

path 依赖用**符号链接**接进 `packages/<name>/`，不是复制。这样本地改动实时生效——这正是用
path 依赖的理由。不支持符号链接的平台（Windows 需要开发者模式）回退到复制，并明确提示
「本地改动不会自动生效，需要重跑 `tapm install`」。

**为什么必须落到 `packages/` 而不是靠 `TAP_MODULE_PATH`**：`tap build` 要独立于 `tapm`，
不能依赖 `tapm` 设环境变量。编译器只认隐式根 `<包根>/packages`，所以 path 依赖必须
出现在那里。

多目录 `TAP_MODULE_PATH` 因此**不再承担 path 依赖的职责**，退化成通用能力：给
`tap run file.tp` 加额外模块根、给测试用。

**源码放在独立仓库**，和 tap 仓库同级（`tap-lang/tapm/`），入口 `src/main.tp`。
它和编译器没有运行时耦合——不调用 `tap`，只操作 `packages/` 和锁文件。
发布时用 `tap` 编译成二进制，装到 `PATH` 里即可。

## 分阶段计划

**阶段 1：本地布局 + 解析规则**（编译器改动都集中在这一步）

1. ✅ 包边界识别（`package.toml` → `src/`）
2. ✅ 多目录 `TAP_MODULE_PATH`（Unix `:`、Windows `;`）
3. ✅ 隐式搜索根 `<包根>/packages`
4. ✅ `[package] entry` 扫描 + `tap build` + `tap run` 包模式
5. ✅ `tapm`：读清单、展开 path 依赖、按 `name` 建 `packages/<name>/`（符号链接）
   —— 源码在**独立仓库** `tap-lang/tapm/`（和 tap 同级），入口 `src/main.tp`；
   它自己也遵守这套约定（`package.toml` + `src/`），所以能 `tap build`、
   也能被别人 `import tapm.toml` 按包边界导入。
   命令 `install` / `list` / `help`。
   **已知未完成**：锁文件的 `version` 字段是空的（读依赖版本的代码会段错误，
   见 `tapm/install.tp` 里的 TODO）；`add` / `remove` / `update` 还没做；
   非 path 依赖（版本号）不支持；符号链接失败时没有回退到复制。

### 回归覆盖的边界

第 1-3 步有回归用例（`tests/run-pass/modules/package_boundary.tp` 和
`multi_module_path.tp`，fixture 在 `tests/fixtures/foo/` 和 `tests/fixtures-extra/`）。

**第 3、4 步的隐式包根和包模式命令不在回归覆盖内**——测试框架是按「扫 `tests/` 下的
`.tp` 文件逐个编译运行」组织的，而包模式要求「不给文件、靠 `package.toml` 定位」，
两者不兼容；而且 fixture 模块没有 `main`，放进被扫描的目录会被当成用例而失败。
这两条目前靠手工验证，以后要补的话得给测试框架加一类「目录级」用例。

**阶段 2：版本与锁**

6. `package.lock` 记录精确来源
7. git URL 依赖

**阶段 3：注册表**

8. 中心 registry + tarball + 内容哈希

**独立线（不阻塞阶段 1-2，但是第三方生态的前提）**

9. `pub` 类型作用域化 —— 见下节

## 阶段 1 验收标准

- `myapp/src/main.tp` 里 `import foo.lib`，`foo` 在 `packages/foo/`（含 `package.toml` +
  `src/lib.tp`）→ `tap build` 与 `tap run` 都通过
- `[dependencies] bar = { path = "../bar" }` → `tapm install` 处理后能 `import bar.x`
- **现有 182 个用例全绿**，`std/*` 不受影响（包边界规则对没有 `package.toml` 的目录是空操作）
- `tap run main.tp` 的现有行为逐字节不变

## 设计决定记录

留档是为了以后不再重新争论。

| 决定 | 选择 | 被否的方案和理由 |
|---|---|---|
| 依赖目录名 | `packages/` | `deps/`、`vendor/` |
| 源码位置 | `src/` 下，清单在外层 | 源码直接放包根——那样零编译器改动，但目录会乱 |
| import 名来源 | manifest 的 `name` | —— |
| 名字怎么映射到目录 | **甲**：`tapm` 按 `name` 建目录，编译器按目录名解析 | 乙：编译器扫所有 `package.toml` 建名字表——等于给 C 编译器塞一个 TOML 解析器 |
| path 依赖 | 多目录 `TAP_MODULE_PATH` | 符号链接（Windows 要开发者模式）、复制（本地改动不生效） |
| 包管理器用什么写 | tap | C（那就不用单独一个命令了）、shell（TOML 解析会很难受） |
| 包管理器名字 | `tapm` | `tpm`（`TPM` 是 Trusted Platform Module 的通用缩写，搜索可发现性差） |
| 版本约束 | 先只支持精确版本 | semver 范围——需要求解器，等有真实需求 |
| 构建产物名 | `package.toml` 的 `name` | 入口文件基名 |
| 编译器读清单 | 极简扫描 `entry` | 完整 TOML 解析（浪费）；约定固定入口（和声明式矛盾） |
| `packages/` 进版本控制吗 | **不进**（gitignore） | 提交——但那是生成物，会让 diff 变脏 |
| `TAP_STD_PATH` 支持多目录吗 | **不支持**，保持单目录 | 标准库天然只有一个根，多目录没意义；模块根才需要多目录 |
| 锁文件名 | `package.lock.toml` | —— |
| 锁文件格式 | 参考 Cargo.lock（`[[package]]` 数组） | 自定义格式——没必要重新发明 |
| `tapm` 源码位置 | **独立仓库**，和 tap 同级（`tap-lang/tapm/`），入口 `src/main.tp` | 放在 tap 仓库里——但它是独立演进的工具，和编译器没有运行时耦合 |
| path 依赖怎么落到 `packages/` | **符号链接**（不支持则复制 + 提示） | 复制（本地改动不生效）；`tapm` 生成 roots 文件给编译器读（多一层耦合） |

**另外否掉的一个方案**：把编译器改名 `tapc`、包管理器叫 `tap`（对应 go / cargo 那套）。
技术上可行，但迁移成本约 35 处引用 / 13 个文件，而收益只是「主入口拿短名字」——
在 `tap` / `tapm` 的分工下没有这个必要。

## 已知限制

### `pub` 类型是全局裸名（第三方生态的拦路虎）

实测：两个包各写 `pub struct Point` 会报 `duplicate struct 'Point'`（且不带位置信息）。

**不解决，第三方包生态不存在**——每个包都得给类型起全局唯一名。

好消息是机制已经有了：非 `pub` 类型会被改名成 `__tap_module_N.Point`，两个包各自定义私有
`Point` 完全正常。只是 `pub` 类型跳过了这一步。

修法是让 `pub` 类型也改名，由 `import` 把类型名带进当前文件作用域，并支持命名空间限定类型
（`a.Point`，现在明确不支持）。这是**最大的单项工程**，动的是类型名解析。

**注意：这条和目录布局无关。** `src/` 怎么放都好，只要两个第三方包导出同名类型就冲突。
所以阶段 1-2 能让「自己项目的多个模块」和「手动放进 `packages/` 的包」跑起来，
但真正的第三方生态要等这一条。

### 精确版本冲突是硬冲突

`packages/` 是平铺的，同一个包只能放一个版本。`foo` 要 `bar 1.0.0`、`baz` 要 `bar 2.0.0`
→ **直接报错**。

理论上可以共存（tap 没有 ABI，全量源码展开），但那要改目录命名方案（`bar@1.0.0`），
先不做。

### 编译时间随依赖数线性涨

依赖是源码级全展开、泛型全量单态化的，每个包每个实例化都进 LLVM。
包多了要做增量编译或缓存 IR。

反过来这也是好处：**没有 ABI，所以不需要 semver 兼容矩阵**，只需要「能编译」。

### 符号链接在 Windows 上需要开发者模式

path 依赖靠符号链接接进 `packages/`，Windows 上创建符号链接需要管理员权限或开启开发者模式。
`tapm` 要检测失败并回退到复制，同时明确提示用户「本地改动不会自动生效」。

机制本身已实测：通过符号链接目录导入正常，且两个符号链接指向同一目录时**只加载一次**
（`realpath` 去重生效，不会报重复定义）。

### `tapm` 的实现缺口：目录遍历

`std.fs` 目前只有单文件操作，没有列目录（`docs/bootstrap.md` 的 B6）。
`tapm` 要扫 `packages/` 就会撞上。两个选择：

- 给 Runtime 加 `__tap_read_dir`（推荐：`std.fs` 普遍受益，且是 `__tap_` ABI，不动语法）
- 让 `tapm` 自己记录清单（锁文件里写），不扫目录

### `tapm` 自身怎么获得

`tapm` 是 tap 写的，发布时要先用 `tap` 编译出来，两个二进制一起装：

```text
bin/tap       C 写的编译器
bin/tapm      tap 写的依赖管理器（发布时编译好）
lib/...       runtime
share/tap/std/
```

`tapm` 不调用 `tap`，所以**用户侧没有运行时耦合**。

**硬约束：`tap` 必须永远能独立使用。** 任何情况下都能 `tap run tools/tapm.tp` 把 `tapm`
重新编译出来——这是唯一的自救通道，不要让它失效。

## 实现时要同步更新的文档

- ✅ [`module.md`](module.md)：路径查找顺序加了多目录、包根 `packages/` 和包边界规则；
  「当前限制」里那条「`TAP_MODULE_PATH` 只接受单个目录」已更新
- ✅ `README.md`：用法一节已加 `tap build` / `tap run` 包模式
- ⬜ [`std.md`](std.md)：如果加了 `__tap_read_dir`，`std.fs` 要补文档
- ⬜ [`test.md`](test.md)：如果回归测试加了目录级用例，要说明

## 尚未决定

暂无。阶段 1 的边界已经完整。

（这一节留给以后新增的未决项。）
