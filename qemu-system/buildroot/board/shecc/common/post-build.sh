#!/usr/bin/env bash

# Buildroot runs this once the root filesystem is assembled, passing the target
# directory and the shecc architecture name. It fails unless the loader and
# libraries sit where shecc's output asks for them, and the tools the test
# scripts use are GNU ones rather than BusyBox applets.

set -euo pipefail

target=$1
arch=$2

# The kernels use their architecture's default configuration, which builds
# hundreds of drivers as modules. Everything a guest needs is built in, so the
# modules would only make the published root filesystem larger.
rm -rf "$target/lib/modules"

# The loaders are DYN_LINKER in mk/$(ARCH).mk. libdl.so.2 is there because
# dynamically linked output reaches stdin, stdout and stderr through dlsym.
case "$arch" in
    arm) loader=/lib/ld-linux-armhf.so.3 ;;
    arm64) loader=/lib/ld-linux-aarch64.so.1 ;;
    riscv) loader=/lib/ld-linux-riscv32-ilp32d.so.1 ;;
    *)
        echo "post-build: unknown architecture '$arch'" >&2
        exit 1
        ;;
esac

failed=0
for path in "$loader" /lib/libc.so.6 /lib/libdl.so.2; do
    if [ ! -e "$target$path" ]; then
        echo "post-build: $path is missing" >&2
        failed=1
    fi
done

for tool in bash sed grep awk mktemp timeout sort seq head tail; do
    path=$target/usr/bin/$tool
    if [ ! -e "$path" ]; then
        echo "post-build: $tool is missing" >&2
        failed=1
    elif [ "$(basename "$(readlink "$path" || true)")" = busybox ]; then
        echo "post-build: $tool is the BusyBox applet" >&2
        failed=1
    fi
done

exit "$failed"
