#!/bin/sh
# 命令行冒烟测试：覆盖 tests/run-tests.sh 管不到的部分。
#
# tests/run-tests.sh 只验证「编译一个 .tp 文件、比对 stdout/stderr/退出码」，
# 覆盖不到 CLI 本身 —— `tap check` 会不会留下文件、包模式会不会建目录、
# 互斥选项有没有报错，这些都得单独测。
#
# 用法：scripts/smoke-cli.sh [tap 可执行文件路径]
#   不给参数时依次尝试 ./build/tap 和 ./build/tap.exe。
#
# 全部通过返回 0，任何一条失败返回 1。
set -u

repo_root=$(cd "$(dirname "$0")/.." && pwd)
tap=${1:-}

if [ -z "$tap" ]; then
    for candidate in "$repo_root/build/tap" "$repo_root/build/tap.exe"; do
        if [ -x "$candidate" ]; then
            tap=$candidate
            break
        fi
    done
fi

if [ -z "$tap" ] || [ ! -x "$tap" ]; then
    echo "smoke-cli: cannot find the tap binary; pass it as the first argument" >&2
    echo "smoke-cli: looked for build/tap and build/tap.exe under $repo_root" >&2
    exit 2
fi

# 后面每条用例都会 cd 到临时目录里执行，所以这里必须先转成绝对路径 ——
# 否则相对路径（./build/tap）换个工作目录就找不到，而「找不到」恰好是
# 非 0 退出，互斥类用例会全部假通过。
case $tap in
    /*) ;;
    *) tap=$(cd "$(dirname "$tap")" && pwd)/$(basename "$tap") ;;
esac

# 工作目录固定用 POSIX 的 /tmp：mktemp 在 MSYS2 下会返回 `C:\...` 这种原生路径，
# 后续 rm -rf 拿到带盘符的路径容易在受限环境里被拒，还会留下垃圾目录。
work=/tmp/tap-smoke-$$
rm -rf "$work"
mkdir -p "$work" || exit 2
trap 'rm -rf "$work"' EXIT INT TERM

passed=0
failed=0

report_pass() {
    passed=$((passed + 1))
    printf 'PASS %s\n' "$1"
}

report_fail() {
    failed=$((failed + 1))
    printf 'FAIL %s\n' "$1"
    if [ $# -gt 1 ]; then
        printf '     %s\n' "$2"
    fi
}

# 1) 通过的代码：静默、退出码 0、不产出任何文件。
mkdir -p "$work/ok"
cp "$repo_root/examples/hello.tp" "$work/ok/hello.tp"
output=$(cd "$work/ok" && "$tap" check hello.tp 2>&1)
status=$?
if [ "$status" -eq 0 ] && [ -z "$output" ]; then
    report_pass "check: 通过时静默返回 0"
else
    report_fail "check: 通过时静默返回 0" "status=$status output=$output"
fi

extra=$(cd "$work/ok" && ls -A | grep -v '^hello\.tp$' || true)
if [ -z "$extra" ]; then
    report_pass "check: 不产出文件"
else
    report_fail "check: 不产出文件" "多出: $extra"
fi

# 2) 有类型错误的代码：退出码 1，且诊断里有位置信息。
mkdir -p "$work/bad"
printf 'fn main(): i32 {\n    let a: i32 = "hello";\n    return 0;\n}\n' \
    > "$work/bad/bad.tp"
output=$(cd "$work/bad" && "$tap" check bad.tp 2>&1)
status=$?
if [ "$status" -eq 1 ]; then
    report_pass "check: 有错时返回 1"
else
    report_fail "check: 有错时返回 1" "status=$status"
fi

case $output in
    *"initializer type mismatch"*) report_pass "check: 报出预期的诊断" ;;
    *) report_fail "check: 报出预期的诊断" "实际: $output" ;;
esac

case $output in
    *"bad.tp:2"*) report_pass "check: 诊断带行列位置" ;;
    *) report_fail "check: 诊断带行列位置" "实际: $output" ;;
esac

# 3) 互斥选项。
for option in -ir -emit-obj -emit-wasm -run-lli -lex -parse -static; do
    if (cd "$work/ok" && "$tap" check "$option" hello.tp >/dev/null 2>&1); then
        report_fail "check 与 $option 互斥" "居然成功了"
    else
        report_pass "check 与 $option 互斥"
    fi
done

if (cd "$work/ok" && "$tap" check -o out.exe hello.tp >/dev/null 2>&1); then
    report_fail "check 与 -o 互斥" "居然成功了"
else
    report_pass "check 与 -o 互斥"
fi

# 4) 只能给一个命令。
if (cd "$work/ok" && "$tap" check run hello.tp >/dev/null 2>&1); then
    report_fail "check + run 被拒" "居然成功了"
else
    report_pass "check + run 被拒"
fi

# 5) 包模式：检查不创建 build/。
mkdir -p "$work/pkg/src"
printf '[package]\nname = "demo"\nentry = "src/main.tp"\n' > "$work/pkg/package.toml"
printf 'fn main(): i32 {\n    print("hi\\n");\n    return 0;\n}\n' \
    > "$work/pkg/src/main.tp"
if (cd "$work/pkg" && "$tap" check >/dev/null 2>&1); then
    if [ -d "$work/pkg/build" ]; then
        report_fail "包模式 check 不创建 build/" "build/ 被创建了"
    else
        report_pass "包模式 check 不创建 build/"
    fi
else
    report_fail "包模式 check 不创建 build/" "check 本身失败了"
fi

# 6) 包模式：在子目录里也能向上找到 package.toml。
if (cd "$work/pkg/src" && "$tap" check >/dev/null 2>&1); then
    report_pass "包模式 check 从子目录可用"
else
    report_fail "包模式 check 从子目录可用" "返回非 0"
fi

# 7) 没有 package.toml 时报错，而不是当成文件模式。
mkdir -p "$work/empty"
if (cd "$work/empty" && "$tap" check >/dev/null 2>&1); then
    report_fail "缺少 package.toml 时报错" "居然成功了"
else
    report_pass "缺少 package.toml 时报错"
fi

# 8) 回归：run / build 仍然正常。
if (cd "$work/ok" && "$tap" run hello.tp >/dev/null 2>&1); then
    report_pass "run 仍然可用"
else
    report_fail "run 仍然可用" "返回非 0"
fi

# 9) 调试信息：-g 打开时 IR 里有 !dbg，关掉时一个都没有。
#    比直接翻二进制里的 .debug_line 段名可靠 —— 运行时的目标文件本身可能带调试信息。
mkdir -p "$work/dbg"
printf 'fn add(a: i32, b: i32): i32 {\n    return a + b;\n}\n\nfn main(): i32 {\n    print("%%d\\n", add(1, 2));\n    return 0;\n}\n' \
    > "$work/dbg/dbg.tp"

if (cd "$work/dbg" && "$tap" -g -ir -o dbg.ll dbg.tp >/dev/null 2>&1) &&
    [ "$(grep -c '!dbg' "$work/dbg/dbg.ll")" -gt 0 ]; then
    report_pass "-g: IR 里带调试位置"
else
    report_fail "-g: IR 里带调试位置" "没有 !dbg"
fi

if (cd "$work/dbg" && "$tap" -ir -o nodbg.ll dbg.tp >/dev/null 2>&1) &&
    [ "$(grep -c '!dbg' "$work/dbg/nodbg.ll")" -eq 0 ]; then
    report_pass "不带 -g: IR 里没有调试位置"
else
    report_fail "不带 -g: IR 里没有调试位置" "混进了 !dbg"
fi

if grep -q 'DICompileUnit' "$work/dbg/dbg.ll" 2>/dev/null; then
    report_pass "-g: 生成了编译单元"
else
    report_fail "-g: 生成了编译单元" "缺 DICompileUnit"
fi

if (cd "$work/dbg" && "$tap" -g -emit-wasm -o dbg.wasm dbg.tp >/dev/null 2>&1); then
    report_fail "-g 与 -emit-wasm 互斥" "居然成功了"
else
    report_pass "-g 与 -emit-wasm 互斥"
fi

printf '\n%s checks: %s passed, %s failed\n' "$((passed + failed))" "$passed" "$failed"
[ "$failed" -eq 0 ]
