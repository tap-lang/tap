#!/bin/sh
# 递归收集 PE 导入表里需要的非系统 DLL。
#
# 为什么要有这个脚本：分发包里光拷 tap.exe 和 ld.lld.exe 是不够的，它们还会
# 拖一串 DLL（libLLVM -> libxml2/libffi/libiconv，libstdc++ -> libgcc_s_seh）。
# 手工维护清单必然漏，漏一个的表现是程序启动后什么都不打印、退出码 127。
#
# 判断规则不需要「系统 DLL 白名单」：候选目录（通常是 MSYS2 的 bin）里有的才算
# 外部依赖，没有的一律当作 Windows 自带的系统 DLL 跳过。这样系统 DLL 清单变了
# 也不用改脚本。
#
# 用法：collect-dlls.sh <目标目录> <objdump 路径> <候选目录>...
#   目标目录里应当已经放好 .exe / .dll，脚本只往里补。
set -eu

target=$1
shift
objdump=$1
shift

if [ $# -eq 0 ]; then
    echo "collect-dlls: need at least one candidate directory" >&2
    exit 2
fi

# 列出某个 PE 文件导入的所有 DLL 名。
list_imports() {
    "$objdump" -p "$1" 2>/dev/null | sed -n 's/.*DLL Name: //p'
}

# 反复扫描，直到没有新 DLL 被加进来（新拷进来的 DLL 自己也有依赖）。
while :; do
    added=0
    for file in "$target"/*.exe "$target"/*.dll; do
        [ -f "$file" ] || continue
        for dll in $(list_imports "$file"); do
            [ -f "$target/$dll" ] && continue
            for dir in "$@"; do
                if [ -f "$dir/$dll" ]; then
                    cp -f "$dir/$dll" "$target/$dll"
                    echo "  + $dll"
                    added=1
                    break
                fi
            done
        done
    done
    [ "$added" -eq 0 ] && break
done

# 再扫一遍，报告仍然缺的（正常情况下应当只剩系统 DLL）。
missing=
for file in "$target"/*.exe "$target"/*.dll; do
    [ -f "$file" ] || continue
    for dll in $(list_imports "$file"); do
        [ -f "$target/$dll" ] && continue
        case " $missing " in
            *" $dll "*) ;;
            *) missing="$missing $dll" ;;
        esac
    done
done

if [ -n "$missing" ]; then
    echo "collect-dlls: not bundled (assumed to be system DLLs):"
    for dll in $missing; do
        echo "  - $dll"
    done
fi
