/*
 * shecc - Self-Hosting and Educational C Compiler.
 *
 * shecc is freely redistributable under the BSD 2 clause license. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 */

/* Translate IR to target machine code */
#include "defs.h"
#include "globals.c"
#include "x64.c"

/* Narrow an integer result in rd to 32 bits, matching shecc's 32-bit int.
 *
 * Values live in 64-bit registers here, so arithmetic would otherwise never
 * overflow, and code that relies on wraparound (hashing, checksums) computes
 * different results and takes different branches. Addresses must keep their
 * full width, so this is skipped whenever the operation produced one.
 */
void wrap_to_int(int rd, bool is_ptr)
{
    emit_narrow_move(rd, rd, 4, !is_ptr);
}

/* Whether a value of @size_bytes, or a pointer when @is_pointer, occupies the
 * whole register.
 *
 * A narrow value means only its low bytes here. Arithmetic runs in 64-bit
 * registers and leaves whatever it likes above bit 31: an unsigned int
 * subtraction leaves a borrow there, and ~x on a zero-extended unsigned leaves
 * ones. Comparisons already respect that by taking their width from the
 * operation, and the tests that ask whether a value is zero have to as well.
 */
bool test_is_wide(int size_bytes, bool is_pointer)
{
    return is_pointer || size_bytes > 4;
}

static bool shift_immediate(ph2_ir_t *ir, int count)
{
    return count >= 0 && count < 64 &&
           (ir->op == OP_lshift ||
            (ir->op == OP_rshift && ir->size_bytes >= 4 &&
             count < (test_is_wide(ir->size_bytes, ir->src0_is_pointer) ? 64
                                                                        : 32)));
}

/* TEST @reg against itself at the width @wide selects. */
void emit_test_self(bool wide, int reg)
{
    emit_opcode_reg(0x85, reg, reg, wide);
}

/* Materialise a condition as 0 or 1 in @rd, given a SETcc opcode byte.
 *
 * SETcc lands in R11B and is then zero-extended into rd. R11 is outside
 * reg_map, so staging through it cannot clobber an allocated register the way
 * AL -- which is reg_map[6] -- would.
 */
void emit_setcc_bool(int rd, int setcc)
{
    emit_byte(REX_BASE | REX_B);
    emit_byte(0x0F);
    emit_byte(setcc);
    emit_byte(modrm(MOD_DIRECT, 0, 3));

    emit_rex(1, rd, 11); /* MOVZX rd, r11b */
    emit_byte(0x0F);
    emit_byte(0xB6);
    emit_byte(modrm(MOD_DIRECT, reg_low3(rd), 3));
}

/* The allocator's register file, in the order map_ir_reg() assigns it: IR
 * registers below this index land in caller-saved registers and cost nothing,
 * while those at or above it land in callee-saved ones that any function using
 * them must preserve. R15 holds the global base pointer and is never handed
 * out, so it never needs saving.
 */
#define X64_FIRST_CALLEE_SAVED 7

/* Bytes the prologue reserves for locals.
 *
 * The call that entered the function pushed a return address, and the four
 * register pushes below are 32 bytes (0 mod 16), so RSP is 8 mod 16 on entry to
 * the body. Reserving a multiple of 16 plus 8 restores the 16-byte alignment
 * the ABI requires at a call site; glibc routines use aligned SSE moves and
 * fault on a misaligned stack. The reservation is unconditional so that a small
 * frame is aligned too.
 */
int x64_frame_bytes(int stack_size, int saved)
{
    /* RBP plus the saved registers must leave RSP 16-byte aligned at a call
     * site. An even number of pushes lands 8 bytes off, so the frame makes it
     * up; an odd number is already aligned.
     */
    int pushes = 1 + saved;
    int pad = (pushes & 1) ? 0 : 8;
    return ALIGN_UP(stack_size, 16) + pad;
}

/* Offset from RSP to the first argument the caller left on the stack: past the
 * locals, the four saved registers and the return address. Set while a copy of
 * another block is being emitted at a back edge.
 */
bool in_dup_block;

/* Whether the instruction just emitted leaves control running into whatever
 * comes next, rather than transferring it away. Only then can the next block
 * inherit what the registers hold.
 */
bool emit_fell_through;

/* Set while emitting instructions whose position in PH2_IR_FLATTEN is unknown.
 * The forward scans below are indexed off emit_ir_index, so without this they
 * would read a stretch of some unrelated block and answer about that.
 */
bool folds_off;

/* Callee-saved registers the function being emitted preserves. */
int cur_saved_regs;

static void emit_frame_adjust(int operation, int stack_size)
{
    int bytes = x64_frame_bytes(stack_size, cur_saved_regs);
    if (bytes)
        emit_alu_imm_width(4, operation, bytes, true);
}

int x64_incoming_arg_base(int stack_size, int saved)
{
    /* Past the frame, the saved registers, RBP and the return address. */
    return x64_frame_bytes(stack_size, saved) + saved * 8 + 8 + 8;
}

/* Set to true to trace x86-64 code emission on stderr. Off in normal builds:
 * the trace is far larger than the program being compiled.
 */
bool x64_debug = false;

/* Print a function -> address map on stderr; used to locate faults in the
 * emitted binary, which carries no symbol table.
 */
bool x64_map = false;

/* Offset of main's entry within the code section, recorded as it is emitted.
 * The entry stub reads this directly instead of re-deriving it from MAIN_BB:
 * the conditional pointer read miscompiled under register pressure when shecc
 * built itself, leaving the stub calling the global initialiser.
 */
int main_code_offset = 0;

/* Frame size reserved by the most recent prologue, used to check that every
 * epilogue releases exactly the same amount.
 */
int cur_define_stack = -1;

/* Pending .rodata address relocations. The address of .rodata depends on the
 * final code size, which is not known while emitting, so each reference is
 * emitted as a MOVABS with a zero immediate and patched afterwards.
 */
typedef struct rodata_ref {
    int patch_location;
    int rodata_offset;
} rodata_ref_t;

rodata_ref_t *rodata_refs;
int rodata_ref_count = 0;
int rodata_ref_cap;

/* Pending function-address relocations, for OP_address_of_func. A function's
 * address is elf_code_start + its entry block offset, which is only final once
 * every function has been emitted. A function the dynamic linker supplies has
 * no entry block; its address is its PLT entry, placed only once the code size
 * is final.
 */
typedef struct funcaddr_ref {
    int patch_location;
    basic_block_t *target_bb;
    int plt_offset;
} funcaddr_ref_t;

funcaddr_ref_t *funcaddr_refs;
int funcaddr_ref_cap;

/* A call whose target is a PLT entry. The PLT's address is only settled once
 * the final code size is known, so the displacement is filled in afterwards.
 */
typedef struct {
    int patch_location;
    int plt_offset;
} pltcall_ref_t;

pltcall_ref_t *pltcall_refs;
int pltcall_ref_count = 0;
int pltcall_ref_cap;

/* Room for one more entry in a relocation table: each grows on demand, since
 * the number of sites is only bounded by the program's size.
 */
void *reloc_reserve(void *table, int count, int *cap, int elem_sz)
{
    if (count < *cap)
        return table;
    return arena_grow(GENERAL_ARENA, (char *) table, cap, elem_sz, 256, 0,
                      NULL);
}

/* Record a PLT call site and reserve its displacement. */
void add_pltcall_ref(int plt_offset)
{
    pltcall_refs = reloc_reserve(pltcall_refs, pltcall_ref_count,
                                 &pltcall_ref_cap, sizeof(pltcall_ref_t));
    pltcall_refs[pltcall_ref_count].patch_location = elf_code->size;
    pltcall_refs[pltcall_ref_count].plt_offset = plt_offset;
    pltcall_ref_count++;
    emit_dword(0);
}

int funcaddr_ref_count = 0;

/* Forward reference tracking */
typedef struct forward_ref {
    int patch_location;       /* Where to patch the offset in the code */
    basic_block_t *target_bb; /* Which basic block we're targeting */
} forward_ref_t;

forward_ref_t *forward_refs;
int forward_ref_count = 0;
int forward_ref_cap;

/* Add a forward reference to be patched later - call this right before
 * emit_dword Currently unused but will be needed for proper control flow
 */
void add_forward_ref(basic_block_t *target_bb)
{
    forward_refs = reloc_reserve(forward_refs, forward_ref_count,
                                 &forward_ref_cap, sizeof(forward_ref_t));

    /* patch_location is where emit_dword will place the displacement. */
    forward_refs[forward_ref_count].patch_location = elf_code->size;
    forward_refs[forward_ref_count].target_bb = target_bb;
    forward_ref_count++;
}

/* Emit x86-64 instructions for each IR operation Map an IR register (0-7) onto
 * its x86-64 register, in System V argument order first -- RDI, RSI, RDX, RCX,
 * R8, R9 -- then RAX and RBX. An index outside that range is already a physical
 * register number.
 *
 * Written as a switch rather than a table: a local array was rebuilt on every
 * one of the tens of thousands of calls to emit_ph2_ir(), and a file-scope
 * array cannot carry an initializer here, because shecc zero-fills those.
 */
int map_ir_reg(int ir_reg)
{
    switch (ir_reg) {
    case 0:
        return 7; /* rdi */
    case 1:
        return 6; /* rsi */
    case 2:
        return 2; /* rdx */
    case 3:
        return 1; /* rcx */
    case 4:
        return 8; /* r8 */
    case 5:
        return 9; /* r9 */
    case 6:
        return 0; /* rax */
    case 7:
        return 3; /* rbx */
    case 8:
        return 14; /* r14 */
    /* R12 and R13 come last because their low three bits are the SIB and disp32
     * escapes in ModR/M: naming them costs an extra byte wherever they address
     * memory, which emit_mem_base() and emit_mem_sib() already spell out, and
     * emit_lea_disp() declines. Handing them out after the others keeps that
     * cost on the least-used registers, and both are callee-saved, so a
     * function that calls anything has four registers it can keep a value in
     * rather than two.
     */
    case 9:
        return 12; /* r12 */
    case 10:
        return 13; /* r13 */
    default:
        return ir_reg;
    }
}

/* The instruction that will be emitted next, so a comparison can see whether
 * the branch consuming its result immediately follows.
 */
ph2_ir_t *emit_next_ir;

/* A comparison that emitted only its CMP, leaving the flags for the branch. */
int fused_cc;
bool fused_cc_pending;

/* The Jcc opcode matching a comparison opcode, or 0 if it is not a comparison.
 * CMP is emitted as "cmp rs1, rs2", so the sense matches the SETcc already used
 * by the unfused path.
 */

int branch_cc_for(opcode_t op, bool is_unsigned)
{
    switch (op) {
    case OP_eq:
        return 0x84; /* JE */
    case OP_neq:
        return 0x85; /* JNE */
    case OP_lt:
        return is_unsigned ? 0x82 : 0x8C; /* JB / JL */
    case OP_leq:
        return is_unsigned ? 0x86 : 0x8E; /* JBE / JLE */
    case OP_gt:
        return is_unsigned ? 0x87 : 0x8F; /* JA / JG */
    case OP_geq:
        return is_unsigned ? 0x83 : 0x8D; /* JAE / JGE */
    default:
        return 0;
    }
}

/* A copied loop test must remain reachable through its jump block. */
static bool bb_has_small_test(basic_block_t *bb)
{
    int count = 0;
    for (ph2_ir_t *ir = bb ? bb->ph2_ir_list.head : NULL; ir; ir = ir->next)
        if (++count > 6)
            return false;
    return count && bb->ph2_ir_list.tail->op == OP_branch;
}

/* Emit a rel32 displacement to @bb, deferring to the patch list when the block
 * has not been placed yet. Resolve an edge to the block that actually holds the
 * code it reaches.
 *
 * A block the allocator left with no instructions occupies no space: the walk
 * never places it, and control arriving there continues into whatever the walk
 * emits next. Following the chain to the first block with instructions names
 * that position explicitly, so both the branch target and the fall-through test
 * below agree on where the edge lands.
 */
basic_block_t *bb_code_target(basic_block_t *bb)
{
    /* Chains are short in practice; the bound is only there so that a cycle of
     * jumps cannot spin here.
     */
    for (int guard = 0; bb && guard < 16; guard++) {
        ph2_ir_t *only = bb->ph2_ir_list.head;

        if (!only) {
            bb = bb->rpo_next;
            continue;
        }

        /* A block that does nothing but jump adds a taken branch to every path
         * through it. Nested if-else chains join through several of these in a
         * row, so name the block the chain really ends at.
         */
        if (only->op == OP_jump && !only->next &&
            !bb_has_small_test(only->next_bb)) {
            bb = only->next_bb;
            continue;
        }
        break;
    }
    return bb;
}

void emit_bb_rel32(basic_block_t *bb)
{
    bb = bb_code_target(bb);
    if (bb && bb->elf_offset >= 0 && bb->elf_offset < elf_code->size) {
        emit_dword(bb->elf_offset - (elf_code->size + 4));
        return;
    }
    add_forward_ref(bb);
    emit_dword(0);
}

/* Which frame slot each IR register currently mirrors. A reload of a slot the
 * register still holds is pure overhead, and the register allocator emits those
 * freely because it reloads at every use.
 */
int reg_mirror_slot[REG_CNT];
int reg_mirror_size[REG_CNT];
bool reg_mirror_valid[REG_CNT];

/* Set when the register mirrors a slot narrower than itself, so reproducing the
 * load means sign-extending the register rather than simply copying it. A
 * mirror recorded from a load is already the loaded value and clears this.
 */
bool reg_mirror_sext[REG_CNT];

/* The width OP_load actually reads, matching the emitter's own bucketing. */
int load_width(ph2_ir_t *ir)
{
    if (ir->size_bytes == 1)
        return 1;
    if (ir->size_bytes == 2)
        return 2;
    if (ir->size_bytes == 4)
        return 4;
    return 8;
}

/* The width OP_store actually writes: one byte, or the whole 64-bit register.
 * Only the 8-byte form leaves the slot holding exactly what the register held,
 * which is what makes a later 8-byte load of that slot redundant.
 */
int store_width(ph2_ir_t *ir)
{
    int eff = ir->size_bytes;
    if (ir->is_pointer && eff != PTR_SIZE)
        eff = PTR_SIZE;
    if (eff <= 4)
        return 4;
    return 8;
}

/* Each register's value expressed as "another register shifted left by a
 * literal", so the same array index computed twice becomes a copy instead of a
 * second shift. shecc's CSE misses these because the two shift counts are
 * distinct SSA constants.
 */
int shift_src[REG_CNT];
int shift_imm[REG_CNT];

/* A global address whose LEA was not emitted because the addition right after
 * it folds the address into an access as [r15 + index + disp]. Anything that
 * turns out to need the register after all emits the LEA first.
 */
int gaddr_pending_ir = -1;
int gaddr_pending_ofs;
int gaddr_pending_at = -1;

void gaddr_materialize(void)
{
    int rd = map_ir_reg(gaddr_pending_ir);

    emit_rex(1, rd, 15);
    emit_byte(0x8D); /* LEA rd, [r15 + disp32] */
    emit_byte(modrm(MOD_DISP32, reg_low3(rd), 7));
    emit_dword(gaddr_pending_ofs);
    gaddr_pending_ir = -1;
}

/* The literal an OP_write's value register holds, when it holds one. */
bool write_imm_known;
int write_imm_val;
bool shift_valid[REG_CNT];

void shift_cache_reset(void)
{
    for (int i = 0; i < REG_CNT; i++)
        shift_valid[i] = false;
}

/* Drop everything the cache knows that depended on @reg. */
void shift_cache_kill(int reg)
{
    if (reg < 0 || reg >= REG_CNT)
        return;
    shift_valid[reg] = false;
    for (int i = 0; i < REG_CNT; i++) {
        if (shift_valid[i] && shift_src[i] == reg)
            shift_valid[i] = false;
    }
}

void frame_mirror_reset(void)
{
    for (int i = 0; i < REG_CNT; i++)
        reg_mirror_valid[i] = false;
}

/* Opcodes that neither write memory nor transfer control, so what a register
 * mirrors survives them. Anything not listed drops every mirror, which keeps
 * this conservative by default.
 */
bool op_keeps_frame_mirrors(opcode_t op)
{
    switch (op) {
    case OP_load:
    case OP_store:
    case OP_load_constant:
    case OP_assign:
    case OP_add:
    case OP_sub:
    case OP_mul: /* the two-operand IMUL writes only its destination */
    /* OP_div and OP_mod are deliberately absent: the division forms clobber RAX
     * and RDX beyond the register the IR names as their destination, which this
     * table cannot express.
     */
    case OP_lshift:
    case OP_rshift:
    case OP_bit_and:
    case OP_bit_or:
    case OP_bit_xor:
    case OP_bit_not:
    case OP_negate:
    case OP_log_not:
    case OP_eq:
    case OP_neq:
    case OP_lt:
    case OP_leq:
    case OP_gt:
    case OP_geq:
    case OP_read:
    case OP_address_of:
    case OP_trunc:
    case OP_sign_ext:
    /* Transferring control writes no register and touches no memory. What the
     * registers hold is still true on the other side; whether it is usable
     * there is decided when the next block is entered, which starts from
     * nothing unless that block has this one as its only predecessor. Keeping
     * them here is what lets the rotated copy of a loop header read the
     * induction variable the body just wrote instead of reloading its slot.
     */
    case OP_jump:
    case OP_branch:
        return true;
    default:
        return false;
    }
}

/* Which basic block each flattened instruction belongs to. */
basic_block_t **instruction_to_bb;
int instruction_to_bb_capacity;

/* Index of the instruction being emitted, for looking ahead within its block.
 */
int emit_ir_index;

/* Whether emit_next_ir belongs to the block being emitted. Every fold that
 * leaves work for the next instruction -- an address, a memory operand, a
 * condition, a source override -- is dropped when a new block starts, so the
 * work would be lost if that instruction began one.
 */
bool next_in_same_bb(void)
{
    return emit_next_ir && emit_ir_index >= 0 &&
           emit_ir_index + 1 < instruction_to_bb_capacity &&
           instruction_to_bb[emit_ir_index + 1] ==
               instruction_to_bb[emit_ir_index];
}

