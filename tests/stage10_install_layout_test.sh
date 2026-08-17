#!/usr/bin/env bash
# AI-CODE-BEGIN: S10-INSTALL-LAYOUT-TEST
set -euo pipefail

if [[ $# -ne 2 ]]; then
    echo "Usage: stage10_install_layout_test.sh <project-dir> <build-dir>" >&2
    exit 2
fi

project_dir="$1"
build_dir="$2"
temporary_dir="$(mktemp -d /tmp/edgegate-stage10-install.XXXXXX)"

cleanup() {
    if [[ "${temporary_dir}" == /tmp/edgegate-stage10-install.* ]]; then
        rm -rf -- "${temporary_dir}"
    fi
}
trap cleanup EXIT

# DESTDIR 把“/usr/local”映射到临时目录，因此可验证安装而不需要 root。
DESTDIR="${temporary_dir}" cmake --install "${build_dir}" --prefix /usr/local
install_root="${temporary_dir}/usr/local"

test -x "${install_root}/bin/edgegate"
test -x "${install_root}/bin/edgegatectl"
test -x "${install_root}/libexec/edgegate/edgegate_test_backend"
test -x "${install_root}/sbin/edgegate-service-install"
test -f "${install_root}/share/edgegate/examples/edgegate.yaml"
test -f "${install_root}/lib/systemd/system/edgegate.service"
test -f "${install_root}/lib/sysusers.d/edgegate.conf"
test -f "${install_root}/lib/tmpfiles.d/edgegate.conf"
test -f "${install_root}/share/doc/EdgeGate/README.md"
test -f "${install_root}/share/doc/EdgeGate/docs/user-guide.md"
test -f "${install_root}/share/doc/EdgeGate/docs/architecture.md"
test -f "${install_root}/share/doc/EdgeGate/docs/troubleshooting.md"
test -f "${install_root}/share/licenses/edgegate/LICENSE"

bash -n "${install_root}/sbin/edgegate-service-install"
grep -Fq "/usr/local/bin/edgegate /etc/edgegate/edgegate.yaml" \
    "${install_root}/lib/systemd/system/edgegate.service"
grep -Fq "/run/edgegate/edgegate.sock" \
    "${install_root}/share/edgegate/examples/edgegate.yaml"
grep -Fq "/var/log/edgegate" \
    "${install_root}/share/edgegate/examples/edgegate.yaml"

if grep -Fq "/tmp/edgegate" \
    "${install_root}/share/edgegate/examples/edgegate.yaml"; then
    echo "Production configuration still contains a development /tmp path" >&2
    exit 1
fi

# 防止文档或打包脚本意外修改 Windows 权威源码。
test -f "${project_dir}/00_项目上下文.md"
echo "STAGE10_INSTALL_LAYOUT=PASS"
# AI-CODE-END: S10-INSTALL-LAYOUT-TEST
