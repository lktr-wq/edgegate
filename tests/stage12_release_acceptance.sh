#!/usr/bin/env bash
# AI-CODE-BEGIN: S12-RELEASE-ACCEPTANCE
# 发布元数据门禁：防止版本不一致、产品文档绑定个人环境或常见凭据进入发布快照。
set -Eeuo pipefail
trap 'status=$?; echo "STAGE12_RELEASE_ACCEPTANCE=FAIL line=${LINENO} command=${BASH_COMMAND} status=${status}" >&2' ERR

project_dir="${1:-$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)}"
build_dir="${2:-}"

test -f "${project_dir}/CMakeLists.txt"
test -f "${project_dir}/README.md"
test -f "${project_dir}/RELEASE_NOTES.md"
test -f "${project_dir}/docs/stage11-report.md"

# 三个用户可见版本入口必须一致，避免发布标签、构建元数据和说明互相矛盾。
grep -Eq '^[[:space:]]*VERSION 1\.0\.0[[:space:]]*$' "${project_dir}/CMakeLists.txt"
grep -Fq '当前版本：`1.0.0`' "${project_dir}/README.md"
grep -Fq '# EdgeGate v1.0.0 发布说明' "${project_dir}/RELEASE_NOTES.md"

if [[ -n "${build_dir}" ]]; then
    grep -Fq 'CMAKE_PROJECT_VERSION:STATIC=1.0.0' "${build_dir}/CMakeCache.txt"
fi

# 只扫描会交付给产品使用者的文件；私人仓库中的学习/决策记录有意保留真实环境上下文。
product_paths=(
    "${project_dir}/.gitattributes"
    "${project_dir}/CMakeLists.txt"
    "${project_dir}/README.md"
    "${project_dir}/RELEASE_NOTES.md"
    "${project_dir}/apps"
    "${project_dir}/bench"
    "${project_dir}/config"
    "${project_dir}/docs"
    "${project_dir}/include"
    "${project_dir}/packaging"
    "${project_dir}/scripts"
    "${project_dir}/src"
    "${project_dir}/tests"
)

# 产品文件不得依赖某个开发者的家目录、Windows 盘符或固定私网地址。
if grep -RInE --exclude-dir='build*' --exclude-dir='__pycache__' \
    --exclude='*.log' --exclude='*.pyc' \
    '($HOME/projects/edgegate|[A-Za-z]:\\(Users|桌面|Projects)|192\.168\.[0-9]+\.[0-9]+)' \
    "${product_paths[@]}"; then
    echo 'product-facing files contain a private environment path' >&2
    exit 1
fi

# 检查常见高风险凭据形式；这不是专业密钥扫描器，但能阻止明显误提交。
if grep -RInE --exclude-dir='build*' --exclude-dir='__pycache__' \
    --exclude='*.log' --exclude='*.pyc' \
    '(BEGIN (RSA |OPENSSH |EC |DSA )?PRIVATE KEY|gh[pousr]_[A-Za-z0-9_]{20,}|AKIA[0-9A-Z]{16}|Bearer[[:space:]]+[A-Za-z0-9._-]{20,})' \
    "${product_paths[@]}"; then
    echo 'product-facing files contain a credential-like value' >&2
    exit 1
fi

# AI 代码边界标记必须成对，避免后续整理时只删掉一侧造成贡献边界失真。
begin_count="$(grep -Rho --exclude-dir='build*' --exclude-dir='__pycache__' \
    --exclude='*.pyc' 'AI-CODE-BEGIN:' "${product_paths[@]}" | wc -l)"
end_count="$(grep -Rho --exclude-dir='build*' --exclude-dir='__pycache__' \
    --exclude='*.pyc' 'AI-CODE-END:' "${product_paths[@]}" | wc -l)"
test "${begin_count}" -gt 0
test "${begin_count}" -eq "${end_count}"

echo 'RELEASE_VERSION_1_0_0=PASS'
echo 'PRODUCT_PATH_PORTABILITY=PASS'
echo 'CREDENTIAL_PATTERN_SCAN=PASS'
echo "AI_MARKER_BALANCE=PASS count=${begin_count}"
echo 'STAGE12_RELEASE_ACCEPTANCE=PASS'
# AI-CODE-END: S12-RELEASE-ACCEPTANCE