/* An address computed as "base + literal" that was never materialised, because
 * the load consuming it can name the displacement itself. This is how a struct
 * field access becomes one instruction instead of a LEA and a load.
 */
bool addr_fold;
int addr_fold_base; /* physical register */
int addr_fold_disp;

/* An array subscript folded into the next access's addressing mode: the element
 * address is base + index * (1 << scale), which one memory operand expresses,
 * so neither the scaling shift nor the addition needs an instruction of its
 * own.
 */
bool addr_sib;
int addr_sib_base;  /* physical register */
int addr_sib_index; /* physical register */
int addr_sib_scale; /* 0..3, a shift count */
int addr_sib_disp;  /* byte offset added to the base */

/* Hand the pending addressing mode, if any, to the access consuming it. */
bool sib_take(int *base, int *index, int *scale, int *disp)
{
    if (!addr_sib)
        return false;
    *base = addr_sib_base;
    *index = addr_sib_index;
    *scale = addr_sib_scale;
    *disp = addr_sib_disp;
    addr_sib = false;
    return true;
}

/* Whether base and index can appear together in one SIB operand.
 *
 * RSP cannot be an index at all. R12 shares its low three bits and so is turned
 * away with it: REX.X does tell the two apart, but declining the fold costs
 * only the fold, and R12 is the second-to-last register handed out.
 */
bool sib_regs_ok(int base, int index)
{
    return reg_low3(index) != 4 && base != index;
}

/* Instructions the walk should emit nothing for, named by index in the
 * flattened stream rather than by a distance, so a fold can drop instructions
 * that are not the very next one. An address fold claims up to two -- the
 * addition it absorbed and the base that addition was given -- and a
 * memory-destination fold up to two more.
 */
#define MAX_SKIP_IR 4
int skip_ir_index[MAX_SKIP_IR];

void skip_ir_reset(void)
{
    for (int i = 0; i < MAX_SKIP_IR; i++)
        skip_ir_index[i] = -1;
}

/* Claim @idx so the walk emits nothing when it reaches it. Losing a claim would
 * emit an instruction a fold has already accounted for, so running out of room
 * has to stop the compile.
 */
void skip_ir_add(int idx)
{
    for (int i = 0; i < MAX_SKIP_IR; i++) {
        if (skip_ir_index[i] < 0) {
            skip_ir_index[i] = idx;
            return;
        }
    }
    fatal("Too many folded instructions to skip");
}

/* When a load was satisfied by a register that already held the slot and its
 * only consumer is the instruction right after, that instruction names the
 * holder as its first operand and the copy is never emitted. -1 when unused.
 */
int src0_override;

/* The instruction the pending substitution belongs to, so it can apply to the
 * first real consumer rather than only the very next instruction. -1 when none.
 */
int src0_override_at;

/* A load folded into the comparison that immediately consumes it: the slot is
 * named directly as the compare's second operand. -1 when unused.
 */
int cmp_mem_slot;

/* A comparison whose second operand is a tracked literal, so it becomes an
 * immediate and the instruction that materialised it falls away.
 */
bool cmp_imm_known;
int cmp_imm_val;

/* Every deferred fold is consumed by an instruction later in the same block.
 * One still pending when the next block begins was dropped: the instruction it
 * stood in for was never emitted, and code reading its result would see a stale
 * register. Resetting the state there would hide that, so stop instead.
 */
void fold_state_check(int first_ir)
{
    bool pending = addr_fold || addr_sib || cmp_mem_slot >= 0 ||
                   src0_override >= 0 || fused_cc_pending ||
                   gaddr_pending_ir >= 0;

    for (int i = 0; i < MAX_SKIP_IR; i++) {
        if (skip_ir_index[i] >= first_ir)
            pending = true;
    }
    if (pending)
        fatal("x64: a folded instruction was left pending across a block");
}

/* How far the forward scans below look. They answer "is this register dead from
 * here?", and a definite answer is only useful near the instruction being
 * emitted; scanning whole blocks made these quadratic in block size and cost
 * more compile time than the emitted code saved.
 */
#define LIVENESS_SCAN_LIMIT 48

/* How many callee-saved registers a function needs, counted from RBX up. IR
 * registers 0..6 map to caller-saved ones, so only the indices above that
 * oblige the prologue to preserve anything.
 */
int func_saved_regs(func_t *func)
{
    return func_highest_used_reg(func, X64_FIRST_CALLEE_SAVED) -
           (X64_FIRST_CALLEE_SAVED - 1);
}

static basic_block_t *fold_scan_setup(int idx, int *stop)
{
    basic_block_t *bb = idx >= 0 && idx < instruction_to_bb_capacity
                            ? instruction_to_bb[idx]
                            : NULL;

    if (!bb)
        return NULL;
    *stop = idx + LIVENESS_SCAN_LIMIT;
    if (*stop > ph2_ir_idx)
        *stop = ph2_ir_idx;
    return bb;
}

/* True when @reg is overwritten before any later read in its block, and is not
 * carried into a successor.
 */
bool reg_dead_after(int idx, int reg)
{
    int stop;
    basic_block_t *bb;
    if (folds_off || !(bb = fold_scan_setup(idx, &stop)))
        return false;
    for (int j = idx; j < stop; j++) {
        if (j >= instruction_to_bb_capacity || instruction_to_bb[j] != bb)
            return !(bb->machine_liveout & (1u << reg));
        ph2_ir_t *ir = PH2_IR_FLATTEN[j];
        if (ir_reads_reg(ir, reg))
            return false;
        if (ph2_ir_defs(ir) & (1u << reg))
            return true;
        if (ir->op == OP_return)
            return true;
    }
    return (stop == ph2_ir_idx || instruction_to_bb[stop] != bb) &&
           !(bb->machine_liveout & (1u << reg));
}

/* Immediate branch folds must check both successors of the current block. */
static bool branch_result_dead(int reg)
{
    int stop;
    basic_block_t *bb = fold_scan_setup(emit_ir_index, &stop);
    return bb && reg >= 0 && reg < REG_CNT &&
           !(bb->machine_liveout & (1u << reg));
}

/* The opcode extension selecting an operation in the 0x81/0x83 group, or -1. */
int alu_ext_for(opcode_t op)
{
    switch (op) {
    case OP_add:
        return 0;
    case OP_sub:
        return 5;
    case OP_bit_and:
        return 4;
    case OP_bit_or:
        return 1;
    case OP_bit_xor:
        return 6;
    default:
        return -1;
    }
}

/* True when @reg's only use in this block is as the offset of a pointer add.
 * Such a value is never observed as a number, so it does not need narrowing
 * back to int width.
 */
bool reg_feeds_address_only(int idx, int reg)
{
    int stop;
    basic_block_t *bb = fold_scan_setup(idx, &stop);

    if (!bb)
        return false;
    for (int j = idx; j < stop; j++) {
        if (j >= instruction_to_bb_capacity || instruction_to_bb[j] != bb)
            return false; /* leaves the block: cannot tell */
        ph2_ir_t *ir = PH2_IR_FLATTEN[j];
        bool reads = ir_reads_reg(ir, reg);
        if (reads) {
            if (ir->op != OP_add || !ir->is_pointer)
                return false;
            return reg_dead_after(j + 1, reg);
        }
        if (op_writes_dest(ir->op) && ir->dest == reg)
            return false; /* overwritten before ever being used */
    }
    return false;
}

/* A register live in a successor must preserve its full value. */
bool low32_past_block(basic_block_t *bb, int reg)
{
    return !(bb->machine_liveout & (1u << reg));
}

/* True when no reader of @reg from @idx onward looks at bits above 31, so the
 * instruction that produced it need not narrow it back to int width.
 *
 * An int computed in a 64-bit register can carry bits an int would have
 * dropped, and MOVSXD after every such instruction is what keeps that from
 * being observed. It is only observable through a reader that uses the whole
 * register: a comparison, an address, a full-width store. A reader that is
 * itself int arithmetic computes its low 32 bits from the low 32 bits of its
 * operands, so the stray bits stay invisible as long as something downstream
 * narrows -- which multiplication and the shifts do to their own results.
 * Following that chain removes the intermediate narrowing from every array
 * index expression.
 */
bool reg_low32_sufficient(int idx, int reg, int depth)
{
    int stop;
    basic_block_t *bb;

    if (folds_off || depth > 3 || !(bb = fold_scan_setup(idx, &stop)))
        return false;
    for (int j = idx; j < stop; j++) {
        if (j >= instruction_to_bb_capacity || instruction_to_bb[j] != bb)
            return low32_past_block(bb, reg);
        ph2_ir_t *ir = PH2_IR_FLATTEN[j];
        bool reads = ir_reads_reg(ir, reg);
        if (reads) {
            /* An address is used at full width, so its offset has to be exact.
             * So does anything this table does not name.
             */
            if (ir->is_pointer)
                return false;
            if (ir->op == OP_mul) {
                /* Narrows its own result, so the stray bits stop here. */
            } else if (ir->op == OP_lshift || ir->op == OP_rshift) {
                /* Also narrows, unless it is the one case that deliberately
                 * does not: a shift feeding a pointer add straight away keeps
                 * its full width, and then the bits going into it do matter. A
                 * right shift moves the bits above 31 of the value it shifts
                 * into the result, and SAR copies bit 63 into it.
                 */
                if (ir->op == OP_rshift && ir->src0 == reg)
                    return false;
                if (reg_feeds_address_only(j + 1, ir->dest))
                    return false;
            } else if (ir->op == OP_add || ir->op == OP_sub ||
                       ir->op == OP_bit_and || ir->op == OP_bit_or ||
                       ir->op == OP_bit_xor) {
                /* Carries the stray bits into its own result, so whether they
                 * matter is decided by what reads that.
                 */
                if (!reg_low32_sufficient(j + 1, ir->dest, depth + 1))
                    return false;
            } else
                return false;
            if (op_writes_dest(ir->op) && ir->dest == reg)
                return true;
            continue;
        }
        if (op_writes_dest(ir->op) && ir->dest == reg)
            return true; /* overwritten with no further reader */
    }

    /* The last block of the program ends with the flattened stream. */
    return stop == ph2_ir_idx && low32_past_block(bb, reg);
}

/* Whether @ir can sit between a scaling shift and the addition that consumes it
 * without disturbing the fold below. These only materialise a value into their
 * own destination; nothing here rewrites more than one instruction, so the
 * addition stays where the scan found it.
 */
bool sib_gap_ok(ph2_ir_t *ir)
{
    switch (ir->op) {
    case OP_global_load:
    case OP_global_address_of:
    case OP_load_data_address:
    case OP_load_rodata_address:
    case OP_address_of:
    case OP_load_constant:
    case OP_assign:
        return true;
    default:
        return false;
    }
}

/* The load or store that consumes the address in @addr, searching forward from
 * @from within @bb, or -1 when the address is used for anything else.
 *
 * Materialising the value a store writes, or the literal an index needs, can
 * come between the address and the access. Those only define registers of their
 * own, so the fold still holds as long as none of them overwrites a register
 * the addressing mode names.
 */
int sib_find_access(basic_block_t *bb,
                    int from,
                    int addr,
                    int base_ir,
                    int index_ir)
{
    int stop = from + 4;

    if (stop > ph2_ir_idx)
        stop = ph2_ir_idx;
    for (int j = from; j < stop; j++) {
        if (instruction_to_bb[j] != bb)
            return -1;

        ph2_ir_t *ir = PH2_IR_FLATTEN[j];
        bool reads = ir_reads_reg(ir, addr);
        if (reads) {
            if ((ir->op == OP_read || ir->op == OP_write) && ir->src0 == addr &&
                !(ir->op == OP_write && ir->src1 == addr))
                return j;
            return -1;
        }
        if (op_writes_dest(ir->op) &&
            (ir->dest == addr || ir->dest == base_ir || ir->dest == index_ir))
            return -1;
        if (!sib_gap_ok(ir))
            return -1;
    }
    return -1;
}

/* Last immediate loaded into each IR register, valid only until that register
 * is written again or control flow leaves the block. Strength reduction turns
 * array indexing into a shift by a literal, but the literal arrives in a
 * register via its own OP_load_constant, so without this the backend has to
 * assume a variable count and stage it through CL.
 */
int const_reg_val[REG_CNT];
bool const_reg_valid[REG_CNT];

void const_track_reset(void)
{
    for (int i = 0; i < REG_CNT; i++)
        const_reg_valid[i] = false;
}

/* Whether @reg still holds "the global area plus a constant", and what that
 * constant is, by finding the instruction in @bb that last wrote it before
 * @before.
 *
 * R15 already holds the area's base and an addressing mode has room for the
 * constant, so an address built this way needs no register of its own in the
 * access that consumes it -- which also means the fold does not depend on the
 * register surviving until then.
 */
bool gaddr_def_ofs(basic_block_t *bb, int before, int reg, int *ofs)
{
    if (!bb || bb->ph2_base < 0 || reg < 0 || reg >= REG_CNT)
        return false;
    if (before > ph2_ir_idx)
        before = ph2_ir_idx;

    /* The definition being looked for is always close by; the same cap the
     * other scans use keeps a long block from making this quadratic.
     */
    int stop = before - LIVENESS_SCAN_LIMIT;
    if (stop < bb->ph2_base)
        stop = bb->ph2_base;
    for (int j = before - 1; j >= stop; j--) {
        ph2_ir_t *ir = PH2_IR_FLATTEN[j];

        if (!op_writes_dest(ir->op) || ir->dest != reg)
            continue;
        if (ir->op != OP_global_address_of)
            return false;
        *ofs = ir->src0;
        return true;
    }
    return false;
}

typedef struct {
    int base, index, scale, disp;
} sib_address_t;

static void sib_publish(const sib_address_t *address)
{
    addr_sib = true;
    addr_sib_base = address->base;
    addr_sib_index = address->index;
    addr_sib_scale = address->scale;
    addr_sib_disp = address->disp;
}

static int sib_consumer(basic_block_t *bb,
                        int sum_at,
                        ph2_ir_t *sum,
                        int base_ir,
                        int index_ir)
{
    int access = sib_find_access(bb, sum_at + 1, sum->dest, base_ir, index_ir);
    return access >= 0 && reg_dead_after(access + 1, sum->dest) ? access : -1;
}

/* Scaled indices need a copy before the consuming address instructions vanish.
 */
static void sib_emit_index(int rs1,
                           const sib_address_t *address,
                           int sum_at,
                           int gaddr_at)
{
    emit_reg_move(address->index, rs1, true);
    sib_publish(address);
    skip_ir_add(sum_at);
    if (gaddr_at >= 0)
        skip_ir_add(gaddr_at);
}

/* Fold a pointer-width scale and address addition into their sole access. */
bool try_fold_sib(ph2_ir_t *shift, int rd, int rs1, int scale)
{
    if (folds_off || addr_fold || addr_sib || emit_ir_index < 0 ||
        shift->size_bytes != PTR_SIZE)
        return false;
    if (scale < 1 || scale > 3)
        return false;

    basic_block_t *bb = instruction_to_bb[emit_ir_index];
    if (!bb)
        return false;

    /* Look ahead for the addition that turns the scaled index into an address.
     * Four instructions is enough for the base to be materialised.
     */
    int sum_at = -1;
    int limit = emit_ir_index + 5;
    if (limit > ph2_ir_idx)
        limit = ph2_ir_idx;
    for (int j = emit_ir_index + 1; j < limit; j++) {
        if (instruction_to_bb[j] != bb)
            return false;
        ph2_ir_t *ir = PH2_IR_FLATTEN[j];
        bool reads = ir_reads_reg(ir, shift->dest);
        if (reads) {
            sum_at = j;
            break;
        }
        if (op_writes_dest(ir->op) && ir->dest == shift->dest)
            return false;
        if (!sib_gap_ok(ir))
            return false;
    }
    if (sum_at < 0)
        return false;
    ph2_ir_t *sum = PH2_IR_FLATTEN[sum_at];
    if (sum->op != OP_add)
        return false;

    int base_ir;
    if (sum->src0 == shift->dest)
        base_ir = sum->src1;
    else if (sum->src1 == shift->dest)
        base_ir = sum->src0;
    else
        return false;
    if (base_ir == shift->dest)
        return false;

    /* Find the access first without insisting the base register survive; a base
     * that turns into a displacement below does not need it to.
     */
    int acc_at = sib_consumer(bb, sum_at, sum, -1, shift->dest);
    if (acc_at < 0)
        return false;

    /* The scaled index and the element address exist only for this access. The
     * addition commonly writes the same register it scaled, which ends the
     * scaled value there and needs no separate check.
     */
    if (sum->dest != shift->dest && !reg_dead_after(sum_at + 1, shift->dest))
        return false;

    /* Global bases become displacements and need not survive the access. */
    sib_address_t address = {map_ir_reg(base_ir), rd, scale, 0};
    int gaddr_at = -1;

    /* Resolve definitions between the scale and sum before older ones. */
    bool base_written = false;

    for (int j = emit_ir_index + 1; j < sum_at; j++) {
        ph2_ir_t *ir = PH2_IR_FLATTEN[j];

        if (op_writes_dest(ir->op) && ir->dest == base_ir) {
            /* A non-global write also blocks the older-definition fallback. */
            base_written = true;
            if (ir->op == OP_global_address_of) {
                gaddr_at = j;
                address.disp = ir->src0;
            } else {
                /* Something else put the address there, so it is not the global
                 * area plus a literal any more.
                 */
                gaddr_at = -1;
            }
            continue;
        }

        /* Anything else reading the base means the address it holds is wanted
         * for more than this one access.
         */
        if (ir_reads_reg(ir, base_ir))
            gaddr_at = -1;
    }

    /* Nothing in between named it, so the definition that reaches the addition
     * is the one before the shift.
     */
    if (gaddr_at < 0 && !base_written &&
        gaddr_def_ofs(bb, emit_ir_index, base_ir, &address.disp)) {
        address.base = 15;
        sib_emit_index(rs1, &address, sum_at, gaddr_at);
        return true;
    }
    if (gaddr_at >= 0 && reg_dead_after(sum_at + 1, base_ir)) {
        address.base = 15;
    } else {
        gaddr_at = -1;
        address.disp = 0;

        /* A register that only ever held a literal may never have been
         * materialised, because the literal was expected to become an
         * immediate.
         */
        if (base_ir >= 0 && base_ir < REG_CNT && const_reg_valid[base_ir])
            return false;
        if (sib_find_access(bb, sum_at + 1, sum->dest, base_ir, shift->dest) !=
            acc_at)
            return false;
    }
    if (!sib_regs_ok(address.base, address.index))
        return false;

    sib_emit_index(rs1, &address, sum_at, gaddr_at);
    return true;
}

