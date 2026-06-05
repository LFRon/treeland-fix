#!/usr/bin/env bash
#
# Pre-commit hook: 使用 git-clang-format 检查暂存的 C/C++ 文件格式
# 只检查修改的部分，而非整个文件，效率更高
#
# 安装方法：
#   1. 手动安装：ln -sf ../../scripts/pre-commit.sh .git/hooks/pre-commit   # or: cmake -S . -B build
#   2. 或在 cmake 配置时自动安装
#

set -e

# 进入仓库根目录
cd "$(git rev-parse --show-toplevel)"

# 检查 git-clang-format 是否可用
if ! command -v git-clang-format &> /dev/null; then
    echo "警告: git-clang-format 未安装，跳过格式检查"
    exit 0
fi

# 检查 .clang-format 配置文件
if [[ ! -f .clang-format ]]; then
    echo "ERROR: 未找到 .clang-format 配置文件"
    exit 1
fi

# 只检查修改的部分
output=$(git clang-format --staged --extensions 'c,h,cpp,cc,cxx' --diff 2>&1) || true

# 检查是否需要格式化
if [[ "$output" == *"no modified files to format"* ]]; then exit 0; fi
if [[ "$output" == *"clang-format did not modify any files"* ]]; then exit 0; fi
if [[ -z "$output" ]]; then exit 0; fi

echo "ERROR: 存在未格式化的代码，请先格式化："
echo ""
echo "  git clang-format --staged --extensions 'c,h,cpp,cc,cxx'    # 格式化暂存的修改"
echo "  git clang-format --staged --extensions 'c,h,cpp,cc,cxx' --diff  # 预览格式化差异"
echo ""
echo "临时跳过检查（不推荐）：git commit --no-verify"
exit 1
