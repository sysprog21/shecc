#!/usr/bin/env bash
set -euo pipefail

task_dir=$(mktemp -d)
trap 'rm -rf "$task_dir"' EXIT
read -r -a cc <<< "${CC:-cc}"
read -r -a a64_cc <<< "${A64_CC:-aarch64-linux-gnu-gcc}"
read -r -a runner <<< "${TARGET_EXEC-qemu-aarch64 -L /usr/aarch64-linux-gnu}"
case "${runner[0]:-}" in
    *qemu-aarch64)
        if [[ " ${runner[*]} " != *" -L "* ]] && test -d /usr/aarch64-linux-gnu; then
            runner+=(-L /usr/aarch64-linux-gnu)
        fi
        ;;
esac

# The hook goes in before the one peephole() call. Had that line changed, sed
# would leave the compiler unhooked and the test would pass untested.
[[ $(grep -c '^    peephole();$' src/main.c) == 1 ]]
# Exercise naked global code, where the usual callee-save prologue cannot
# protect the startup registers. Every allocated lane is overwritten.
{
    echo 'static void startup_bank_clobber(void);'
    sed 's/    peephole();/    startup_bank_clobber(); peephole();/' src/main.c
    cat << 'C'
static void startup_bank_clobber(void)
{
    for (int reg = 0; reg < REG_CNT; reg++) {
        vir_mop_t *ir = bb_add_mop(GLOBAL_FUNC->bbs, OP_load_constant);
        ir->dest = reg;
        ir->src0 = 123;
        ir->src1 = 0;
        ir->size_bytes = PTR_SIZE;
    }
}
C
} > "$task_dir/main.c"
"${cc[@]}" -O0 -g -std=c99 -fwrapv -iquote src -Isrc "$task_dir/main.c" -o "$task_dir/compiler"

# glibc itself need not inspect stack_end. Interpose its entry so losing the
# original process SP is checked independently of main's argc/argv checks.
cat > "$task_dir/shim.c" << 'C'
#include <unistd.h>
int __libc_start_main(int (*entry)(int, char **, char **), int argc,
                     char **argv, void *init, void *fini, void *rtld_fini,
                     void *stack_end)
{
    if ((unsigned long) stack_end < 4096 ||
        *(unsigned long *) stack_end != (unsigned long) argc)
        _exit(42);
    _exit(entry(argc, argv, 0));
}
C
"${a64_cc[@]}" -shared -fPIC "$task_dir/shim.c" -o "$task_dir/shim.so"

for size in 16 40000; do
    cat > "$task_dir/input.c" << C
char padding[$size];
int main(int argc, char **argv)
{
    return argc != 3 || argv[1][0] != 97 || argv[2][0] != 98 ||
           padding[$((size - 1))];
}
C
    for opt_flag in --no-opt ''; do
        "$task_dir/compiler" --no-libc ${opt_flag:+"$opt_flag"} -o "$task_dir/static.elf" "$task_dir/input.c"
        "$task_dir/compiler" --no-libc --dynlink ${opt_flag:+"$opt_flag"} -o "$task_dir/dynamic.elf" "$task_dir/input.c"
        chmod +x "$task_dir/static.elf" "$task_dir/dynamic.elf"
        "${runner[@]}" "$task_dir/static.elf" abc bcd
        if ((${#runner[@]})); then
            "${runner[@]}" -E "LD_PRELOAD=$task_dir/shim.so" "$task_dir/dynamic.elf" abc bcd
        else
            LD_PRELOAD="$task_dir/shim.so" "$task_dir/dynamic.elf" abc bcd
        fi
    done
done
echo 'ARM64 startup register-bank checks passed'