/* Build an unscaled address without consuming or publishing the producer. */
static bool sib_add_address(ph2_ir_t *sum,
                            basic_block_t *bb,
                            int at,
                            int global_ir,
                            int rs1,
                            int rs2,
                            sib_address_t *address)
{
    if (folds_off || addr_fold || addr_sib || !bb || sum->op != OP_add ||
        sum->src0 < 0 || sum->src0 >= REG_CNT || sum->src1 < 0 ||
        sum->src1 >= REG_CNT || const_reg_valid[sum->src0] ||
        const_reg_valid[sum->src1])
        return false;
    int base_ir = sum->src0, index_ir = sum->src1;
    address->base = rs1;
    address->index = rs2;
    address->scale = address->disp = 0;
    if (address->base == address->index)
        return false;
    bool global = global_ir >= 0;
    if (global) {
        if ((base_ir == global_ir) == (index_ir == global_ir))
            return false;
        if (index_ir == global_ir)
            index_ir = base_ir;
    } else if (gaddr_def_ofs(bb, at, base_ir, &address->disp)) {
        global = true;
    } else if (gaddr_def_ofs(bb, at, index_ir, &address->disp)) {
        global = true;
        index_ir = base_ir;
    }
    if (global) {
        address->base = 15;
        address->index = index_ir == sum->src0 ? rs1 : rs2;
        base_ir = -1;
    }
    if (sib_consumer(bb, at, sum, base_ir, index_ir) < 0)
        return false;
    if (!global && reg_low3(address->index) == 4) {
        address->index = address->base;
        address->base = rs2;
    }
    return reg_low3(address->index) != 4;
}

bool try_fold_sib_add(ph2_ir_t *ir, int rs1, int rs2)
{
    if (emit_ir_index < 0 || emit_ir_index >= instruction_to_bb_capacity)
        return false;
    sib_address_t address;
    if (!sib_add_address(ir, instruction_to_bb[emit_ir_index], emit_ir_index,
                         -1, rs1, rs2, &address))
        return false;
    sib_publish(&address);
    return true;
}

/* True when the only remaining reads of @reg in this block are shift counts
 * that will be emitted as immediates, so materialising the constant is
 * pointless. A successor may still read the register, so leaving the block
 * without an overwrite does not prove that the load is dead.
 */
bool const_load_dead(int idx, int reg, int val)
{
    int stop;
    basic_block_t *bb;

    if (folds_off || !(bb = fold_scan_setup(idx, &stop)))
        return false;
    bool folds_ok = true;
    for (int j = idx; j < stop; j++) {
        if (j >= instruction_to_bb_capacity || instruction_to_bb[j] != bb)
            return !(bb->machine_liveout & (1u << reg));
        ph2_ir_t *ir = PH2_IR_FLATTEN[j];
        /* Uses that become immediates do not read the register at all. */
        bool folded_count = false;
        if (ir->src1 == reg) {
            if (shift_immediate(ir, val))
                folded_count = true;
            if (ir->op == OP_add || ir->op == OP_sub || ir->op == OP_bit_and ||
                ir->op == OP_bit_or || ir->op == OP_bit_xor)
                folded_count = true;
            /* The three-operand IMUL carries its multiplier as an immediate. */
            if (ir->op == OP_mul)
                folded_count = true;
            /* A stored literal becomes the store's immediate operand. */
            if (ir->op == OP_write && ir->src0 != reg)
                folded_count = true;
            if (branch_cc_for(ir->op,
                              ir->src0_is_unsigned || ir->src1_is_unsigned))
                folded_count = true;
        }

        if (op_src0_is_reg(ir->op) && ir->src0 == reg)
            return false;
        if (!(folds_ok && folded_count) && op_src1_is_reg(ir->op) &&
            ir->src1 == reg)
            return false;
        if (op_writes_dest(ir->op) && ir->dest == reg)
            return true;

        /* Past anything that clears the constant table, a later use can no
         * longer become an immediate. A store through a pointer reads only the
         * registers it names, so a register nothing reads again is still dead
         * across it and the scan can go on. Anything else may read registers
         * this walk cannot see -- a call takes its arguments in them -- so stop
         * rather than drop a value it would consume.
         */
        if (!op_keeps_frame_mirrors(ir->op)) {
            if (ir->op == OP_return)
                return true;
            if (ir->op == OP_jump || ir->op == OP_branch)
                return !(bb->machine_liveout & (1u << reg));
            if (ir->op != OP_write)
                return false;
            folds_ok = false;
        }
    }
    /* Ran out of scan budget: assume the register is still needed. */
    return (stop == ph2_ir_idx || instruction_to_bb[stop] != bb) &&
           !(bb->machine_liveout & (1u << reg));
}

/* True when @bb is entered from a backward edge, i.e. it heads a loop. */
bool bb_is_loop_header(basic_block_t *bb)
{
    for (int i = 0; i < bb->prev_idx; i++) {
        basic_block_t *pred = bb->prev[i].bb;
        if (pred && pred->rpo >= bb->rpo)
            return true;
    }
    return false;
}

/* The block whose code actually runs before @bb, when there is exactly one.
 *
 * A block that emits nothing leaves the registers as its own predecessor left
 * them, so the search walks back through empty blocks. Without this, the arm a
 * conditional branch falls into looks like it has an unrelated predecessor
 * whenever the CFG puts an empty block on that edge, and everything the
 * registers hold is discarded for no reason.
 */
basic_block_t *bb_sole_code_pred(basic_block_t *bb)
{
    basic_block_t *pred = bb_sole_pred(bb);
    int guard = 0;

    /* A chain of empty blocks that led back to itself would spin here. Every
     * cycle needs a jump or a branch, and a block holding one is not empty, so
     * this should not arise -- but the walk is cheap to bound and the failure
     * would be a hang rather than a wrong answer.
     */
    while (pred && !pred->ph2_ir_list.head && guard < MAX_BB_DOM_SUCC) {
        pred = bb_sole_pred(pred);
        guard++;
    }
    return pred;
}

/* CMP rs1 against rs2, or against the slot a folded load named instead. */
void emit_cmp(int size_bytes, int rs1, int rs2)
{
    if (cmp_imm_known) {
        cmp_imm_known = false;
        cmp_mem_slot = -1;
        emit_alu_imm_width(rs1, 7, cmp_imm_val, size_bytes > 4);
        return;
    }
    if (cmp_mem_slot >= 0) {
        emit_rex(size_bytes > 4, rs1, -1);
        emit_byte(0x3B); /* CMP rs1, [rsp + slot] */
        emit_rsp_mem(rs1, cmp_mem_slot);
        cmp_mem_slot = -1;
        return;
    }
    emit_opcode_reg(0x39, rs1, rs2, size_bytes > 4); /* CMP rs1, rs2 */
}

/* True when @bb is the block the emitter is about to start, so a branch to it
 * would land on the very next instruction and can simply fall through. The
 * emission loop writes nothing between instructions, so this is exact rather
 * than an assumption about block ordering.
 */
bool bb_falls_through(basic_block_t *bb)
{
    bb = bb_code_target(bb);
    return bb && emit_next_ir && bb->ph2_ir_list.head == emit_next_ir;
}

static void emit_conditional_edges(int condition,
                                   basic_block_t *then_bb,
                                   basic_block_t *else_bb)
{
    if (bb_falls_through(then_bb)) {
        basic_block_t *target = then_bb;
        then_bb = else_bb;
        else_bb = target;
        condition ^= 1;
    }
    emit_byte(0x0F);
    emit_byte(condition);
    emit_bb_rel32(then_bb);
    if (!bb_falls_through(else_bb)) {
        emit_byte(0xE9);
        emit_bb_rel32(else_bb);
        emit_fell_through = false;
    }
}

static void emit_alu_imm_rsp(int ext, int slot, int imm, bool wide)
{
    if (wide)
        emit_rex(true, -1, -1);
    if (imm >= -128 && imm <= 127) {
        emit_byte(0x83);
        emit_rsp_mem(ext, slot);
        emit_byte(imm);
    } else {
        emit_byte(0x81);
        emit_rsp_mem(ext, slot);
        emit_dword(imm);
    }
}

static void emit_mov_imm32(int reg, int imm, bool wide)
{
    if (wide || reg >= 8)
        emit_rex(wide, -1, reg);
    emit_byte(0xC7);
    emit_byte(modrm(MOD_DIRECT, 0, reg_low3(reg)));
    emit_dword(imm);
}

static bool x64_signed_imm32(unsigned long long constant, int *imm)
{
    long long signed_value = (long long) constant;

    if (signed_value < INT_MIN || signed_value > INT_MAX ||
        (unsigned long long) signed_value != constant)
        return false;
    *imm = (int) signed_value;
    return true;
}

static void emit_x64_constant(int reg, bool wide, unsigned long long constant)
{
    int signed_imm;

    if (wide && x64_signed_imm32(constant, &signed_imm)) {
        emit_mov_imm32(reg, signed_imm, true);
        return;
    }
    if (wide || reg >= 8)
        emit_rex(wide, -1, reg);
    emit_byte(0xB8 + reg_low3(reg)); /* MOV r, imm */
    emit_dword((unsigned int) constant);
    if (wide)
        emit_dword((unsigned int) (constant >> 32));
}

/* Collapse "load slot; op result, loaded, x; store result to the same slot"
 * into one instruction operating directly on the slot.
 *
 * Returns true when it emitted the folded form and the two following
 * instructions should be skipped. Only fires when both accesses use the same
 * width and neither register outlives the sequence.
 */
bool try_fold_mem_dest(ph2_ir_t *ld)
{
    if (folds_off || emit_ir_index < 0)
        return false;
    if (ld->op != OP_load || emit_ir_index + 2 >= ph2_ir_idx)
        return false;
    if (emit_ir_index + 2 >= instruction_to_bb_capacity ||
        instruction_to_bb[emit_ir_index] !=
            instruction_to_bb[emit_ir_index + 2])
        return false;

    ph2_ir_t *alu = PH2_IR_FLATTEN[emit_ir_index + 1];
    ph2_ir_t *st = PH2_IR_FLATTEN[emit_ir_index + 2];
    int ext = alu_ext_for(alu->op);
    if (ext < 0 || st->op != OP_store)
        return false;
    if (alu->src0 != ld->dest || st->src0 != alu->dest)
        return false;
    if (st->src1 != ld->src0)
        return false; /* a different slot */

    int width = load_width(ld);
    if (width != store_width(st) || (width != 4 && width != 8))
        return false;
    if (!reg_dead_after(emit_ir_index + 2, ld->dest) ||
        !reg_dead_after(emit_ir_index + 3, alu->dest))
        return false;

    bool imm =
        alu->src1 >= 0 && alu->src1 < REG_CNT && const_reg_valid[alu->src1];
    int other = map_ir_reg(alu->src1);
    if (!imm && (alu->src1 == ld->dest || alu->src1 == alu->dest))
        return false; /* the operand would have to come from the skipped load */

    if (imm) {
        int val = const_reg_val[alu->src1];
        emit_alu_imm_rsp(ext, ld->src0, val, width == 8);
    } else {
        if (width == 8)
            emit_rex(1, other, -1);
        else if (other >= 8)
            emit_byte(REX_R);
        /* The register-source group with a memory destination. */
        emit_byte(alu_opcode(ext, false));
        emit_rsp_mem(other, ld->src0);
    }

    /* The slot changed, so nothing mirrors it any more. */
    for (int i = 0; i < REG_CNT; i++) {
        if (reg_mirror_valid[i] && reg_mirror_slot[i] == ld->src0)
            reg_mirror_valid[i] = false;
    }
    skip_ir_add(emit_ir_index + 1);
    skip_ir_add(emit_ir_index + 2);
    return true;
}

/* Collapse "op result, x, imm; store result into the slot x mirrors" into one
 * instruction on the slot. The load may be far behind; what matters is that the
 * operand register still mirrors the slot. Both registers must be dead
 * afterwards -- if the incremented value or its source is read again, keeping
 * it in a register beats making the next reader go back to memory.
 */
bool try_fold_alu_to_slot(ph2_ir_t *alu)
{
    if (folds_off || emit_ir_index < 0)
        return false;
    int ext = alu_ext_for(alu->op);
    if (ext < 0 || emit_ir_index + 1 >= ph2_ir_idx)
        return false;
    if (emit_ir_index + 1 >= instruction_to_bb_capacity ||
        instruction_to_bb[emit_ir_index] !=
            instruction_to_bb[emit_ir_index + 1])
        return false;

    ph2_ir_t *st = PH2_IR_FLATTEN[emit_ir_index + 1];
    if (st->op != OP_store || st->src0 != alu->dest)
        return false;
    if (alu->src0 < 0 || alu->src0 >= REG_CNT || !reg_mirror_valid[alu->src0])
        return false;
    if (reg_mirror_slot[alu->src0] != st->src1)
        return false;

    int width = reg_mirror_size[alu->src0];
    if (width != store_width(st) || (width != 4 && width != 8))
        return false;
    if (alu->src1 < 0 || alu->src1 >= REG_CNT || !const_reg_valid[alu->src1])
        return false;
    if (!reg_dead_after(emit_ir_index + 2, alu->dest) ||
        !reg_dead_after(emit_ir_index + 2, alu->src0))
        return false;

    int val = const_reg_val[alu->src1];
    emit_alu_imm_rsp(ext, st->src1, val, width == 8);
    for (int i = 0; i < REG_CNT; i++) {
        if (reg_mirror_valid[i] && reg_mirror_slot[i] == st->src1)
            reg_mirror_valid[i] = false;
    }
    shift_cache_kill(alu->dest);
    skip_ir_add(emit_ir_index + 1);
    return true;
}

/* Emit "rd = rs1 * c" using something cheaper than IMUL where the multiplier
 * allows it, and report whether it did.
 *
 * IMUL takes three cycles, which is the whole of a loop-carried chain like "h =
 * h * 31 + c". A shift, an LEA, or a shift with one correction computes the
 * same product in one or two, so a multiplier that is a power of two, one away
 * from one, or small enough for LEA's scale is worth spelling out.
 */
bool emit_mul_by_const(int rd, int rs1, int c, bool wide)
{
    int k = exact_log2(c);

    if (c == 0) {
        emit_opcode_reg(0x31, rd, rd, false); /* XOR rd, rd */
        return true;
    }

    /* LEA computes rs1 + rs1 * s for s of 1, 2, 4 or 8 in one instruction,
     * covering multipliers of 2, 3, 5 and 9 without touching rs1.
     */
    if ((c == 2 || c == 3 || c == 5 || c == 9) &&
        emit_lea_scaled_sum_width(rd, rs1, rs1, exact_log2(c - 1), wide))
        return true;
    /* A power of two is a shift, and one is that shift by nothing. */
    if (k >= 0) {
        if (rd != rs1)
            emit_reg_move(rd, rs1, wide);
        if (k) {
            emit_shift_imm(rd, SHIFT_EXT_SHL, k, wide);
        }
        return true;
    }

    /* One away from a power of two: shift, then correct by the original value.
     * Two instructions where IMUL is one, but two cycles where IMUL is three --
     * worth it exactly when the product feeds back into its own operand, which
     * is what an accumulator like "h = h * 31 + c" looks like once the
     * destination has been coalesced onto the source. Elsewhere the extra
     * instruction is not paid for.
     */
    if (rd == rs1) {
        int up = exact_log2(c - 1), down = exact_log2(c + 1);

        if (up > 1 || down > 1) {
            /* R10 is outside the allocator's file, so it can hold the original
             * value across the shift that overwrites it.
             */
            emit_reg_move(10, rs1, wide); /* MOV r10, rs1 */
            emit_shift_imm(rd, SHIFT_EXT_SHL, up > 1 ? up : down, wide);
            emit_rex(wide, 10, rd);
            emit_byte(up > 1 ? 0x01 : 0x29); /* ADD/SUB rd, r10 */
            emit_byte(modrm(MOD_DIRECT, 2, reg_low3(rd)));
            return true;
        }
    }
    return false;
}

/* An OP_jump whose target is a single instruction emits that instruction in
 * place, so the family emitters below reach back into the dispatcher.
 */
void emit_ph2_ir(ph2_ir_t *ph2_ir);

/* DIV/IDIV clobber RAX and RDX regardless of width. Preserve allocated RAX
 * around the PH2 division sequence.
 */
static void emit_x64_division(int rd,
                              int dividend,
                              int divisor,
                              bool preserve_rax,
                              bool wide,
                              bool is_unsigned,
                              bool remainder)
{
    int division_reg = divisor;

    if (preserve_rax)
        emit_reg_move(10, 0, true);
    emit_push_reg(2);
    if (divisor != 10 && divisor != 11) {
        emit_reg_move(11, divisor, wide);
        division_reg = 11;
    }
    if (dividend != 0)
        emit_reg_move(0, dividend, wide);
    if (is_unsigned)
        emit_opcode_reg(0x31, 2, 2, false);
    else {
        if (wide)
            emit_byte(REX_W);
        emit_byte(0x99);
    }
    emit_rex(wide, -1, division_reg);
    emit_byte(0xF7);
    emit_byte(modrm(MOD_DIRECT, is_unsigned ? 6 : 7, reg_low3(division_reg)));
    emit_reg_move(11, remainder ? 2 : 0, wide);
    if (preserve_rax)
        emit_reg_move(0, 10, true);
    emit_pop_reg(2);
    emit_reg_move(rd, 11, wide);
}

static void emit_alu_reg(int ext, int dest, int src, bool wide)
{
    emit_opcode_reg(alu_opcode(ext, false), dest, src, wide);
}

