#!/usr/bin/env bash
# 格式化所有 C++ 源文件
# 用法: ./tools/format.sh
set -euo pipefail

CLANG_FORMAT="${CLANG_FORMAT:-clang-format}"

# 如果系统没有 clang-format，尝试 npm 安装的版本
if ! command -v "$CLANG_FORMAT" &>/dev/null; then
  NPM_CFMT="$HOME/.npm/_npx/*/node_modules/clang-format/bin/linux_x64/clang-format"
  for f in $NPM_CFMT; do
    if [[ -x "$f" ]]; then
      CLANG_FORMAT="$f"
      break
    fi
  done
fi

if ! command -v "$CLANG_FORMAT" &>/dev/null; then
  echo "Error: clang-format not found. Install via: npm install -g clang-format"
  exit 1
fi

echo "Using: $CLANG_FORMAT --version"
$CLANG_FORMAT --version

DIRS=("src" "tests" "examples")
EXTENSIONS=("*.cpp" "*.hpp" "*.h")

for dir in "${DIRS[@]}"; do
  for ext in "${EXTENSIONS[@]}"; do
    find "$dir" -name "$ext" -exec "$CLANG_FORMAT" -i {} +
  done
done

echo "✅ Formatted all source files under: ${DIRS[*]}"
