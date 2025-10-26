#!/bin/bash

# 判断是否存在git，如果存在执行 git log -1 --pretty=format:%h 获取最新的commit id
if command -v git >/dev/null 2>&1; then
    GIT_COMMIT_ID=$(git log -1 --pretty=format:%h)
else
    GIT_COMMIT_ID="dev"
fi

#echo $GIT_COMMIT_ID

# 复制src/version.h.ini到src/version.h 并替换@GIT_COMMIT_ID为最新的commit id
cp src/version.h.ini src/version.h
sed -i "s/@GIT_COMMIT_ID/$GIT_COMMIT_ID/g" src/version.h