/* Integer arithmetic. */
void emit_arith(ph2_ir_t *ph2_ir,
                int rd,
                int rs1,
                int rs2,
                bool src1_const_known,
                int src1_const)
{
    switch (ph2_ir->op) {
    case OP_add:
    case OP_sub: {
        bool subtract = ph2_ir->op == OP_sub;
        bool wide = test_is_wide(ph2_ir->size_bytes, ph2_ir->is_pointer);

        if (src1_const_known) {
            /* A following memory operation can carry this address directly. */
            if (wide && !subtract && !addr_fold && next_in_same_bb() &&
                (emit_next_ir->op == OP_read || emit_next_ir->op == OP_write) &&
                emit_next_ir->src0 == ph2_ir->dest &&
                emit_next_ir->src1 != ph2_ir->dest && reg_low3(rs1) != 4 &&
                reg_dead_after(emit_ir_index + 2, ph2_ir->dest)) {
                addr_fold = true;
                addr_fold_base = rs1;
                addr_fold_disp = src1_const;
                return;
            }
            if (rd != rs1 && !(subtract && src1_const == INT_MIN) &&
                emit_lea_disp_width(rd, rs1,
                                    subtract ? -src1_const : src1_const, wide))
                return;
            if (rd != rs1)
                emit_reg_move(rd, rs1, wide);
            emit_alu_imm_width(rd, alu_ext_for(ph2_ir->op), src1_const, wide);
            return;
        }

        if (subtract) {
            if (rd != rs2) {
                if (rd != rs1)
                    emit_reg_move(rd, rs1, wide);
                emit_alu_reg(5, rd, rs2, wide);
            } else {
                emit_reg_move(11, rs1, wide);
                emit_alu_reg(5, 11, rs2, wide);
                emit_reg_move(rd, 11, wide);
            }
            return;
        }

        if (wide && try_fold_sib_add(ph2_ir, rs1, rs2)) {
            gaddr_pending_ir = -1;
            return;
        }
        if (gaddr_pending_ir >= 0)
            gaddr_materialize();
        if (rd == rs2 && rd != rs1)
            emit_alu_reg(0, rd, rs1, wide);
        else if (rd == rs1 || rd == rs2 ||
                 !emit_lea_scaled_sum_width(rd, rs1, rs2, 0, wide)) {
            if (rd != rs1)
                emit_reg_move(rd, rs1, wide);
            emit_alu_reg(0, rd, rs2, wide);
        }
        return;
    }
    case OP_mul: {
        bool wide = test_is_wide(ph2_ir->size_bytes, ph2_ir->is_pointer);

        if (src1_const_known && emit_mul_by_const(rd, rs1, src1_const, wide)) {
            /* nothing further: the product is already in rd */
        } else if (src1_const_known) {
            /* The three-operand IMUL takes the multiplier as an immediate and
             * writes a different register, so neither the literal nor the MOV
             * that would stage it needs an instruction.
             */
            emit_imul_imm(rd, rs1, src1_const, wide);
        } else if (rd == rs2 && rd != rs1) {
            /* IMUL is commutative, so multiply by rs1 in place. */
            emit_imul_reg(rd, rs1, wide);
        } else {
            if (rd != rs1)
                emit_reg_move(rd, rs1, wide);
            emit_imul_reg(rd, rs2, wide);
        }
        return;
    }
    case OP_div:
    case OP_mod: {
        /* DIV/IDIV force the dividend into RDX:RAX and return the quotient in
         * RAX and remainder in RDX. Both are allocatable registers here
         * (reg_map[6] and reg_map[2]) and the allocator does not know they are
         * clobbered, so save and restore them around the sequence. R10 and R11
         * are outside reg_map and can stage values; RDX goes on the stack
         * rather than into a third scratch, because every register that once
         * served as one has since joined the file and staging RDX there
         * destroyed a dividend pinned to it.
         */
        bool wide = ph2_ir->size_bytes > 4;
        bool is_unsigned = ph2_ir->src0_is_unsigned || ph2_ir->src1_is_unsigned;

        /* A 32-bit unsigned operation must use EDX:EAX, not its 64-bit
         * counterpart. For example, C requires -1 / 2U to be 2147483647,
         * whereas a 64-bit DIV sees 2^64 - 1. The save of RAX is always 64-bit:
         * RAX may hold a live long long beside an int division.
         */
        emit_x64_division(rd, rs1, rs2, true, wide, is_unsigned,
                          ph2_ir->op == OP_mod);

        /* The 32-bit move above zero-extends, but a signed int result must sit
         * in its register sign-extended like every other narrow scalar: a later
         * widening to long long reads the whole register, and -5000 was seen as
         * 4294962296.
         */
        if (!wide && !is_unsigned &&
            !reg_low32_sufficient(emit_ir_index + 1, ph2_ir->dest, 0))
            wrap_to_int(rd, false);
        return;
    }
    default:
        break;
    }
}

/* Bitwise operations and shifts. */
static void emit_variable_shift(int rd,
                                int rs1,
                                int rs2,
                                int size_bytes,
                                bool source_unsigned,
                                bool source_pointer,
                                bool left)
{
    emit_reg_move(10, 1, true);
    if (!left && !source_unsigned && size_bytes <= 4 && !source_pointer)
        emit_narrow_move(11, rs1, 4, true);
    else
        emit_reg_move(11, rs1, left || !source_unsigned || size_bytes > 4);
    emit_reg_move(1, rs2, true);
    emit_byte(REX_W | REX_B);
    emit_byte(0xD3);
    emit_byte(modrm(MOD_DIRECT,
                    left              ? SHIFT_EXT_SHL
                    : source_unsigned ? SHIFT_EXT_SHR
                                      : SHIFT_EXT_SAR,
                    3));
    emit_reg_move(1, 10, true);
    emit_reg_move(rd, 11, true);
}

void emit_bitwise(ph2_ir_t *ph2_ir,
                  int rd,
                  int rs1,
                  int rs2,
                  bool src1_const_known,
                  int src1_const)
{
    switch (ph2_ir->op) {
    case OP_bit_and:
    case OP_bit_or:
    case OP_bit_xor: {
        int ext = alu_ext_for(ph2_ir->op);
        bool wide = test_is_wide(ph2_ir->size_bytes, ph2_ir->is_pointer);
        if (src1_const_known) {
            if (rd != rs1)
                emit_reg_move(rd, rs1, wide);
            emit_alu_imm_width(rd, ext, src1_const, wide);
            return;
        }

        /* Two-operand form: "rd = rs1 OP rs2" is MOV rd, rs1 followed by OP rd,
         * rs2. When rd already names rs2, commutativity lets the MOV go and rs1
         * becomes the source instead.
         */
        int src = rs2;
        if (rd == rs2 && rd != rs1)
            src = rs1;
        else if (rd != rs1)
            emit_reg_move(rd, rs1, wide);
        emit_alu_reg(ext, rd, src, wide);
        return;
    }

    case OP_bit_not:
    case OP_negate:
        emit_mov_reg(rd, rs1);
        emit_rex(1, -1, rd);
        emit_byte(0xF7);
        emit_byte(
            modrm(MOD_DIRECT, ph2_ir->op == OP_bit_not ? 2 : 3, reg_low3(rd)));
        return;

    case OP_lshift: {
        /* A count the block already loaded as a literal needs neither CL nor
         * the save/restore around it: SHL r64, imm8 is one instruction.
         */
        if (src1_const_known && shift_immediate(ph2_ir, src1_const)) {
            bool wide =
                test_is_wide(ph2_ir->size_bytes, ph2_ir->src0_is_pointer);
            int want_src = ph2_ir->src0;
            int dest_ir = ph2_ir->dest;

            /* The same shift of the same register may already sit in another
             * register; copying it is one instruction instead of three.
             */
            for (int i = 0; i < REG_CNT; i++) {
                if (!wide || !shift_valid[i] || shift_src[i] != want_src ||
                    shift_imm[i] != src1_const || i == dest_ir)
                    continue;
                int held = map_ir_reg(i);
                emit_mov_reg(rd, held);
                if (dest_ir >= 0 && dest_ir < REG_CNT) {
                    shift_valid[dest_ir] = true;
                    shift_src[dest_ir] = want_src;
                    shift_imm[dest_ir] = src1_const;
                }
                return;
            }

            /* "index << k; add base; read/write" is one x86 addressing mode.
             * Folding it away removes both the shift and the addition, which is
             * every array subscript in the program.
             */
            if (try_fold_sib(ph2_ir, rd, rs1, src1_const))
                return;

            bool shift_wide = wide || src1_const >= 32;
            emit_reg_move(rd, rs1, shift_wide);
            emit_shift_imm(rd, SHIFT_EXT_SHL, src1_const, shift_wide);
            if (shift_wide && ph2_ir->size_bytes <= 4)
                wrap_to_int(rd, ph2_ir->is_pointer);

            /* Recording needs the shift source to still be intact: if the
             * result landed in it, the pairing no longer describes anything.
             */
            if (wide && dest_ir >= 0 && dest_ir < REG_CNT &&
                dest_ir != want_src && want_src >= 0 && want_src < REG_CNT) {
                shift_valid[dest_ir] = true;
                shift_src[dest_ir] = want_src;
                shift_imm[dest_ir] = src1_const;
            }
            return;
        }

        /* rd = rs1 shl rs2.
         *
         * x86 takes the shift count in CL, but RCX is reg_map[3] and may hold a
         * live value the allocator still needs -- it has no notion of this
         * instruction clobbering a register. Save RCX in R10, stage the value
         * in R11, then restore. R10/R11 are never handed out by reg_map, so
         * this is safe for every aliasing of rd, rs1 and rs2.
         */
        emit_variable_shift(rd, rs1, rs2, ph2_ir->size_bytes,
                            ph2_ir->src0_is_unsigned, ph2_ir->src0_is_pointer,
                            true);

        /* The shift happened in a 64-bit register, so bits that an int would
         * have dropped survive. Narrow exactly as OP_mul does: without this, "1
         * << 31" (which strength reduction also produces for "x * 2") stayed
         * positive and every later comparison took the wrong branch.
         */
        if (ph2_ir->size_bytes <= 4)
            wrap_to_int(rd, ph2_ir->is_pointer);
        return;
    }
    case OP_rshift: {
        bool wide = test_is_wide(ph2_ir->size_bytes, ph2_ir->src0_is_pointer);
        if (src1_const_known && shift_immediate(ph2_ir, src1_const)) {
            if (rd != rs1)
                emit_reg_move(rd, rs1, wide);
            emit_shift_imm(
                rd, ph2_ir->src0_is_unsigned ? SHIFT_EXT_SHR : SHIFT_EXT_SAR,
                src1_const, wide);
            return;
        }

        /* rd = rs1 sar rs2.
         *
         * x86 takes the shift count in CL, but RCX is reg_map[3] and may hold a
         * live value the allocator still needs -- it has no notion of this
         * instruction clobbering a register. Save RCX in R10, stage the value
         * in R11, then restore. R10/R11 are never handed out by reg_map, so
         * this is safe for every aliasing of rd, rs1 and rs2.
         */
        emit_variable_shift(rd, rs1, rs2, ph2_ir->size_bytes,
                            ph2_ir->src0_is_unsigned, ph2_ir->src0_is_pointer,
                            false);
        return;
    }
    default:
        break;
    }
}

/* Comparisons, and the jumps and branches they feed. */
void emit_compare_jump(ph2_ir_t *ph2_ir, int rd, int rs1, int rs2)
{
    if (op_is_comparison(ph2_ir->op)) {
        /* CMP rs1, rs2, then turn the flags into 0 or 1 in rd. The six
         * comparisons differ only in the condition, and SETcc is the matching
         * Jcc opcode plus 0x10, so branch_cc_for() supplies both forms.
         */
        emit_cmp(ph2_ir->size_bytes, rs1, rs2);
        emit_setcc_bool(
            rd, branch_cc_for(ph2_ir->op, ph2_ir->src0_is_unsigned ||
                                              ph2_ir->src1_is_unsigned) +
                    0x10);
        return;
    }
    switch (ph2_ir->op) {
    case OP_jump: {
        if (bb_falls_through(ph2_ir->next_bb))
            return;

        /* Everything below either jumps away or emits a rotated copy of the
         * target block, whose own control flow this walk does not model. Treat
         * the next block as unreachable from here either way.
         */
        emit_fell_through = false;

        /* Rotate the loop. Jumping back to a small block that tests the
         * condition and branches costs two taken branches per iteration, where
         * a bottom-tested loop costs one. Emitting that test here instead makes
         * the unconditional jump disappear, and this copy is reached only along
         * the back edge, so what the registers hold coming out of the body
         * still applies.
         */
        if (!in_dup_block && ph2_ir->next_bb) {
            if (bb_has_small_test(ph2_ir->next_bb)) {
                ph2_ir_t *saved_next = emit_next_ir;
                int saved_index = emit_ir_index;

                /* The copy runs the same instructions in the same order, so the
                 * forward scans stay valid -- but only against the block's own
                 * position in the flattened stream, not the jump's.
                 */
                int dup_base = ph2_ir->next_bb->ph2_base;

                in_dup_block = true;
                folds_off = dup_base < 0;
                int n = 0;
                for (ph2_ir_t *t = ph2_ir->next_bb->ph2_ir_list.head; t;
                     t = t->next) {
                    emit_ir_index = dup_base < 0 ? -1 : dup_base + n;
                    emit_next_ir = t->next ? t->next : saved_next;
                    emit_ph2_ir(t);
                    n++;
                }
                in_dup_block = false;
                folds_off = false;
                emit_ir_index = saved_index;
                emit_next_ir = saved_next;
                return;
            }
        }

        /* JMP rel32, through the same target resolution the conditional edges
         * use, so a jump landing on nothing but another jump goes straight to
         * where that one ends up.
         */
        emit_byte(0xE9);
        emit_bb_rel32(ph2_ir->next_bb);
        return;
    }

    case OP_branch: {
        int condition;

        if (fused_cc_pending) {
            fused_cc_pending = false;
            condition = fused_cc;
        } else {
            /* An int condition is decided by its low word alone. */
            emit_test_self(
                test_is_wide(ph2_ir->size_bytes, ph2_ir->src0_is_pointer), rs1);
            condition = 0x85; /* JNZ */
        }
        emit_conditional_edges(condition, ph2_ir->then_bb, ph2_ir->else_bb);
        return;
    }
    default:
        break;
    }
}

/* Keep a call result in RAX when its only consumer can read it directly. */
static void emit_call_result(void)
{
    if (next_in_same_bb() && emit_next_ir->op == OP_assign &&
        emit_next_ir->src0 == 0 && emit_next_ir->dest > 0 &&
        emit_next_ir->dest_hi < 0 &&
        emit_ir_index + 2 < instruction_to_bb_capacity &&
        instruction_to_bb[emit_ir_index + 2] ==
            instruction_to_bb[emit_ir_index] &&
        reg_dead_after(emit_ir_index + 2, 0)) {
        src0_override = 0;
        src0_override_at = emit_ir_index + 1;
    } else
        emit_reg_move(7, 0, true); /* System V RAX to machine register 0. */
}

/* Calls, and the returns that unwind them. */
void emit_call_return(ph2_ir_t *ph2_ir, int rs1)
{
    switch (ph2_ir->op) {
    case OP_call:
        /* CALL rel32 - find the target function and calculate relative offset
         */
        {
            func_t *target_func = find_func(ph2_ir->func_name);

            if (target_func && target_func->is_static && !target_func->bbs) {
                printf("Error: Undefined static function called: %s\n",
                       function_source_name(target_func));
                fflush(stdout); /* see fatal() */
                abort();
            } else if (dynlink && target_func && !target_func->bbs) {
                /* An external symbol: call its PLT entry, which the loader
                 * redirects to the real function on first use.
                 */
                emit_byte(0xE8); /* CALL rel32 */
                add_pltcall_ref(target_func->plt_offset);
            } else if (!target_func || !target_func->bbs) {
                /* Without dynamic linking there is nothing to call, and a CALL
                 * with displacement 0 does not trap: it pushes a return address
                 * and falls into the next instruction, leaving RSP eight bytes
                 * low for the remainder of the function so that every [rsp
                 * + ofs] local and the epilogue read the wrong slots. Refuse to
                 * emit the image instead of shipping one that misbehaves
                 * silently.
                 */
                printf("Error: Undefined function called: %s\n",
                       ph2_ir->func_name);
                fflush(stdout); /* see fatal() */
                abort();
            } else {
                emit_byte(0xE8); /* CALL rel32 */

                /* Check if this is a forward reference (function not compiled
                 * yet)
                 */
                if (target_func->bbs->elf_offset == -1) {
                    /* Forward reference - BB not emitted yet, add to patch list
                     */
                    add_forward_ref(target_func->bbs);
                    emit_dword(0); /* Placeholder - will be patched later */
                } else {
                    /* BB already emitted - calculate actual offset */
                    int displacement =
                        target_func->bbs->elf_offset - (elf_code->size + 4);
                    emit_dword(displacement);
                }
            }

            emit_call_result();
        }
        return;

    case OP_return:
        emit_fell_through = false;
        /* If there's a return value in src0, move it to RAX */
        if (ph2_ir->src0 >= 0) {
            /* rs1 already names the physical register, including the case where
             * a folded load redirected this instruction to read the register
             * that still holds the value. Re-deriving it from src0 here would
             * ignore that redirection and return a stale register.
             */
            int ret_reg = rs1;
            if (ph2_ir->src0_is_unsigned && ph2_ir->size_bytes < PTR_SIZE) {
                emit_zero_extend(0, ret_reg, ph2_ir->size_bytes);
                ret_reg = 0;
            }
            /* Debug output MOV rax, ret_reg - only if not already in RAX */
            if (ret_reg != 0) {                  /* 0 is RAX, no move needed */
                emit_reg_move(0, ret_reg, true); /* MOV RAX, ret_reg */
            }
        }
        /* Function epilogue: ADD rsp, aligned_size; POP r13; POP r12; POP rbx;
         * POP rbp; RET (R15 is reserved for global base pointer)
         */
        {
            int stack_size_ret = ph2_ir->src1;
            if (cur_define_stack >= 0 && stack_size_ret != cur_define_stack)
                fprintf(stderr,
                        "[x64] FRAME MISMATCH: prologue reserved %d, epilogue "
                        "releases %d\n",
                        cur_define_stack, stack_size_ret);
            emit_frame_adjust(0, stack_size_ret);
        }
        /* R15 is reserved for global base pointer - don't save/restore */
        for (int cs = cur_saved_regs - 1; cs >= 0; cs--)
            emit_pop_reg(map_ir_reg(X64_FIRST_CALLEE_SAVED + cs));
        emit_byte(0x5D); /* POP rbp */
        emit_byte(0xC3); /* RET */
        return;

    case OP_func_ret:
        /* This should not be used in x64 backend - the register allocator
         * generates OP_assign to move the return value from RAX
         */
        printf("Warning: OP_func_ret should not reach x64 backend\n");
        return;

    default:
        break;
    }
}

