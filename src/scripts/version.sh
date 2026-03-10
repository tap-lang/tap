#!/bin/bash

# 判断是否存在 git，如果存在执行 git log -1 --pretty=format:%h 获取最新的 commit id
if command -v git >/dev/null 2>&1; then
    GIT_COMMIT_ID=$(git log -1 --pretty=format:%h)
else
    GIT_COMMIT_ID="dev"
fi

#echo $GIT_COMMIT_ID

# 复制 src/version.h.ini 到 src/version.h 并替换@GIT_COMMIT_ID 为最新的 commit id
cp src/version.h.ini src/version.h

# 根据操作系统类型使用不同的 sed 语法
UNAME_S=$(uname -s)
if [ "$UNAME_S" = "Darwin" ]; then
    # macOS
    sed -i '' "s/@GIT_COMMIT_ID/$GIT_COMMIT_ID/g" src/version.h
else
    # Linux 和其他系统
    sed -i "s/@GIT_COMMIT_ID/$GIT_COMMIT_ID/g" src/version.h
fi
