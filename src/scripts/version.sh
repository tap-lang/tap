#!/bin/bash

# 获取脚本所在目录的绝对路径
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
# 获取项目根目录
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

# 判断是否存在 git，如果存在执行 git log -1 --pretty=format:%h 获取最新的 commit id
if command -v git >/dev/null 2>&1; then
    GIT_COMMIT_ID=$(git log -1 --pretty=format:%h)
else
    GIT_COMMIT_ID="dev"
fi

#echo $GIT_COMMIT_ID

# 复制 version.h.ini 到 version.h 并替换@GIT_COMMIT_ID 为最新的 commit id
cp "$PROJECT_ROOT/version.h.ini" "$PROJECT_ROOT/version.h"

# 根据操作系统类型使用不同的 sed 语法
UNAME_S=$(uname -s)
if [ "$UNAME_S" = "Darwin" ]; then
    # macOS
    sed -i '' "s/@GIT_COMMIT_ID/$GIT_COMMIT_ID/g" "$PROJECT_ROOT/version.h"
else
    # Linux 和其他系统
    sed -i "s/@GIT_COMMIT_ID/$GIT_COMMIT_ID/g" "$PROJECT_ROOT/version.h"
fi