/* Stack slots, addresses, conditional moves, loads and stores. */
static int slot_width(const ph2_ir_t *ir)
{
    return ir->is_pointer && ir->size_bytes != PTR_SIZE ? PTR_SIZE
                                                        : ir->size_bytes;
}

static void emit_load_instruction(int reg,
                                  int width,
                                  bool is_unsigned,
                                  int base,
                                  int index,
                                  bool sib)
{
    bool zero_extend = is_unsigned && width <= 4;

    if (sib)
        emit_rex_sib(zero_extend ? 0 : 1, reg, base, index);
    else
        emit_rex(zero_extend ? 0 : 1, reg, base);
    if (width == 1 || width == 2) {
        emit_byte(0x0F);
        emit_byte(width == 1 ? (is_unsigned ? 0xB6 : 0xBE)
                             : (is_unsigned ? 0xB7 : 0xBF));
    } else if (width == 4) {
        emit_byte(zero_extend ? 0x8B : 0x63);
    } else {
        emit_byte(0x8B);
    }
}

/* Locals use pointer-sized slots, so narrow values share a dword store. Globals
 * are packed and must be written at their declared width.
 */
static void emit_store_instruction(int width,
                                   int reg,
                                   int base,
                                   int index,
                                   bool sib,
                                   bool immediate)
{
    bool wide = width != 1 && width != 2 && width != 4;
    bool rex =
        wide || reg >= 8 || base >= 8 || index >= 8 || (width == 1 && reg >= 4);

    if (width == 2)
        emit_byte(0x66);
    if (rex) {
        if (sib)
            emit_rex_sib(wide, reg, base, index);
        else
            emit_rex(wide, reg, base);
    }
    emit_byte(immediate ? (width == 1 ? 0xC6 : 0xC7)
                        : (width == 1 ? 0x88 : 0x89));
}

static void emit_slot_store(ph2_ir_t *ir, int reg, int base, bool packed)
{
    int width = slot_width(ir);

    if (!packed && width <= 4)
        width = 4;
    emit_store_instruction(width, reg, base, -1, false, false);
}

typedef struct {
    int base, index, scale, displacement;
    bool sib, has_displacement;
} x64_memory_operand_t;

static x64_memory_operand_t take_memory_operand(int base)
{
    x64_memory_operand_t operand = {0};

    operand.base = base;
    operand.sib = sib_take(&operand.base, &operand.index, &operand.scale,
                           &operand.displacement);
    if (addr_fold) {
        if (!operand.sib) {
            operand.base = addr_fold_base;
            operand.displacement = addr_fold_disp;
            operand.has_displacement = true;
        }
        addr_fold = false;
    }
    return operand;
}

static void emit_memory_operand(int reg, const x64_memory_operand_t *operand)
{
    if (operand->sib)
        emit_mem_sib(reg, operand->base, operand->index, operand->scale,
                     operand->displacement);
    else
        emit_mem_base(reg, operand->base, operand->displacement,
                      operand->has_displacement);
}

void emit_memory(ph2_ir_t *ph2_ir, int rd, int rs1)
{
    switch (ph2_ir->op) {
    case OP_allocat: {
        /* SUB rsp, size.
         *
         * src0 is a byte count, not a register index, so it must be read raw:
         * 'rs1' has already been rewritten through reg_map[] above and would
         * turn any size in 0..7 into an unrelated register number.
         */
        int alloc_bytes = ph2_ir->src0;
        emit_alu_imm_width(4, 5, alloc_bytes, true);
        return;
    }

    case OP_address_of: {
        /* LEA rd, [rsp + src0] */
        emit_rex(1, rd, -1);
        emit_byte(0x8D);
        emit_rsp_mem(rd, ph2_ir->src0);
        return;
    }

    case OP_load:
        emit_load_instruction(rd, slot_width(ph2_ir), ph2_ir->is_unsigned, -1,
                              -1, false);
        emit_rsp_mem(rd, ph2_ir->src0);
        return;

    case OP_store:
        emit_slot_store(ph2_ir, rs1, -1, false);
        emit_rsp_mem(rs1, ph2_ir->src1);
        return;

    default:
        break;
    }
}

/* Indirect access through a pointer. */
void emit_read_write(ph2_ir_t *ph2_ir, int rd, int rs1, int rs2)
{
    switch (ph2_ir->op) {
    case OP_read:
        /* Load *rs1 into rd. The element width lives in src1 (1, 2 or 4);
         * pointer-typed reads must move a full pointer instead.
         */
        {
            /* get_size() already yields PTR_SIZE for pointer-typed reads, so
             * the IR width is authoritative. is_pointer describes the address
             * operand, not the element, and must not be consulted.
             */
            int rsize = ph2_ir->src1;
            x64_memory_operand_t address = take_memory_operand(rs1);
            ph2_ir_t *alu = emit_next_ir;
            int at = emit_ir_index + 1;

            /* A postincrement may sit between the read and its arithmetic use.
             * Moving register arithmetic across it preserves the memory access.
             */
            if (alu && alu->op == OP_add && alu->size_bytes == 8 &&
                alu->dest == ph2_ir->src0 && alu->src0 == alu->dest &&
                alu->src1 != ph2_ir->dest && ph2_ir->dest != ph2_ir->src0) {
                alu = alu->next;
                at++;
            }
            int ext = alu ? alu_ext_for(alu->op) : -1;
            if (!folds_off && ext >= 0 && at < instruction_to_bb_capacity &&
                instruction_to_bb[at] == instruction_to_bb[emit_ir_index] &&
                (rsize == 4 || rsize == 8) && alu->size_bytes == rsize &&
                (!alu->is_pointer || rsize == 8) && alu->src1 == ph2_ir->dest &&
                alu->dest == alu->src0 && alu->dest != ph2_ir->dest &&
                (at == emit_ir_index + 1 ||
                 (alu->op != OP_bit_and && alu->dest != ph2_ir->src0 &&
                  alu->dest != emit_next_ir->src1)) &&
                reg_dead_after(at + 1, ph2_ir->dest)) {
                int accumulator = map_ir_reg(alu->dest);
                if (address.sib)
                    emit_rex_sib(rsize == 8, accumulator, address.base,
                                 address.index);
                else
                    emit_rex(rsize == 8, accumulator, address.base);
                emit_byte(alu_opcode(ext, false) + 2);
                emit_memory_operand(accumulator, &address);
                const_reg_valid[alu->dest] = false;
                reg_mirror_valid[alu->dest] = false;
                shift_cache_kill(alu->dest);
                skip_ir_add(at);
                return;
            }

            emit_load_instruction(rd, rsize, ph2_ir->is_unsigned, address.base,
                                  address.index, address.sib);
            emit_memory_operand(rd, &address);
        }
        return;

    case OP_write:
        /* Store value (src1) into [address (src0)] - IR uses src0 as address */
        {
            int val_reg = rs2;
            x64_memory_operand_t address = take_memory_operand(rs1);
            /* Debug: trace write width mismatches for pointers */
            if (ph2_ir->is_pointer && ph2_ir->size_bytes != PTR_SIZE) {
                if (x64_debug)
                    fprintf(stderr,
                            "[EMIT] OP_write ptr width mismatch: size=%d "
                            "ptr=%d "
                            "addr=%d val=%d\n",
                            ph2_ir->size_bytes, ph2_ir->is_pointer,
                            ph2_ir->src0, ph2_ir->src1);
            }

            /* The store width the IR asks for lives in dest (1, 2 or 4);
             * pointer-typed writes must store a full pointer. As with OP_read,
             * the IR width already accounts for pointers.
             */
            int wsize = ph2_ir->dest;

            /* A value the block loaded as a literal is stored as an immediate:
             * MOV r/m, imm, with /0 in the reg field. The literal register's
             * load is then dead and never emitted. A 64-bit store takes a
             * sign-extended imm32, which is the value the register held.
             */
            if (write_imm_known &&
                (wsize == 1 || wsize == 2 || wsize == 4 || wsize == 8)) {
                emit_store_instruction(wsize, 0, address.base, address.index,
                                       address.sib, true);
                emit_memory_operand(0, &address);
                if (wsize == 1)
                    emit_byte(write_imm_val & 0xFF);
                else if (wsize == 2) {
                    emit_byte(write_imm_val & 0xFF);
                    emit_byte((write_imm_val >> 8) & 0xFF);
                } else
                    emit_dword(write_imm_val);
                return;
            }
            emit_store_instruction(wsize, val_reg, address.base, address.index,
                                   address.sib, false);
            emit_memory_operand(val_reg, &address);
        }
        return;

    case OP_indirect:
        /* CALL r11 (staged by the preceding OP_load_func) */
        emit_byte(REX_W | REX_B);
        emit_byte(0xFF);
        emit_byte(modrm(MOD_DIRECT, 2, 3));
        emit_call_result();
        return;

    default:
        break;
    }
}

/* Globals, functions, and the data sections they live in. Emit R15 plus a
 * global byte offset.
 */
static void emit_r15_address(int rd, int data_ofs)
{
    if (!data_ofs) {
        /* R15 is the MOV source, so it occupies ModRM's reg field. */
        emit_reg_move(rd, 15, true);
    } else {
        emit_rex(1, rd, 11);
        emit_byte(0x8D);
        if (data_ofs < 128) {
            emit_byte(modrm(MOD_DISP8, reg_low3(rd), 7));
            emit_byte(data_ofs);
        } else {
            emit_byte(modrm(MOD_DISP32, reg_low3(rd), 7));
            emit_dword(data_ofs);
        }
    }
}

void emit_global(ph2_ir_t *ph2_ir, int rd, int rs1)
{
    switch (ph2_ir->op) {
    case OP_load_func:
        /* Stage the callee address in R11 for the OP_indirect that follows. R11
         * is outside reg_map, so nothing the allocator owns is disturbed.
         */
        emit_reg_move(11, rs1, true);
        return;

    case OP_address_of_func:
        /* Store the address of func_name into [rs1]. The address is not known
         * until every function has been emitted, so record it for patching.
         */
        {
            func_t *target = find_func(ph2_ir->func_name);
            emit_byte(REX_W | REX_B);
            emit_byte(0xB8 + 3); /* MOVABS r11, imm64 */
            if (target && (target->bbs || dynlink)) {
                funcaddr_refs =
                    reloc_reserve(funcaddr_refs, funcaddr_ref_count,
                                  &funcaddr_ref_cap, sizeof(funcaddr_ref_t));
                funcaddr_refs[funcaddr_ref_count].patch_location =
                    elf_code->size;
                funcaddr_refs[funcaddr_ref_count].target_bb = target->bbs;
                funcaddr_refs[funcaddr_ref_count].plt_offset =
                    target->plt_offset;
                funcaddr_ref_count++;
            }
            emit_dword(0);
            emit_dword(0);

            /* MOV [rs1], r11. Built with statements rather than a conditional
             * expression: folding the register test into emit_byte's argument
             * produced a corrupted high nibble once shecc compiled itself, so
             * the store named the wrong base register.
             */
            emit_rex(1, 11, rs1); /* reg field is r11 */
            emit_byte(0x89);
            emit_mem_base(3, rs1, 0, false);
        }
        return;

    case OP_global_address_of: {
        /* src0 is a byte offset, not a mapped register. */
        int data_ofs = ph2_ir->src0;

        /* When the addition right after takes this address only to fold it into
         * an access as [r15 + index + disp], and nothing else reads it, the LEA
         * is dead. These are the conditions try_fold_sib_add() folds under; if
         * it declines anyway, it emits the LEA then.
         */
        ph2_ir_t *sum = emit_next_ir;
        basic_block_t *gbb =
            emit_ir_index >= 0 && emit_ir_index + 1 < instruction_to_bb_capacity
                ? instruction_to_bb[emit_ir_index]
                : NULL;
        sib_address_t address;
        if (gbb && sum && instruction_to_bb[emit_ir_index + 1] == gbb &&
            sib_add_address(sum, gbb, emit_ir_index + 1, ph2_ir->dest,
                            map_ir_reg(sum->src0), map_ir_reg(sum->src1),
                            &address) &&
            reg_dead_after(emit_ir_index + 2, ph2_ir->dest)) {
            gaddr_pending_ir = ph2_ir->dest;
            gaddr_pending_ofs = data_ofs;
            gaddr_pending_at = emit_ir_index;
            return;
        }
        emit_r15_address(rd, data_ofs);
    }
        return;

    case OP_global_load: {
        int dest_reg = map_ir_reg(ph2_ir->dest);

        emit_load_instruction(dest_reg, slot_width(ph2_ir), ph2_ir->is_unsigned,
                              15, -1, false);
        emit_r15_mem(reg_low3(dest_reg), ph2_ir->src0);
    }
        return;

    case OP_global_store: {
        int src_reg = rs1; /* honours a redirected source, as OP_store does */

        emit_slot_store(ph2_ir, src_reg, 15, true);
        emit_r15_mem(reg_low3(src_reg), ph2_ir->src1);
    }
        return;

    case OP_load_data_address: {
        /* Calculate address using R15 + offset (LEA rd, [r15 + offset]).
         *
         * src0 is a byte offset into the data area, not a register index, so it
         * is read raw rather than through 'rs1', which emit_ph2_ir() has
         * already rewritten through reg_map[].
         */
        int data_ofs = ph2_ir->src0;
        emit_r15_address(rd, data_ofs);
    }
        return;

    case OP_load_rodata_address: {
        /* MOVABS rd, imm64 with a placeholder address, recorded for patching
         * once .rodata's final location is known.
         */
        emit_rex(1, -1, rd);
        emit_byte(0xB8 + reg_low3(rd));
        {
            rodata_refs = reloc_reserve(rodata_refs, rodata_ref_count,
                                        &rodata_ref_cap, sizeof(rodata_ref_t));
            rodata_refs[rodata_ref_count].patch_location = elf_code->size;

            /* src0 is a .rodata byte offset, not a register index; read it raw
             * so that offsets 0..7 are not rewritten by reg_map[].
             */
            rodata_refs[rodata_ref_count].rodata_offset = ph2_ir->src0;
            rodata_ref_count++;
        }
        emit_dword(0);
        emit_dword(0);
        return;
    }
    default:
        break;
    }
}

/* Logical operators and width conversions. */
void emit_logic_cast(ph2_ir_t *ph2_ir, int rd, int rs1)
{
    switch (ph2_ir->op) {
    case OP_log_not: {
        /* TEST rs1, rs1; SETE r11b; MOVZX rd, r11b.
         *
         * The source is rs1, which is not always the same register as rd -- for
         * a global it never is.
         */
        emit_test_self(
            test_is_wide(ph2_ir->size_bytes, ph2_ir->src0_is_pointer), rs1);

        emit_setcc_bool(rd, 0x94); /* SETE */
    }
        return;

    case OP_log_and:
    case OP_log_or:
        emit_mov_imm32(rd, 1, true); /* Always return 1 for now */
        return;

    case OP_trunc: {
        /* Narrow rs1 to the target width held in src1, matching the 32-bit
         * backends: the language has no unsigned types, so every narrowing
         * keeps the sign -- 127 + 1 wraps to -128, 32767
         * + 1 to -32768 -- and a 4-byte truncation sign-extends the low word.
         */
        int tsize = ph2_ir->src1;
        if (ph2_ir->is_unsigned) {
            emit_zero_extend(rd, rs1, tsize);
            return;
        }
        emit_narrow_move(rd, rs1, tsize, true);
        return;
    }

    case OP_sign_ext: {
        /* src1 packs (source type size << 16) | target size, as built by the
         * parser's promote() helper.
         *
         * rs1/rd are already physical registers here; they must not be run
         * through reg_map a second time.
         */
        int src_size = (ph2_ir->src1 >> 16) & 0xFFFF;
        int dst_size = ph2_ir->src1 & 0xFFFF;
        int width = src_size;

        /* Widening to a pointer. The register already holds a complete 64-bit
         * address, so sign-extending it from 32 bits would discard the upper
         * half and destroy every stack address. Copy it instead, which is also
         * what a source that is already full width wants.
         */
        if (ph2_ir->is_pointer && dst_size == PTR_SIZE && PTR_SIZE == 8)
            width = 8;
        else if (width != 1 && width != 2 && width != 4)
            width = 8;
        if (ph2_ir->src0_is_unsigned)
            emit_zero_extend(rd, rs1, width);
        else
            emit_narrow_move(rd, rs1, width, true);
    }
        return;

    case OP_cast: {
        /* A narrow scalar sits in its 64-bit register extended by its own
         * signedness, and the instructions that read the whole register -- a
         * right shift or a widening, for instance -- rely on that. An
         * equal-width cast that changes the signedness must therefore redo the
         * extension: an int cast from unsigned int 0xffffffff otherwise kept
         * zeroes above it, and shifting it right arithmetically gave
         * 0x7fffffff.
         */
        if (ph2_ir->is_unsigned && ph2_ir->size_bytes < PTR_SIZE)
            emit_zero_extend(rd, rs1, ph2_ir->size_bytes);
        else if (ph2_ir->src0_is_unsigned && ph2_ir->size_bytes < PTR_SIZE)
            emit_narrow_move(rd, rs1, ph2_ir->size_bytes, true);
        else
            emit_mov_reg(rd, rs1);
    }
        return;

    default:
        break;
    }
}

