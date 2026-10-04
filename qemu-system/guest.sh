#!/usr/bin/env bash

# Runs in the guest, from the root of the payload qemu-system/run.sh packed, and
# is given the directory whose contents return to the host. Stage 1 and every
# test program are started by the guest kernel's own ELF loader. The timeouts
# keep one hung program from stalling the run.

set -uo pipefail

results=$1
# shellcheck source=/dev/null
source ./guest.env

export LC_ALL=C PATH=/usr/sbin:/usr/bin:/sbin:/bin TMPDIR=/work/tmp
mkdir -p "$TMPDIR"

# qemu-system/run.sh compares these with QSYS_FACTS to be sure the run happened
# on the kernel it meant.
{
    echo "machine: $(uname -m)"
    echo "kernel: $(uname -sr)"
    echo "page size: $(awk '/^KernelPageSize:/ { print $2, $3; exit }' /proc/self/smaps)"
} | tee "$results/facts.txt"

failed=0

# Run one step and report its exit status.
step()
{
    local name=$1 status
    shift
    echo "=== $name"
    "$@"
    status=$?
    echo "=== $name: exit $status"
    [ "$status" -eq 0 ] || failed=1
}

# The fixed point: stage 1 compiles the compiler into a byte-identical stage 2.
bootstrap()
{
    local flags
    read -r -a flags <<< "$STAGE1_FLAGS"
    timeout -k 10 3600 out/shecc-stage1.elf "${flags[@]}" \
        -o out/shecc-stage2.elf src/main.c || return
    cmp out/shecc-stage1.elf out/shecc-stage2.elf
}

step bootstrap bootstrap

# KILL rather than TERM: timeout then exits 137, which the driver reports as a
# crash, where the 124 it returns after TERM would pass as a diagnostic.
export TARGET_EXEC="timeout -s KILL 600"
if [ -f out/shecc-stage2.elf ]; then
    step driver bash tests/driver.sh 2 "$DYNLINK"
    step abi bash "tests/$ARCH-abi.sh" 2 "$DYNLINK"
fi
exit "$failed"