void emit_ph2_ir(ph2_ir_t *ph2_ir)
{
    for (int i = 0; i < MAX_SKIP_IR; i++) {
        if (skip_ir_index[i] >= 0 && skip_ir_index[i] == emit_ir_index) {
            skip_ir_index[i] = -1;
            return;
        }
    }

    /* Only the addition the elided address was meant for may run without it,
     * and only through the fold in try_fold_sib_add().
     */
    bool gaddr_owner = gaddr_pending_ir >= 0 && ph2_ir->op == OP_add &&
                       emit_ir_index == gaddr_pending_at + 1;
    if (gaddr_pending_ir >= 0 && !gaddr_owner)
        gaddr_materialize();
    if (!gaddr_owner && try_fold_mem_dest(ph2_ir))
        return;
    if (!gaddr_owner && try_fold_alu_to_slot(ph2_ir))
        return;

    int rd = map_ir_reg(ph2_ir->dest);
    int rs1 = map_ir_reg(ph2_ir->src0);
    if (src0_override >= 0 && src0_override_at == emit_ir_index) {
        rs1 = src0_override;
        src0_override = -1;
        src0_override_at = -1;
    }
    int rs2 = map_ir_reg(ph2_ir->src1);

    /* Read the tracked value before invalidating, since dest may alias src1. */
    bool src1_const_known = false;
    int src1_const = 0;
    if (ph2_ir->src1 >= 0 && ph2_ir->src1 < REG_CNT &&
        const_reg_valid[ph2_ir->src1]) {
        src1_const_known = true;
        src1_const = const_reg_val[ph2_ir->src1];
    }
    if (op_writes_dest(ph2_ir->op) && ph2_ir->dest >= 0 &&
        ph2_ir->dest < REG_CNT) {
        const_reg_valid[ph2_ir->dest] = false;
        shift_cache_kill(ph2_ir->dest);
    }

    /* Anything that can touch a register this table does not name -- a call, or
     * a division writing RAX and RDX -- invalidates all of it. This is the same
     * set that drops frame mirrors below.
     */
    if (!op_keeps_frame_mirrors(ph2_ir->op)) {
        const_track_reset();
        shift_cache_reset();
    }
    if (ph2_ir->op != OP_branch)
        fused_cc_pending = false;
    if (!branch_cc_for(ph2_ir->op,
                       ph2_ir->src0_is_unsigned || ph2_ir->src1_is_unsigned)) {
        cmp_mem_slot = -1;
        cmp_imm_known = false;
    } else if (src1_const_known) {
        cmp_imm_known = true;
        cmp_imm_val = src1_const;
    }

    /* A value loaded only to be compared does not need a register: x86 takes
     * the second compare operand from memory. Unlike arithmetic, nothing reuses
     * this value afterwards, so folding it costs no later reload.
     */
    if (ph2_ir->op == OP_load && next_in_same_bb() &&
        branch_cc_for(emit_next_ir->op, emit_next_ir->src0_is_unsigned ||
                                            emit_next_ir->src1_is_unsigned) &&
        emit_next_ir->src1 == ph2_ir->dest &&
        emit_next_ir->src0 != ph2_ir->dest &&
        reg_dead_after(emit_ir_index + 2, ph2_ir->dest)) {
        int w = load_width(ph2_ir);

        /* A folded memory operand must have the same width as the comparison.
         * In particular, comparing an int load after it has been promoted to
         * long must not turn into an eight-byte read from its four-byte slot.
         */
        if ((w == 4 || w == 8) && w == emit_next_ir->size_bytes) {
            cmp_mem_slot = ph2_ir->src0;
            return;
        }
    }

    if (!op_keeps_frame_mirrors(ph2_ir->op)) {
        frame_mirror_reset();
    } else {
        /* A volatile slot is read however recently a register mirrored it: the
         * read is an access the program performs.
         */
        if (ph2_ir->op == OP_load && !ph2_ir->is_volatile &&
            ph2_ir->dest >= 0 && ph2_ir->dest < REG_CNT) {
            int want = load_width(ph2_ir);
            if (reg_mirror_valid[ph2_ir->dest] &&
                reg_mirror_slot[ph2_ir->dest] == ph2_ir->src0 &&
                reg_mirror_size[ph2_ir->dest] == want &&
                !reg_mirror_sext[ph2_ir->dest])
                return; /* the register still holds this slot */
            /* Some other register may already hold it. Copying between
             * registers costs the same instruction but avoids waiting on the
             * store-to-load forwarding this slot would otherwise require.
             */
            for (int i = 0; i < REG_CNT; i++) {
                if (!reg_mirror_valid[i] ||
                    reg_mirror_slot[i] != ph2_ir->src0 ||
                    reg_mirror_size[i] != want)
                    continue;
                if (i == ph2_ir->dest && !reg_mirror_sext[i])
                    continue;
                int held = map_ir_reg(i);

                /* If this value exists only to be stored straight back out, let
                 * the store read the register that already holds it. Only sound
                 * when the register is bit-identical to what the load would
                 * produce, so a narrower mirror is excluded. Only when the
                 * consumer is the very next instruction: a later one could be
                 * skipped by another fold, and the copy would already be gone.
                 * And only when src0 is its one read of the register: the
                 * override redirects that read alone, so a second read of the
                 * same register in src1 would read the value that this load
                 * would have overwritten.
                 */
                if (!reg_mirror_sext[i] && src0_override < 0 &&
                    next_in_same_bb() && op_src0_is_reg(emit_next_ir->op) &&
                    emit_next_ir->src0 == ph2_ir->dest &&
                    emit_next_ir->src1 != ph2_ir->dest &&
                    reg_dead_after(emit_ir_index + 2, ph2_ir->dest)) {
                    src0_override = held;
                    src0_override_at = emit_ir_index + 1;
                    return;
                }
                emit_narrow_move(rd, held, want, reg_mirror_sext[i]);
                reg_mirror_valid[ph2_ir->dest] = true;
                reg_mirror_slot[ph2_ir->dest] = ph2_ir->src0;
                reg_mirror_size[ph2_ir->dest] = want;
                reg_mirror_sext[ph2_ir->dest] = false;
                const_reg_valid[ph2_ir->dest] = false;
                return;
            }
        }

        if (ph2_ir->dest >= 0 && ph2_ir->dest < REG_CNT)
            reg_mirror_valid[ph2_ir->dest] = false;

        if (ph2_ir->op == OP_load && ph2_ir->dest >= 0 &&
            ph2_ir->dest < REG_CNT) {
            reg_mirror_valid[ph2_ir->dest] = true;
            reg_mirror_slot[ph2_ir->dest] = ph2_ir->src0;
            reg_mirror_size[ph2_ir->dest] = load_width(ph2_ir);
            reg_mirror_sext[ph2_ir->dest] = false;
        } else if (ph2_ir->op == OP_store) {
            /* Storing a register into the slot it already mirrors writes the
             * bytes that are there. Coalescing a phi with its operand makes
             * this common: the value's own write-back and the phi's copy become
             * the same store.
             */
            if (ph2_ir->src0 >= 0 && ph2_ir->src0 < REG_CNT &&
                !ph2_ir->is_volatile && reg_mirror_valid[ph2_ir->src0] &&
                reg_mirror_slot[ph2_ir->src0] == ph2_ir->src1) {
                int keep = ph2_ir->size_bytes;
                if (ph2_ir->is_pointer && keep != PTR_SIZE)
                    keep = PTR_SIZE;
                if (reg_mirror_size[ph2_ir->src0] == keep)
                    return;
            }

            /* The slot takes a new value, so any register mirroring it is now
             * stale.
             */
            for (int i = 0; i < REG_CNT; i++) {
                if (reg_mirror_valid[i] && reg_mirror_slot[i] == ph2_ir->src1)
                    reg_mirror_valid[i] = false;
            }

            /* The slot now holds the source register's low bytes, so that
             * register mirrors it. A full-width store leaves the two
             * bit-identical; a narrower one does not, and a later load of the
             * slot sign-extends what was written -- which the sext flag
             * records, so the reload becomes a MOVSX rather than a copy.
             */
            if (ph2_ir->src0 >= 0 && ph2_ir->src0 < REG_CNT) {
                int eff = ph2_ir->size_bytes;
                if (ph2_ir->is_pointer && eff != PTR_SIZE)
                    eff = PTR_SIZE;
                if (eff == 1 || eff == 2 || eff == 4 || eff == 8) {
                    reg_mirror_valid[ph2_ir->src0] = true;
                    reg_mirror_slot[ph2_ir->src0] = ph2_ir->src1;
                    reg_mirror_size[ph2_ir->src0] = eff;
                    reg_mirror_sext[ph2_ir->src0] = eff < 8;
                }
            }
        }
    }

    /* When the only consumer of a comparison is the branch right behind it, the
     * CMP's flags can drive Jcc directly: SETcc, MOVZX and TEST all go away,
     * and the boolean never needs to exist when no successor uses it.
     */
    int fuse_cc = branch_cc_for(
        ph2_ir->op, ph2_ir->src0_is_unsigned || ph2_ir->src1_is_unsigned);
    if (fuse_cc && branch_result_dead(ph2_ir->dest) && next_in_same_bb() &&
        emit_next_ir->op == OP_branch && emit_next_ir->src0 == ph2_ir->dest) {
        int producer = emit_ir_index - 1;
        if (!folds_off && producer >= 0 && src1_const_known &&
            src1_const == 0 && ph2_ir->op == OP_eq && cmp_mem_slot < 0 &&
            ph2_ir->src0 != ph2_ir->src1) {
            ph2_ir_t *previous = PH2_IR_FLATTEN[producer];
            if (previous->op == OP_load_constant &&
                previous->dest == ph2_ir->src1 && previous->src0 == 0 &&
                previous->src1 == 0)
                producer--;
        } else
            producer = -1;
        bool flags_ready =
            producer >= 0 &&
            instruction_to_bb[producer] == instruction_to_bb[emit_ir_index] &&
            PH2_IR_FLATTEN[producer]->op == OP_bit_and &&
            PH2_IR_FLATTEN[producer]->dest == ph2_ir->src0 &&
            PH2_IR_FLATTEN[producer]->size_bytes == ph2_ir->size_bytes &&
            (ph2_ir->size_bytes == 4 || ph2_ir->size_bytes == 8);
        if (!flags_ready)
            emit_cmp(ph2_ir->size_bytes, rs1, rs2);
        else
            cmp_imm_known = false;
        fused_cc = fuse_cc;
        fused_cc_pending = true;
        return;
    }

    /* "if (x & mask)" needs no destination register: TEST leaves exactly the
     * flags AND would, and the branch reads nothing else. This drops the AND,
     * the MOV that staged its destination, and the TEST the branch would
     * otherwise emit -- three instructions down to one. Only when the masked
     * value is dead afterwards, since TEST does not write it.
     */
    if (ph2_ir->op == OP_bit_and && next_in_same_bb() &&
        emit_next_ir->op == OP_branch && emit_next_ir->src0 == ph2_ir->dest &&
        !folds_off && branch_result_dead(ph2_ir->dest)) {
        bool wide = test_is_wide(ph2_ir->size_bytes, ph2_ir->is_pointer);

        if (src1_const_known) {
            emit_rex(wide, 0, rs1);
            emit_byte(0xF7); /* TEST rs1, imm32 */
            emit_byte(modrm(MOD_DIRECT, 0, reg_low3(rs1)));
            emit_dword(src1_const);
        } else {
            emit_opcode_reg(0x85, rs1, rs2, wide); /* TEST rs1, rs2 */
        }
        fused_cc = 0x85; /* JNZ */
        fused_cc_pending = true;
        return;
    }

    switch (ph2_ir->op) {
    case OP_load_constant: {
        /* A 64-bit constant whose high word is the sign of its low word is the
         * same value MOV r64, imm32 sign-extends to. Letting it take that path
         * also records it as a known constant, so a shift by it, or an add or
         * multiply with it, folds into an immediate instead of materialising
         * the value and shifting through CL.
         */
        if (ph2_ir->size_bytes == 8 &&
            ph2_ir->src1 != (ph2_ir->src0 < 0 ? -1 : 0)) {
            unsigned long long constant =
                (unsigned int) ph2_ir->src0 |
                ((unsigned long long) (unsigned int) ph2_ir->src1 << 32);

            emit_x64_constant(rd, true, constant); /* MOVABS r64, imm64 */
            return;
        }

        /* MOV r64, imm32 (sign-extended). The immediate is in src0, not a
         * register index.
         */
        if (ph2_ir->dest >= 0 && ph2_ir->dest < REG_CNT) {
            const_reg_valid[ph2_ir->dest] = true;
            const_reg_val[ph2_ir->dest] = ph2_ir->src0;

            /* Nothing will read the register itself: every remaining use turns
             * into a shift immediate, so the materialisation is dead.
             */
            if (const_load_dead(emit_ir_index + 1, ph2_ir->dest, ph2_ir->src0))
                return;
        }

        /* A 32-bit register write zero-extends to 64 bits, which is exactly the
         * representation an unsigned scalar needs before a logical shift or
         * unsigned comparison.
         */
        emit_mov_imm32(rd, ph2_ir->src0,
                       !(ph2_ir->is_unsigned && ph2_ir->size_bytes <= 4));
        return;
    }

    case OP_assign: {
        if (ph2_ir->is_unsigned && ph2_ir->size_bytes < PTR_SIZE)
            emit_zero_extend(rd, rs1, ph2_ir->size_bytes);
        else
            emit_mov_reg(rd, rs1);
        return;
    }

    case OP_add:
    case OP_sub:
    case OP_mul:
    case OP_div:
    case OP_mod:
        emit_arith(ph2_ir, rd, rs1, rs2, src1_const_known, src1_const);
        break;
    case OP_bit_and:
    case OP_bit_or:
    case OP_bit_xor:
    case OP_bit_not:
    case OP_negate:
    case OP_lshift:
    case OP_rshift:
        emit_bitwise(ph2_ir, rd, rs1, rs2, src1_const_known, src1_const);
        break;
    case OP_eq:
    case OP_neq:
    case OP_lt:
    case OP_leq:
    case OP_gt:
    case OP_geq:
    case OP_jump:
    case OP_branch:
        emit_compare_jump(ph2_ir, rd, rs1, rs2);
        break;
    case OP_call:
    case OP_return:
    case OP_func_ret:
        emit_call_return(ph2_ir, rs1);
        break;
    case OP_allocat:
    case OP_address_of:
    case OP_load:
    case OP_store:
        emit_memory(ph2_ir, rd, rs1);
        break;
    case OP_read:
    case OP_write:
    case OP_indirect:
        /* A store resets the constant table before it is emitted, so hand it
         * what its value register held.
         */
        write_imm_known = src1_const_known;
        write_imm_val = src1_const;
        emit_read_write(ph2_ir, rd, rs1, rs2);
        write_imm_known = false;
        break;
    case OP_load_func:
    case OP_address_of_func:
    case OP_global_address_of:
    case OP_global_load:
    case OP_global_store:
    case OP_load_data_address:
    case OP_load_rodata_address:
        emit_global(ph2_ir, rd, rs1);
        break;
    case OP_log_not:
    case OP_log_and:
    case OP_log_or:
    case OP_trunc:
    case OP_sign_ext:
    case OP_cast:
        emit_logic_cast(ph2_ir, rd, rs1);
        break;
    case OP_define:
        fatal_function_context = ph2_ir->func_name;
        /* Update the function's actual offset to current code position */
        {
            func_t *func = find_func(ph2_ir->func_name);
            if (func && func->bbs) {
                func->bbs->elf_offset = elf_code->size;
            }
        }

        /* Check if this is the main function and record its location */
        if (!strcmp(ph2_ir->func_name, "main")) {
            /* Store the actual location where main starts */
            MAIN_BB->elf_offset = elf_code->size;
            main_code_offset = elf_code->size;
        }

        /* Function prologue: PUSH rbp; MOV rbp, rsp; save callee-saved; SUB
         * rsp, aligned_size
         */
        emit_byte(0x55);           /* PUSH rbp */
        emit_reg_move(5, 4, true); /* MOV rbp, rsp */
        /* Preserve only the callee-saved registers this function touches,
         * naming them through the same mapping the allocator uses.
         */
        for (int cs = 0; cs < cur_saved_regs; cs++)
            emit_push_reg(map_ir_reg(X64_FIRST_CALLEE_SAVED + cs));
        /* R15 is reserved for global base pointer - don't save/restore */

        /* Allocate stack space if needed, keeping 16-byte alignment for calls
         */
        int stack_size = ph2_ir->src0;
        cur_define_stack = stack_size;
        if (x64_map) {
            fprintf(stderr, "[map] %s 0x%x\n", ph2_ir->func_name,
                    elf_code_start + elf_code->size);
            fflush(stderr);
        }
        emit_frame_adjust(5, stack_size);
        return;

    case OP_ternary:
        /* Ternary operator - implemented as conditional move pattern This is
         * typically lowered to branches in earlier phases For now, just move
         * rs1 to rd
         */
        emit_mov_reg(rd, rs1);
        return;

    case OP_push:
        /* PUSH r64 - for now just push on stack */
        if (rs1 >= 8) {
            emit_byte(REX_B); /* Need REX prefix for r8-r15 */
        }
        emit_byte(0x50 + reg_low3(rs1));
        return;

    default:
        /* Fail the way arm-codegen.c and riscv-codegen.c do. Emitting a NOP and
         * continuing turned a gap in the backend into a silently wrong binary
         * with exit status 0.
         */
        fatal("Unknown opcode");
    }
}

/* Flatten control flow graph for code generation */
void cfg_flatten(void)
{
    /* __syscall offset will be set when it's actually emitted in code_generate
     */
    func_t *func;
    ph2_ir_prepare(true);

    /* Entry point code varies based on globals: POP rdi (1) + MOV rsi,rsp (3) +
     * global_setup (varies) + CALL globals (0 or 5) + CALL main (5) + MOV
     * rdi,rax (3) + MOV rax,60 (5) + SYSCALL (2) Global setup with zeroing: MOV
     * rax,imm32 (7) + SUB rsp,rax (3) + MOV r15,rsp (4) + MOV rdi,r15 (3) + MOV
     * rcx,imm32 (6) + XOR rax,rax (3) + REP STOSQ (3) = 29 bytes Without
     * globals: MOV r15,0 (7) = 7 bytes
     */
    int global_setup_size;
    if (GLOBAL_FUNC && GLOBAL_FUNC->stack_size > 0)
        global_setup_size = 29;
    else
        global_setup_size = 7;
    int global_call_size;
    if (GLOBAL_FUNC && GLOBAL_FUNC->bbs && GLOBAL_FUNC->bbs->ph2_ir_list.head)
        global_call_size = 5;
    else
        global_call_size = 0;

    /* Entry code size: POP rdi (1) + MOV rsi,rsp (3) + save argc/argv (6)
     * + global setup + optional CALL globals (5) + restore argc/argv (6) + CALL
     * main (5)
     * + MOV rdi,rax (3) + MOV rax,60 (7) + SYSCALL (2)
     */
    elf_offset = (1 + 3 + 6) + global_setup_size + global_call_size + 6 + 5 +
                 (3 + 7 + 2);
    if (GLOBAL_FUNC && GLOBAL_FUNC->bbs) {
        /* Global function starts immediately after entry sequence */
        GLOBAL_FUNC->bbs->elf_offset = elf_offset;
    }

    /* Account for the RET at the end of the global function */
    if (GLOBAL_FUNC && GLOBAL_FUNC->bbs)
        elf_offset += 1;

    /* Entry already fully accounted for above. No extra fudge needed. */

    for (func = FUNC_LIST.head; func; func = func->next) {
        if (!ph2_ir_function_included(func, true))
            continue;

        /* Arguments past MAX_ARGS_IN_REG stay in the caller's frame, and
         * machine lowering records their offsets relative to it. Convert those
         * to offsets from this function's RSP, which sits below everything the
         * prologue pushed:
         *
         *   RSP + aligned locals        -> saved r13, r12, rbx, rbp (32 bytes)
         *   RSP + aligned locals + 32   -> return address pushed by CALL
         *   RSP + aligned locals + 40   -> the caller's first stacked argument
         *
         * The rounding has to match the prologue's SUB exactly, otherwise every
         * stacked argument is read from the wrong slot.
         */
        func->saved_regs = func_saved_regs(func);
        int stack_top_ofs =
            x64_incoming_arg_base(func->stack_size, func->saved_regs);

        /* Reserve stack: account prologue via OP_define */
        ph2_ir_t *flatten_ir = add_ph2_ir(OP_define);
        flatten_ir->src0 = func->stack_size;
        flatten_ir->func_name = intern_string(func->return_def.var_name);

        for (basic_block_t *bb = func->bbs; bb; bb = bb->rpo_next) {
            /* Do not preset BB offsets here; set them at emission time when we
             * enter the BB to avoid stale offsets.
             */

            for (ph2_ir_t *insn = bb->ph2_ir_list.head; insn;
                 insn = insn->next) {
                if (insn->ofs_based_on_stack_top) {
                    /* Only these three name a variable's address; every other
                     * opcode carrying the flag has nothing to rebase.
                     */
                    if (insn->op == OP_load || insn->op == OP_address_of)
                        insn->src0 = insn->src0 + stack_top_ofs;
                    else if (insn->op == OP_store)
                        insn->src1 = insn->src1 + stack_top_ofs;
                }

                flatten_ir = add_existed_ph2_ir(insn);

                if (insn->op == OP_return) {
                    /* restore sp */
                    flatten_ir->src1 = bb->belong_to->stack_size;
                }
            }
        }
    }
}

/* Generate x86-64 machine code */

/* Build the procedure linkage table.
 *
 * PLT[0] is the trampoline into the resolver:
 *     push QWORD PTR [rip + d]   ; GOT[1], the link map
 *     jmp  QWORD PTR [rip + d]   ; GOT[2], the resolver
 * PLT[n] calls the function through its GOT slot, which the loader has pointed
 * back at the push below until the symbol is resolved: jmp QWORD PTR [rip + d]
 * ; GOT[n] push n - 1 ; relocation index jmp PLT[0]
 *
 * RIP-relative displacements are measured from the end of the instruction they
 * appear in, so every target below subtracts the address just past it.
 */
void plt_generate(void)
{
    int plt_start = dynamic_sections.elf_plt_start;
    int got_start = dynamic_sections.elf_got_start;
    int entries = (dynamic_sections.plt_size - PLT_FIXUP_SIZE) / PLT_ENT_SIZE;

    /* PLT[0] */
    elf_write_byte(dynamic_sections.elf_plt, 0xFF);
    elf_write_byte(dynamic_sections.elf_plt, 0x35); /* push [rip + d] */
    elf_write_int(dynamic_sections.elf_plt,
                  got_start + PTR_SIZE - (plt_start + 6));
    elf_write_byte(dynamic_sections.elf_plt, 0xFF);
    elf_write_byte(dynamic_sections.elf_plt, 0x25); /* jmp [rip + d] */
    elf_write_int(dynamic_sections.elf_plt,
                  got_start + PTR_SIZE * 2 - (plt_start + 12));
    elf_write_byte(dynamic_sections.elf_plt, 0x0F); /* 4-byte nop */
    elf_write_byte(dynamic_sections.elf_plt, 0x1F);
    elf_write_byte(dynamic_sections.elf_plt, 0x40);
    elf_write_byte(dynamic_sections.elf_plt, 0x00);

    for (int i = 0; i < entries; i++) {
        int ent = plt_start + PLT_FIXUP_SIZE + i * PLT_ENT_SIZE;
        int got_slot = got_start + PTR_SIZE * (RESERVED_GOT_NUM + i);

        elf_write_byte(dynamic_sections.elf_plt, 0xFF);
        elf_write_byte(dynamic_sections.elf_plt, 0x25); /* jmp [rip + d] */
        elf_write_int(dynamic_sections.elf_plt, got_slot - (ent + 6));

        elf_write_byte(dynamic_sections.elf_plt, 0x68); /* push imm32 */
        elf_write_int(dynamic_sections.elf_plt, i);

        elf_write_byte(dynamic_sections.elf_plt, 0xE9); /* jmp rel32 */
        elf_write_int(dynamic_sections.elf_plt, plt_start - (ent + 16));
    }
}

void code_generate(void)
{
    arena_mark_t instruction_to_bb_mark;
    int instruction_to_bb_bytes;

    src0_override = -1;
    src0_override_at = -1;
    cmp_mem_slot = -1;
    addr_fold = false;
    addr_sib = false;
    skip_ir_reset();

    /* Where the dynamic-mode main wrapper's operands sit, filled in once the
     * final layout fixes the global base and main's offset.
     */
    int main_wrapper_ofs = 0, wrapper_r15_patch = 0, wrapper_jmp_patch = 0;

    /* Reset forward references for this compilation unit */
    if (x64_debug)
        fprintf(stderr, "[x64] codegen begin\n");
    forward_ref_count = 0;

    /* Block offsets are filled in as the code is emitted, and every section
     * address is recomputed from the real code size once it is. Nothing is
     * placed from an estimate here: x86-64 instructions are variable length, so
     * an estimate would be wrong for every section that followed it.
     */

    /* Generate code for the global function */
    ph2_ir_t *ph2_ir;
    if (GLOBAL_FUNC && GLOBAL_FUNC->bbs) {
        /* Record where the global initializer actually starts. cfg_flatten only
         * computed a predicted offset, and x86-64 instruction lengths are
         * variable, so the prediction does not survive; the entry stub's CALL
         * is patched from this value.
         */
        GLOBAL_FUNC->bbs->elf_offset = elf_code->size;

        for (ph2_ir = GLOBAL_FUNC->bbs->ph2_ir_list.head; ph2_ir;
             ph2_ir = ph2_ir->next) {
            if (x64_debug)
                fprintf(stderr, "[x64] emit global op=%d\n", ph2_ir->op);

            /* The initializer runs through a CALL with no frame of its own, and
             * the slots it spills temporaries to are allocated from the global
             * data area, which sits behind R15 on this target rather than on
             * the stack as on the others. Addressing them from RSP wrote past
             * the top of the stack once an initializer was large enough to
             * spill.
             */
            if (ph2_ir->op == OP_load)
                ph2_ir->op = OP_global_load;
            else if (ph2_ir->op == OP_store)
                ph2_ir->op = OP_global_store;
            else if (ph2_ir->op == OP_address_of)
                ph2_ir->op = OP_global_address_of;
            emit_next_ir = NULL;
            emit_ph2_ir(ph2_ir);
        }

        /* Add RET instruction at end of global function */
        emit_byte(0xC3); /* RET */
    }

    /* Generate __syscall function implementation __syscall takes syscall number
     * in first arg and up to 6 additional args x64 Linux syscall convention:
     * - syscall number in RAX
     * - args in RDI, RSI, RDX, R10, R8, R9 Our calling convention passes args
     * in:
     * - RDI (syscall#), RSI (arg1), RDX (arg2), RCX (arg3), R8 (arg4), R9
     * (arg5)
     */
    int syscall_func_offset = elf_code->size;

    /* Find __syscall function and update its offset */
    func_t *syscall_func = find_func("__syscall");
    if (syscall_func && syscall_func->bbs) {
        syscall_func->bbs->elf_offset = syscall_func_offset;
    }

    /* WORKAROUND: Fix missing initialization for offset 0x14 The global
     * variable at R15+0x14 is not being initialized properly. We'll patch this
     * by modifying the entry point code to initialize it.
     */

    /* DEBUG: mark entering __syscall - DISABLED due to stack corruption Writing
     * to (%rsp) corrupts the return address since __syscall has no frame
     * __syscall mapping from our compiler's calling convention Caller provides
     * (per reg_map): RDI=sysno, RSI=a1, RDX=a2, RCX=a3, R8=a4, R9=a5 Map to
     * Linux x86-64 syscall convention: RAX=sysno, RDI=a1, RSI=a2, RDX=a3,
     * R10=a4, R8=a5, R9=a6 (unused)
     */

    /* Map shecc's calling convention onto the Linux x86-64 syscall ABI.
     *
     * Incoming (reg_map order, MAX_ARGS_IN_REG = 6, seventh on the frame):
     *     RDI=sysno RSI=a1 RDX=a2 RCX=a3 R8=a4 R9=a5, a6 at [rsp + 8]
     * Required by the kernel:
     *     RAX=sysno RDI=a1 RSI=a2 RDX=a3 R10=a4 R8=a5 R9=a6
     *
     * The syscall number is parked in R11 first so that RAX is free to take it
     * back at the end, and each register is read before it is overwritten. Six
     * arguments are needed in full for mmap(2).
     */
    /* MOV r11, rdi (save sysno) */
    emit_reg_move(11, 7, true);
    /* MOV rdi, rsi (a1) */
    emit_reg_move(7, 6, true);
    /* MOV rsi, rdx (a2) */
    emit_reg_move(6, 2, true);
    /* MOV rdx, rcx (a3) */
    emit_reg_move(2, 1, true);
    /* MOV r10, r8 (a4) */
    emit_reg_move(10, 8, true);
    /* MOV r8, r9 (a5) */
    emit_reg_move(8, 9, true);

    /* MOV r9, [rsp + 8] (a6)
     *
     * The seventh argument does not travel in a register: the file holds six,
     * and everything past them goes to the caller's frame, where the call left
     * it just above the return address. Reading RAX here instead worked only
     * while RAX happened to hold zero, which is the offset every mmap(2) shecc
     * makes asks for -- until a caller left something else there.
     */
    emit_byte(REX_W | REX_R);
    emit_byte(0x8B);
    emit_byte(modrm(MOD_DISP8, 1, 4));
    emit_byte(0x24); /* SIB: base RSP, no index */
    emit_byte(8);
    /* MOV rax, r11 (sysno) */
    emit_reg_move(0, 11, true);

    /* SYSCALL */
    emit_byte(0x0F);
    emit_byte(0x05);

    /* DEBUG: mark leaving __syscall - DISABLED due to stack corruption RET */
    emit_byte(0xC3);

    /* Emit all flattened IR - this includes all functions We need to track and
     * update basic block offsets as we emit them Track current function and
     * basic block during emission
     */
    func_t *emit_func = NULL;
    basic_block_t *emit_bb = NULL;

    /* The last block the walk emitted. Unlike emit_bb it survives the branch
     * that ends a block, so the block falling out of that branch can tell that
     * it is the one being fallen into.
     */
    basic_block_t *prev_emitted_bb = NULL;

    /* Keep track of all BBs we encounter to set their offsets */

    /* Track each flattened instruction's block. One sentinel slot lets bounded
     * scans inspect the end-of-stream entry without reserving a maximum-sized
     * map for every compilation.
     */
    if (ph2_ir_idx == INT_MAX)
        limit_error("too many phase-2 IR instructions for x64 map");
    instruction_to_bb_capacity = ph2_ir_idx + 1;
    if (instruction_to_bb_capacity > (INT_MAX - ((int) sizeof(void *) - 1)) /
                                         (int) sizeof(*instruction_to_bb))
        limit_error("x64 instruction-to-block map size overflow");
    instruction_to_bb_bytes =
        instruction_to_bb_capacity * (int) sizeof(*instruction_to_bb);
    instruction_to_bb_mark = arena_mark(GENERAL_ARENA);
    instruction_to_bb = arena_alloc(GENERAL_ARENA, instruction_to_bb_bytes);
    if (!instruction_to_bb)
        limit_error("cannot allocate x64 instruction-to-block map");
    instruction_to_bb[ph2_ir_idx] = NULL;

    /* Build the mapping in the same order as cfg_flatten. */
    int instr_idx = 0;
    for (func_t *func = FUNC_LIST.head; func; func = func->next) {
        if (!ph2_ir_function_included(func, true))
            continue;
        /* OP_define for the function; it belongs to no BB */
        if (instr_idx < ph2_ir_idx)
            instruction_to_bb[instr_idx] = NULL;
        instr_idx++;

        int blocks_base = instr_idx;
        for (basic_block_t *bb = func->bbs; bb; bb = bb->rpo_next) {
            bb->ph2_base = bb->ph2_ir_list.head ? instr_idx : -1;

            /* Map all instructions in this BB */
            for (ph2_ir_t *insn = bb->ph2_ir_list.head; insn;
                 insn = insn->next) {
                if (instr_idx < ph2_ir_idx)
                    instruction_to_bb[instr_idx] = bb;
                instr_idx++;
            }
        }

        /* Record which blocks an edge actually jumps to, so the rest are known
         * to be reachable only by falling out of the block before them.
         *
         * A conditional branch's taken side is always a jump; its other side is
         * a jump only when the walk does not place it next. An unconditional
         * jump names its target -- and when that target is a loop header the
         * walk rotates, the copy emitted inline repeats the header's branch, so
         * both of the header's successors are jumped to as well.
         */
        int mark_idx = blocks_base;
        for (basic_block_t *bb = func->bbs; bb; bb = bb->rpo_next) {
            for (ph2_ir_t *insn = bb->ph2_ir_list.head; insn;
                 insn = insn->next) {
                basic_block_t *t;

                mark_idx++;
                if (insn->op == OP_branch) {
                    t = bb_code_target(insn->then_bb);
                    if (t)
                        t->is_branch_target = true;
                    t = bb_code_target(insn->else_bb);
                    if (t && t->ph2_base != mark_idx)
                        t->is_branch_target = true;
                    continue;
                }
                if (insn->op != OP_jump)
                    continue;
                t = bb_code_target(insn->next_bb);
                if (!t)
                    continue;
                if (t->ph2_base != mark_idx)
                    t->is_branch_target = true;
                ph2_ir_t *tail = t->ph2_ir_list.tail;
                if (tail && tail->op == OP_branch) {
                    basic_block_t *d = bb_code_target(tail->then_bb);
                    if (d)
                        d->is_branch_target = true;
                    d = bb_code_target(tail->else_bb);
                    if (d)
                        d->is_branch_target = true;
                }
            }
        }
    }

    if (instr_idx != ph2_ir_idx)
        fatal("x64: flattened IR and block map counts disagree");

    if (x64_map) {
        fprintf(stderr, "[count] flattened IR = %d (map slots %d)\n",
                ph2_ir_idx, instruction_to_bb_capacity);
        fflush(stderr);
    }
    for (int i = 0; i < ph2_ir_idx; i++) {
        ph2_ir = PH2_IR_FLATTEN[i];
        emit_next_ir = i + 1 < ph2_ir_idx ? PH2_IR_FLATTEN[i + 1] : NULL;
        emit_ir_index = i;

        /* Skip __syscall - it's emitted manually early (before other functions)
         */
        if (ph2_ir->op == OP_define &&
            !strcmp(ph2_ir->func_name, "__syscall")) {
            continue;
        }

        /* Check if we're starting a new function. Its entry block is made
         * current here, so the check at a block's start would not see it.
         */
        if (ph2_ir->op == OP_define) {
            fold_state_check(i);
            emit_func = find_func(ph2_ir->func_name);
            cur_saved_regs = emit_func ? emit_func->saved_regs : 1;
            if (emit_func && emit_func->bbs) {
                emit_bb = emit_func->bbs;

                /* Don't update offset here - it was already set in OP_define
                 * BEFORE the prologue was emitted
                 */
            }
            if (x64_debug)
                fprintf(stderr, "[x64] define %s\n", ph2_ir->func_name);
        }

        /* Check if this instruction starts a new BB */
        if (i < instruction_to_bb_capacity && instruction_to_bb[i]) {
            basic_block_t *current_instr_bb = instruction_to_bb[i];

            /* If this is the first instruction of a BB and offset not set, set
             * it now
             */
            if (current_instr_bb != emit_bb) {
                fold_state_check(i);

                /* A loop header is the hottest branch target in a function and
                 * is reached from a backward edge. Starting it on a 16-byte
                 * boundary keeps it off a shared predictor slot; without this,
                 * an unrelated change in code size upstream can triple the
                 * branch misses of a tight loop.
                 */
                if (bb_is_loop_header(current_instr_bb))
                    emit_nop_bytes((16 - (elf_code->size & 15)) & 15);

                /* Entering a new basic block: record where it starts. A block
                 * can be reached from several predecessors, so nothing learned
                 * about register contents in the preceding block carries in.
                 */
                current_instr_bb->elf_offset = elf_code->size;

                /* What the registers hold survives only when control can reach
                 * this block exactly one way: out of the block just emitted.
                 * Any join point has to start from nothing.
                 *
                 * The block just emitted is tracked separately from emit_bb,
                 * which is cleared after a branch. The block a conditional
                 * branch falls into is still reached only by falling out of
                 * that branch's block, so it keeps what the registers hold; the
                 * taken side is a jump target and does not.
                 */
                if (!emit_fell_through || current_instr_bb->is_branch_target ||
                    bb_sole_code_pred(current_instr_bb) != prev_emitted_bb) {
                    frame_mirror_reset();
                    const_track_reset();
                    shift_cache_reset();
                }
                emit_bb = current_instr_bb;
                prev_emitted_bb = current_instr_bb;
                fused_cc_pending = false;
                src0_override = -1;
                src0_override_at = -1;
                cmp_mem_slot = -1;
                addr_fold = false;
                addr_sib = false;
                skip_ir_reset();
            }
        }

        /* After a branch/jump, track BB changes */
        if (emit_bb && (ph2_ir->op == OP_branch || ph2_ir->op == OP_jump)) {
            /* The next instruction will be in a different BB */
            emit_bb = NULL;
        }

        /* (removed debug tracing) */

        /* (removed debug context dump) */

        /* Emission-time assertion: pointer stores must be PTR_SIZE wide */
        if ((ph2_ir->op == OP_store || ph2_ir->op == OP_write) &&
            ph2_ir->is_pointer && ph2_ir->size_bytes != PTR_SIZE) {
            /* Split across two calls: nine arguments exceeds MAX_PARAMS, so the
             * last one was garbage in a self-hosted build.
             */
            if (x64_debug) {
                char *fname = "<unknown>";
                if (emit_func)
                    fname = emit_func->return_def.var_name;
                fprintf(stderr,
                        "WARN: pointer store width mismatch at IR idx %d in "
                        "%s: op=%d size=%d\n",
                        i, fname, ph2_ir->op, ph2_ir->size_bytes);
                fprintf(stderr, "      src0=%d src1=%d dest=%d\n", ph2_ir->src0,
                        ph2_ir->src1, ph2_ir->dest);
            }
            /* Continue without aborting */
        }
        if (x64_debug)
            fprintf(stderr, "[x64] emit op=%d\n", ph2_ir->op);
        emit_fell_through = true;
        emit_ph2_ir(ph2_ir);
        if (x64_debug)
            fprintf(stderr, "[x64] done op=%d\n", ph2_ir->op);
    }
    fold_state_check(ph2_ir_idx);

    /* BB offsets from cfg_flatten should now be available for forward reference
     * patching
     */

    /* Emit any basic blocks that were referenced but never emitted This can
     * happen for blocks only reachable by forward jumps
     */

    /* Emit any block the walk above never placed. Asking the blocks themselves
     * costs one pass over the CFG and cannot miss one, where collecting
     * candidates as they were referenced pushed a block once per instruction
     * naming it and silently dropped the rest once the list filled.
     */
    for (func_t *lf = FUNC_LIST.head; lf; lf = lf->next) {
        for (basic_block_t *bb = lf->bbs; bb; bb = bb->rpo_next) {
            if (bb->elf_offset >= 0)
                continue;

            /* At this point, bb is non-null and has offset < 0 Count
             * instructions in this BB
             */
            int insn_count = 0;
            for (ph2_ir_t *ir = bb->ph2_ir_list.head; ir; ir = ir->next) {
                insn_count++;
            }

            if (insn_count == 0) {
                /* Empty and unreached: leave it unplaced. A forward jump to it
                 * is an error that patching will catch.
                 */
            } else {
                /* This BB has instructions - emit them */
                if (x64_debug)
                    fprintf(stderr, "[x64] late-emit unreached BB at 0x%x\n",
                            elf_code->size);
                bb->elf_offset = elf_code->size;

                /* Nothing reaches this block by falling into it -- it is placed
                 * here only because the walk never got to it -- so it starts
                 * from nothing.
                 */
                frame_mirror_reset();
                const_track_reset();
                shift_cache_reset();

                /* Emit all instructions in this BB */
                folds_off = true;
                for (ph2_ir_t *ir = bb->ph2_ir_list.head; ir; ir = ir->next) {
                    emit_next_ir = NULL;
                    emit_ph2_ir(ir);
                }
                folds_off = false;
            }
        }
    }

    /* Track R15 patch offset if entry is emitted at the end */
    int r15_patch_offset = 0;
    {
        /* Emit the entry stub at the end, where main's offset is known.
         * __libc_start_main does not return -- it calls main -- so the R15 this
         * stub sets up holds glibc's value by the time main runs, even though
         * R15 is callee-saved. Interpose a small wrapper that restores the
         * global base and tail-calls main, and hand that to the runtime
         * instead. The address is patched once main's offset is known.
         */
        if (dynlink) {
            main_wrapper_ofs = elf_code->size;
            emit_byte(0x49); /* movabs r15, imm64 */
            emit_byte(0xBF);
            wrapper_r15_patch = elf_code->size;
            emit_dword(0);
            emit_dword(0);
            emit_byte(0xE9); /* jmp rel32 -> main, preserving the arguments */
            wrapper_jmp_patch = elf_code->size;
            emit_dword(0);
        }

        int entry_start = elf_code->size;
        elf_entry_offset = entry_start; /* Entry relative to code start */

        /* Debug: check if any BB has this offset */
        for (func_t *func = x64_debug ? FUNC_LIST.head : NULL; func;
             func = func->next) {
            for (basic_block_t *bb = func->bbs; bb; bb = bb->rpo_next) {
                if (bb->elf_offset == entry_start) {
                    fprintf(stderr,
                            "[x64] ERROR: a basic block shares the entry "
                            "stub's offset (0x%x); jumps to it will land on "
                            "the entry instead\n",
                            entry_start);
                }
            }
        }

        /* Removed debug output that was corrupting file writes POP rdi (argc)
         */
        emit_byte(0x5F);
        /* MOV rsi, rsp (argv) */
        emit_reg_move(6, 4, true);

        /* Initialize R15 to point to global data area before calling globals
         * MOV r15, imm64 - Load absolute address of data section We'll patch
         * this later when we know the final data address
         */
        emit_byte(0x49); /* REX.W + REX.B */
        emit_byte(0xBF); /* MOV r15, imm64 */
        r15_patch_offset = elf_code->size;
        emit_dword(0); /* Lower 32 bits - will be patched */
        emit_dword(0); /* Upper 32 bits - will be patched */

        /* Globals setup: if any */
        if (GLOBAL_FUNC && GLOBAL_FUNC->bbs &&
            GLOBAL_FUNC->bbs->ph2_ir_list.head) {
            /* The generated global initializer is not an ABI boundary: it may
             * allocate r12/r13, so do not preserve argv in callee-saved
             * registers across it. Keep both values on the startup stack and
             * pad to the SysV call alignment instead.
             */
            emit_byte(0x57);                   /* push rdi (argc) */
            emit_byte(0x56);                   /* push rsi (argv) */
            emit_alu_imm_width(4, 5, 8, true); /* sub rsp, 8 */
            emit_byte(0xE8);
            int patch_at_g = elf_code->size;
            int disp_g = GLOBAL_FUNC->bbs->elf_offset - (patch_at_g + 4);
            emit_dword(disp_g);
            emit_alu_imm_width(4, 0, 8, true); /* add rsp, 8 */
            emit_byte(0x5E);                   /* pop rsi (argv) */
            emit_byte(0x5F);                   /* pop rdi (argc) */
        }
        if (dynlink) {
            /* Hand control to the C runtime:
             *   __libc_start_main(main, argc, argv, 0, 0, 0, stack_end)
             * argc is in RDI and argv in RSI at this point, so both shift one
             * register along to make room for main in RDI.
             */
            emit_reg_move(2, 6, true); /* argv -> rdx */
            emit_reg_move(6, 7, true); /* argc -> rsi */
            emit_byte(REX_W);
            emit_byte(0xBF); /* movabs rdi, main_wrapper */
            emit_dword(elf_code_start + main_wrapper_ofs);
            emit_dword(0);
            /* init, fini and rtld_fini are unused by modern glibc. */
            emit_opcode_reg(0x31, 1, 1, false); /* xor ecx,ecx */
            emit_opcode_reg(0x31, 8, 8, false); /* xor r8d,r8d */
            emit_opcode_reg(0x31, 9, 9, false); /* xor r9d,r9d */
            /* Align, then push a pad and stack_end so RSP is 16-byte aligned at
             * the call, as the ABI requires.
             */
            emit_alu_imm_width(4, 4, -16, true); /* and rsp, -16 */
            emit_byte(0x50);                     /* push rax  (pad) */
            emit_byte(0x54);                     /* push rsp  (stack_end) */

            emit_byte(0xE8); /* call PLT[1], reserved for __libc_start_main */
            add_pltcall_ref(PLT_FIXUP_SIZE);
            emit_byte(0xF4); /* hlt: __libc_start_main does not return */
        } else {
            /* Align stack by 8 for call */
            emit_alu_imm_width(4, 5, 8, true);
            /* CALL main */
            emit_byte(0xE8);
            {
                /* Safer call target: function entry */
                int patch_at_m = elf_code->size;

                /* Written as statements rather than a conditional expression:
                 * folding the pointer test into the initialiser produced 0 here
                 * when shecc compiled itself, so the entry stub called the
                 * global initialiser instead of main.
                 */
                int main_off = main_code_offset;
                int disp_m = main_off - (patch_at_m + 4);
                emit_dword(disp_m);
            }
            /* Undo stack adjust */
            emit_alu_imm_width(4, 0, 8, true);
            /* Move exit code (rax) to rdi and syscall exit(60) */
            emit_reg_move(7, 0, true);
            emit_mov_imm32(0, 60, true);
            emit_byte(0x0F);
            emit_byte(0x05);
        }

        /* Dump first bytes of entry stub for diagnostics */
    }

    /* Recalculate elf_data_start using the final code size */
    /* ELF64 layout: the first load segment holds the headers, .text and
     * .rodata; the second holds .data (plus .bss). .rodata cannot sit next to
     * .data because the global variable slots R15 addresses extend past .data's
     * file content into .bss, and would collide with it.
     *
     * The second segment must satisfy the ELF constraint
     *     p_vaddr === p_offset  (mod p_align)
     * Starting it at the next page boundary in the file and deriving its
     * virtual address from that same boundary satisfies it by construction,
     * whatever the final code size turned out to be.
     */
    if (dynlink) {
        /* Everything after .text is placed by address arithmetic, and the GOT
         * at the end of that chain must be pointer-aligned. Padding .text and
         * .rodata to a pointer boundary keeps every following section aligned
         * without special-casing each one.
         */
        elf_align_to(elf_code, PTR_SIZE);
        elf_align_to(elf_rodata, PTR_SIZE);
    }

    elf_rodata_start = elf_code_start + elf_code->size;

    if (dynlink) {
        /* elf_preprocess() placed these from cfg_flatten's estimate of the code
         * size. x86-64 instructions are variable length, so that estimate is
         * not the final size and every dynamic address derived from it has
         * moved. Re-run the same layout, now that the code is emitted, and
         * rebuild the sections that encode those addresses.
         */
        elf_layout_dynamic();

        elf_reset_dynamic_sections();
        elf_generate_dynamic_sections();
        plt_generate();

        elf_data_start =
            elf_dynamic_start() + dynamic_sections.elf_dynamic->size;

        /* Fill in the wrapper: the global base it restores and the jump to main
         * are both only known now.
         */
        {
            int at = wrapper_r15_patch;
            int base = elf_data_start;
            patch_qword(at, base);

            int jat = wrapper_jmp_patch;
            int jdisp = main_code_offset - (jat + 4);
            patch_dword(jat, jdisp);
        }

        /* The PLT's address is settled, so every call into it can be resolved
         * now.
         */
        for (int i = 0; i < pltcall_ref_count; i++) {
            int at = pltcall_refs[i].patch_location;
            int target =
                dynamic_sections.elf_plt_start + pltcall_refs[i].plt_offset;
            int disp = target - (elf_code_start + at + 4);
            patch_dword(at, disp);
        }

        elf_bss_start = elf_data_start + elf_data->size;
    } else {
        int ro_bytes = elf_header_len + elf_code->size + elf_rodata->size;
        int data_file_ofs = ALIGN_UP(ro_bytes, PAGESIZE);
        elf_data_start = ELF_START + data_file_ofs;
        elf_bss_start = elf_data_start + elf_data->size;
    }

    /* .rodata's address is only known now that the code size is final, so fill
     * in every MOVABS that referenced it.
     */
    for (int i = 0; i < funcaddr_ref_count; i++) {
        int at = funcaddr_refs[i].patch_location;
        int addr;
        if (funcaddr_refs[i].target_bb)
            addr = elf_code_start + funcaddr_refs[i].target_bb->elf_offset;
        else
            addr = dynamic_sections.elf_plt_start + funcaddr_refs[i].plt_offset;
        patch_qword(at, addr);
    }

    for (int i = 0; i < rodata_ref_count; i++) {
        int at = rodata_refs[i].patch_location;
        int addr = elf_rodata_start + rodata_refs[i].rodata_offset;
        patch_qword(at, addr);
    }

    /* Set BSS size to accommodate global variables */
    if (GLOBAL_FUNC) {
        elf_bss_size = GLOBAL_FUNC->stack_size;
    }

    /* Patch R15 initialization with the actual data address if we emitted it */
    if (r15_patch_offset > 0) {
        /* Write the 64-bit address of the data section The data section's
         * runtime address is elf_data_start (which includes load address)
         */
        patch_qword(r15_patch_offset, elf_data_start);
    }

    /* No RIP-relative patching needed when using RSP-based globals */

    /* Dump first CF trace entries (guarded) */

    /* Resolve basic blocks that emitted no code.
     *
     * An empty basic block contributes zero bytes, so it never appears in the
     * flattened instruction stream and never receives an offset during
     * emission. Its address is simply that of the next block in reverse
     * post-order that did emit code; a run of consecutive empty blocks all
     * collapse onto that same address. Branches to such a block are legal and
     * must be patched, so fill these in before patching forward references.
     */
    for (func_t *rf = FUNC_LIST.head; rf; rf = rf->next) {
        for (basic_block_t *bb = rf->bbs; bb; bb = bb->rpo_next) {
            if (bb->elf_offset >= 0)
                continue;
            int resolved = -1;
            for (basic_block_t *nx = bb->rpo_next; nx; nx = nx->rpo_next) {
                if (nx->elf_offset >= 0) {
                    resolved = nx->elf_offset;
                    break;
                }
            }

            /* A trailing run of empty blocks falls off the end of the function;
             * its address is the current end of the code section.
             */
            if (resolved < 0)
                resolved = elf_code->size;
            bb->elf_offset = resolved;
        }
    }

    /* Patch all forward references that have valid targets */
    for (int i = 0; i < forward_ref_count; i++) {
        forward_ref_t *ref = &forward_refs[i];

        if (!ref->target_bb || ref->patch_location < 0 ||
            ref->patch_location + 4 > elf_code->size) {
            continue;
        }

        /* Check if this patch would overwrite the entry stub */
        if (x64_debug && elf_entry_offset > 0 &&
            ref->patch_location >= elf_entry_offset &&
            ref->patch_location < elf_entry_offset + 64) {
            fprintf(stderr,
                    "[x64] WARNING: Forward ref %d would overwrite entry "
                    "stub! patch_loc=0x%x, entry_offset=0x%x\n",
                    i, ref->patch_location, elf_entry_offset);
        }

        int target_offset = ref->target_bb->elf_offset;
        /* Debug check for invalid offsets */
        if (target_offset < 0) {
            fprintf(stderr,
                    "[x64] ERROR: BB %d was never emitted (offset=%d)! "
                    "Skipping patch at 0x%x\n",
                    i, target_offset, ref->patch_location);
            continue;
        }

        /* elf_code->size is a legal target: a trailing run of empty basic
         * blocks resolves to the current end of the code section, and a branch
         * there simply leaves the region. Only offsets past that are genuinely
         * out of range.
         */
        if (target_offset > elf_code->size) {
            fprintf(stderr,
                    "[x64] ERROR: BB %d has offset 0x%x > code size 0x%x!\n", i,
                    target_offset, elf_code->size);
            /* Skip this patch - it's wrong */
            continue;
        }

        /* Both out-of-range cases already skipped this entry above, so the
         * offset is known good here.
         */
        int relative_offset = target_offset - (ref->patch_location + 4);

        /* Debug: check for suspiciously large offsets */
        if (x64_debug && relative_offset > 0x10000) {
            fprintf(stderr,
                    "[x64] WARNING: Large relative offset 0x%x at "
                    "patch_loc=0x%x to target=0x%x\n",
                    relative_offset, ref->patch_location, target_offset);
        }

        patch_dword(ref->patch_location, relative_offset);
    }

    /* Debug: dump first bytes of main entry to help diagnose crashes */
    if (x64_debug && MAIN_BB && MAIN_BB->elf_offset >= 0 &&
        MAIN_BB->elf_offset < elf_code->size) {
        int start = MAIN_BB->elf_offset;
        int end = start + 32;
        if (end > elf_code->size)
            end = elf_code->size;
        fprintf(stderr, "[x64] main head @%d: ", start);
        for (int i = start; i < end; i++)
            fprintf(stderr, "%02x ", elf_code->elements[i] & 0xFF);
        fprintf(stderr, "\n");
    }
    if (arena_bytes_since(GENERAL_ARENA, instruction_to_bb_mark) ==
        instruction_to_bb_bytes)
        arena_rewind(GENERAL_ARENA, instruction_to_bb_mark);
    instruction_to_bb = NULL;
    instruction_to_bb_capacity = 0;
}
