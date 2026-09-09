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

/* Registers the allocator is holding a variable in for the whole of the
 * function being emitted, one bit each. Such a register is live everywhere, so
 * nothing that writes it may be dropped as dead.
 */
int cur_pinned_regs;

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

#define RODATA_REF_MAX 8192
rodata_ref_t rodata_refs[RODATA_REF_MAX];
int rodata_ref_count = 0;

/* Pending function-address relocations, for OP_address_of_func. A function's
 * address is elf_code_start + its entry block offset, which is only final once
 * every function has been emitted.
 */
typedef struct funcaddr_ref {
    int patch_location;
    basic_block_t *target_bb;
} funcaddr_ref_t;

#define FUNCADDR_REF_MAX 4096
funcaddr_ref_t funcaddr_refs[FUNCADDR_REF_MAX];

#define PLTCALL_REF_MAX 2048

/* A call whose target is a PLT entry. The PLT's address is only settled once
 * the final code size is known, so the displacement is filled in afterwards.
 */
typedef struct {
    int patch_location;
    int plt_offset;
} pltcall_ref_t;

pltcall_ref_t pltcall_refs[PLTCALL_REF_MAX];
int pltcall_ref_count = 0;

/* Record a PLT call site and reserve its displacement. */
void add_pltcall_ref(int plt_offset)
{
    if (pltcall_ref_count >= PLTCALL_REF_MAX)
        fatal("x64: too many PLT call sites");
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

/* Allow many forward references for large programs */
#define FORWARD_REF_MAX 49152
forward_ref_t forward_refs[FORWARD_REF_MAX];
int forward_ref_count = 0;

/* Add a forward reference to be patched later - call this right before
 * emit_dword Currently unused but will be needed for proper control flow
 */
void add_forward_ref(basic_block_t *target_bb)
{
    if (forward_ref_count >= FORWARD_REF_MAX)
        fatal("x64: too many forward branch relocations");

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
        if (only->op == OP_jump && !only->next) {
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
basic_block_t *instruction_to_bb[MAX_IR_INSTR];

/* Index of the instruction being emitted, for looking ahead within its block.
 */
int emit_ir_index;

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

/* Opcodes whose "dest" names a register they write. Only opcodes that certainly
 * do are listed, so a live register is never mistaken for dead.
 */
bool op_writes_dest(opcode_t op)
{
    switch (op) {
    case OP_cmov:
    case OP_load:
    case OP_load_constant:
    case OP_global_load:
    case OP_assign:
    case OP_add:
    case OP_sub:
    case OP_mul:
    case OP_div:
    case OP_mod:
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
    case OP_global_address_of:
    case OP_trunc:
    case OP_sign_ext:
        return true;
    default:
        return false;
    }
}

/* Opcodes whose src0 holds something other than a register number. Anything not
 * listed is assumed to read src0, which only costs a missed rewrite.
 */
bool op_src0_is_reg(opcode_t op)
{
    switch (op) {
    case OP_load:
    case OP_load_constant:
    case OP_global_load:
    case OP_address_of:
    case OP_global_address_of:
    case OP_load_data_address:
    case OP_load_rodata_address:
    case OP_define:
    case OP_label:
    case OP_jump:
        return false;
    default:
        return true;
    }
}

/* Opcodes whose src1 is a register rather than a width, slot or immediate. */
bool op_src1_is_reg(opcode_t op)
{
    switch (op) {
    case OP_add:
    case OP_sub:
    case OP_mul:
    case OP_div:
    case OP_mod:
    case OP_lshift:
    case OP_rshift:
    case OP_bit_and:
    case OP_bit_or:
    case OP_bit_xor:
    case OP_eq:
    case OP_neq:
    case OP_lt:
    case OP_leq:
    case OP_gt:
    case OP_geq:
    case OP_write:
    case OP_cmov:
        return true;
    default:
        return false;
    }
}

/* Whether src2 names a register. Only a select does: it is the value kept when
 * the condition does not hold, and a scan that missed it would take that value
 * for dead and drop whatever computed it.
 */
bool op_src2_is_reg(opcode_t op)
{
    return op == OP_cmov;
}

/* Whether @ir reads @reg as an operand. */
bool ir_reads_reg(ph2_ir_t *ir, int reg)
{
    if (op_src2_is_reg(ir->op) && ir->src2 == reg)
        return true;
    if (op_src0_is_reg(ir->op) && ir->src0 == reg)
        return true;
    return op_src1_is_reg(ir->op) && ir->src1 == reg;
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
    int top = X64_FIRST_CALLEE_SAVED - 1;

    /* A register pinned for the whole function must be preserved even if the
     * scan below were to miss it: its value has to survive the calls this
     * function makes.
     */
    for (int i = 0; i < REG_CNT; i++) {
        if (((func->pinned_regs >> i) & 1) && i > top)
            top = i;
    }
    for (basic_block_t *bb = func->bbs; bb; bb = bb->rpo_next) {
        for (ph2_ir_t *ir = bb->ph2_ir_list.head; ir; ir = ir->next) {
            if (op_writes_dest(ir->op) && ir->dest > top)
                top = ir->dest;
            if (op_src0_is_reg(ir->op) && ir->src0 > top)
                top = ir->src0;
            if (op_src1_is_reg(ir->op) && ir->src1 > top)
                top = ir->src1;

            /* A function whose only use of a preserved register is a select's
             * third operand still has to preserve it.
             */
            if (op_src2_is_reg(ir->op) && ir->src2 > top)
                top = ir->src2;
        }
    }
    if (top > REG_CNT - 1)
        top = REG_CNT - 1;
    return top - (X64_FIRST_CALLEE_SAVED - 1);
}

/* True when @reg still holds a live value past the end of @bb.
 *
 * reg_alloc() hands its register file to a successor that this block is the
 * only way into (bb_export_regs()), so a register can outlive the block that
 * filled it. Both scans below stop at the block boundary, and what they may
 * conclude there depends on this: with nothing carried, every register dies at
 * the end of a block and an unread value is dead.
 */
bool reg_live_out_of_bb(basic_block_t *bb, int reg)
{
    basic_block_t *succs[3];

    if (!bb || reg < 0 || reg >= REG_CNT)
        return false;

    /* A register the allocator pinned holds its variable on every path, so it
     * is live out of every block regardless of what the successors record.
     */
    if ((cur_pinned_regs >> reg) & 1)
        return true;

    succs[0] = bb->next;
    succs[1] = bb->then_;
    succs[2] = bb->else_;

    for (int i = 0; i < 3; i++) {
        if (succs[i] && succs[i]->entry_regs && succs[i]->entry_regs[reg])
            return true;
    }
    return false;
}

/* True when @reg is overwritten before any later read in its block, and is not
 * carried into a successor.
 */
bool reg_dead_after(int idx, int reg)
{
    if (folds_off || idx < 0)
        return false;
    basic_block_t *bb = idx < MAX_IR_INSTR ? instruction_to_bb[idx] : NULL;
    if (!bb)
        return false;
    int stop = idx + LIVENESS_SCAN_LIMIT;
    if (stop > ph2_ir_idx)
        stop = ph2_ir_idx;
    for (int j = idx; j < stop; j++) {
        if (j >= MAX_IR_INSTR || instruction_to_bb[j] != bb)
            return !reg_live_out_of_bb(bb, reg);
        ph2_ir_t *ir = PH2_IR_FLATTEN[j];
        if (ir_reads_reg(ir, reg))
            return false;
        if (op_writes_dest(ir->op) && ir->dest == reg)
            return true;
    }
    /* Ran out of scan budget: assume the register is still needed. */
    if (stop == ph2_ir_idx || instruction_to_bb[stop] != bb)
        return !reg_live_out_of_bb(bb, reg);
    return false;
}

/* The "op r/m, r" opcode matching an 0x81/0x83 group extension. */
int alu_mem_opcode(int ext)
{
    if (ext == 0)
        return 0x01; /* ADD */
    if (ext == 5)
        return 0x29; /* SUB */
    if (ext == 4)
        return 0x21; /* AND */
    if (ext == 1)
        return 0x09; /* OR */
    return 0x31;     /* XOR */
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
    if (idx < 0)
        return false;
    basic_block_t *bb = idx < MAX_IR_INSTR ? instruction_to_bb[idx] : NULL;
    if (!bb)
        return false;
    int stop = idx + LIVENESS_SCAN_LIMIT;
    if (stop > ph2_ir_idx)
        stop = ph2_ir_idx;
    for (int j = idx; j < stop; j++) {
        if (j >= MAX_IR_INSTR || instruction_to_bb[j] != bb)
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
    if (folds_off || idx < 0 || depth > 3)
        return false;
    basic_block_t *bb = idx < MAX_IR_INSTR ? instruction_to_bb[idx] : NULL;
    if (!bb)
        return false;
    int stop = idx + LIVENESS_SCAN_LIMIT;
    if (stop > ph2_ir_idx)
        stop = ph2_ir_idx;
    for (int j = idx; j < stop; j++) {
        if (j >= MAX_IR_INSTR || instruction_to_bb[j] != bb)
            return false; /* leaves the block: cannot tell */
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
                 * its full width, and then the bits going into it do matter.
                 */
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
    return false;
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
    if (stop > MAX_IR_INSTR)
        stop = MAX_IR_INSTR;
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
    if (before > MAX_IR_INSTR)
        before = MAX_IR_INSTR;

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

/* Put the sign-extended subscript in @rd and record the addressing mode the
 * access behind it will use, along with the instructions that no longer need
 * emitting: the addition at @sum_at, and the base it was given at @gaddr_at
 * when that folded into the displacement (-1 when it did not).
 *
 * MOVSXD is what makes @rd the value a load of the int would have produced,
 * since the subscript enters the addressing mode at full width.
 */
void sib_emit_index(int rd,
                    int rs1,
                    int base,
                    int scale,
                    int disp,
                    int sum_at,
                    int gaddr_at)
{
    emit_rex(1, rd, rs1); /* MOVSXD rd, rs1_32 */
    emit_byte(0x63);
    emit_byte(modrm(MOD_DIRECT, reg_low3(rd), reg_low3(rs1)));

    addr_sib = true;
    addr_sib_base = base;
    addr_sib_index = rd;
    addr_sib_scale = scale;
    addr_sib_disp = disp;
    skip_ir_add(sum_at);
    if (gaddr_at >= 0)
        skip_ir_add(gaddr_at);
}

/* Fold "D = I << k; A = base + D; read/write through A" into the addressing
 * mode of the access, when k scales by 1, 2, 4 or 8 and neither intermediate
 * outlives the access.
 *
 * The addition need not follow the shift immediately -- loading the array's
 * base commonly sits between them -- so the scan looks ahead past instructions
 * that only define a register of their own. The access itself must come
 * directly after the addition, since that is what carries the operand.
 *
 * The index is sign-extended before scaling rather than after, where the
 * separate instructions would scale first and narrow the product. The two
 * differ only when the scaled subscript overflows an int, which is already
 * undefined, and this is what gcc does as well.
 *
 * Returns true when the shift emitted the sign extension and recorded the
 * operand, leaving the addition to emit nothing.
 */
bool try_fold_sib(ph2_ir_t *shift, int rd, int rs1, int scale)
{
    if (folds_off || addr_fold || addr_sib || emit_ir_index < 0)
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
    if (limit > MAX_IR_INSTR)
        limit = MAX_IR_INSTR;
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
    int acc_at = sib_find_access(bb, sum_at + 1, sum->dest, -1, shift->dest);
    if (acc_at < 0)
        return false;

    /* The scaled index and the element address exist only for this access. The
     * addition commonly writes the same register it scaled, which ends the
     * scaled value there and needs no separate check.
     */
    if (sum->dest != shift->dest && !reg_dead_after(sum_at + 1, shift->dest))
        return false;
    if (!reg_dead_after(acc_at + 1, sum->dest))
        return false;

    /* A base that is just "the global area plus a constant" needs no register
     * of its own: R15 already holds that area, and the constant becomes the
     * addressing mode's displacement. Every global array is addressed this way,
     * and it also frees the fold from needing the base register to survive to
     * the access -- the allocator often reuses it in between.
     */
    int base = map_ir_reg(base_ir);
    int disp = 0;
    int gaddr_at = -1;

    /* The base is commonly materialised between the shift and the addition, so
     * look there first: that definition is the one the addition reads, and a
     * search that started before the shift walked straight past it to whatever
     * wrote the register last time round -- one global array's contents read at
     * another's address.
     */
    bool base_written = false;

    for (int j = emit_ir_index + 1; j < sum_at; j++) {
        ph2_ir_t *ir = PH2_IR_FLATTEN[j];

        if (op_writes_dest(ir->op) && ir->dest == base_ir) {
            /* Whatever wrote it, the definition reaching the addition is no
             * longer the one before the shift. Recording that separately from
             * whether the write was a global address matters: a later read
             * below clears gaddr_at, and without this the fallback would go
             * looking before the shift and find a stale definition.
             */
            base_written = true;
            if (ir->op == OP_global_address_of) {
                gaddr_at = j;
                disp = ir->src0;
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
        gaddr_def_ofs(bb, emit_ir_index, base_ir, &disp)) {
        base = 15; /* R15 */
        sib_emit_index(rd, rs1, base, scale, disp, sum_at, gaddr_at);
        return true;
    }
    if (gaddr_at < 0)
        disp = 0;
    if (gaddr_at >= 0 && reg_dead_after(sum_at + 1, base_ir)) {
        base = 15; /* R15 */
    } else {
        gaddr_at = -1;
        disp = 0;

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
    if (!sib_regs_ok(base, rd))
        return false;

    sib_emit_index(rd, rs1, base, scale, disp, sum_at, gaddr_at);
    return true;
}

/* The unscaled counterpart of try_fold_sib(): an addition whose only reader is
 * the access right behind it supplies the base and index of that access's
 * addressing mode, and so needs no instruction of its own. Reports whether it
 * folded.
 */
bool try_fold_sib_add(ph2_ir_t *ph2_ir, int rs1, int rs2)
{
    if (folds_off || addr_fold || addr_sib || emit_ir_index < 0 ||
        emit_ir_index >= MAX_IR_INSTR || rs1 == rs2)
        return false;
    if (const_reg_valid[ph2_ir->src0] || const_reg_valid[ph2_ir->src1])
        return false;

    basic_block_t *bb = instruction_to_bb[emit_ir_index];
    int acc_at = bb ? sib_find_access(bb, emit_ir_index + 1, ph2_ir->dest,
                                      ph2_ir->src0, ph2_ir->src1)
                    : -1;
    if (acc_at < 0 || !reg_dead_after(acc_at + 1, ph2_ir->dest))
        return false;

    /* A base that the instruction before computed as "the global area plus a
     * constant" becomes a displacement, the same way try_fold_sib() handles a
     * scaled subscript.
     */
    int base = rs1, index = rs2, disp = 0;
    bool folded_base = false;

    if (gaddr_def_ofs(bb, emit_ir_index, ph2_ir->src0, &disp)) {
        base = 15; /* R15 */
        index = rs2;
        folded_base = true;
    } else if (gaddr_def_ofs(bb, emit_ir_index, ph2_ir->src1, &disp)) {
        base = 15; /* R15 */
        index = rs1;
        folded_base = true;
    }

    /* RSP and R12 share the encoding that means "no index", so whichever
     * operand is one of those has to be the base.
     */
    if (!folded_base && reg_low3(index) == 4) {
        base = rs2;
        index = rs1;
    }
    if (reg_low3(index) == 4)
        return false;

    addr_sib = true;
    addr_sib_base = base;
    addr_sib_index = index;
    addr_sib_scale = 0;
    addr_sib_disp = disp;
    return true;
}

/* True when the only remaining reads of @reg in this block are shift counts
 * that will be emitted as immediates, so materialising the constant is
 * pointless. spill_live_out() empties every register at the end of a block, so
 * reaching the end unread settles it.
 */
bool const_load_dead(int idx, int reg, int val)
{
    if (folds_off || idx < 0)
        return false;
    basic_block_t *bb = idx < MAX_IR_INSTR ? instruction_to_bb[idx] : NULL;
    if (!bb)
        return false;
    int stop = idx + LIVENESS_SCAN_LIMIT;
    if (stop > ph2_ir_idx)
        stop = ph2_ir_idx;
    bool folds_ok = true;
    for (int j = idx; j < stop; j++) {
        if (j >= MAX_IR_INSTR || instruction_to_bb[j] != bb)
            return !reg_live_out_of_bb(bb, reg);
        ph2_ir_t *ir = PH2_IR_FLATTEN[j];
        /* Uses that become immediates do not read the register at all. */
        bool folded_count = false;
        if (ir->src1 == reg) {
            if ((ir->op == OP_lshift || ir->op == OP_rshift) && val >= 0 &&
                val < 32)
                folded_count = true;
            if (ir->op == OP_add || ir->op == OP_sub || ir->op == OP_bit_and ||
                ir->op == OP_bit_or || ir->op == OP_bit_xor)
                folded_count = true;
            /* The three-operand IMUL carries its multiplier as an immediate. */
            if (ir->op == OP_mul)
                folded_count = true;
            if (branch_cc_for(ir->op,
                              ir->src0_is_unsigned || ir->src1_is_unsigned))
                folded_count = true;
        }

        /* A select's third operand is a plain register read that never turns
         * into an immediate, and it is checked before the write below because
         * the same instruction reads it and writes the destination.
         */
        if (op_src2_is_reg(ir->op) && ir->src2 == reg)
            return false;
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
            if (ir->op == OP_branch || ir->op == OP_jump || ir->op == OP_return)
                return !reg_live_out_of_bb(bb, reg);
            if (ir->op != OP_write)
                return false;
            folds_ok = false;
        }
    }
    /* Ran out of scan budget: assume the register is still needed. */
    if (stop == ph2_ir_idx || instruction_to_bb[stop] != bb)
        return !reg_live_out_of_bb(bb, reg);
    return false;
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

/* The single predecessor of @bb, or NULL when it has none or more than one. */
basic_block_t *bb_sole_pred(basic_block_t *bb)
{
    basic_block_t *only = NULL;
    for (int i = 0; i < bb->prev_idx; i++) {
        if (!bb->prev[i].bb)
            continue;
        if (only)
            return NULL;
        only = bb->prev[i].bb;
    }
    return only;
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
        emit_rex(size_bytes > 4, -1, rs1);
        if (cmp_imm_val >= -128 && cmp_imm_val <= 127) {
            emit_byte(0x83); /* CMP r32/r64, imm8 */
            emit_byte(modrm(MOD_DIRECT, 7, reg_low3(rs1)));
            emit_byte(cmp_imm_val);
        } else {
            emit_byte(0x81); /* CMP r32/r64, imm32 */
            emit_byte(modrm(MOD_DIRECT, 7, reg_low3(rs1)));
            emit_dword(cmp_imm_val);
        }
        return;
    }
    if (cmp_mem_slot >= 0) {
        emit_rex(size_bytes > 4, rs1, -1);
        emit_byte(0x3B); /* CMP rs1, [rsp + slot] */
        emit_rsp_mem(rs1, cmp_mem_slot);
        cmp_mem_slot = -1;
        return;
    }
    emit_rex(size_bytes > 4, rs2, rs1);
    emit_byte(0x39); /* CMP rs1, rs2 */
    emit_byte(modrm(MOD_DIRECT, reg_low3(rs2), reg_low3(rs1)));
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
    if (emit_ir_index + 2 >= MAX_IR_INSTR ||
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
        if (width == 8)
            emit_rex(1, -1, -1);
        if (val >= -128 && val <= 127) {
            emit_byte(0x83);
            emit_rsp_mem(ext, ld->src0);
            emit_byte(val);
        } else {
            emit_byte(0x81);
            emit_rsp_mem(ext, ld->src0);
            emit_dword(val);
        }
    } else {
        if (width == 8)
            emit_rex(1, other, -1);
        else if (other >= 8)
            emit_byte(REX_R);
        /* The register-source group with a memory destination. */
        emit_byte(alu_mem_opcode(ext));
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
    if (emit_ir_index + 1 >= MAX_IR_INSTR ||
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
    if (width == 8)
        emit_rex(1, -1, -1);
    if (val >= -128 && val <= 127) {
        emit_byte(0x83);
        emit_rsp_mem(ext, st->src1);
        emit_byte(val);
    } else {
        emit_byte(0x81);
        emit_rsp_mem(ext, st->src1);
        emit_dword(val);
    }
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
bool emit_mul_by_const(int rd, int rs1, int c)
{
    int k = exact_log2(c);

    if (c == 0) {
        emit_rex(1, rd, rd); /* XOR rd, rd */
        emit_byte(0x31);
        emit_byte(modrm(MOD_DIRECT, reg_low3(rd), reg_low3(rd)));
        return true;
    }

    /* LEA computes rs1 + rs1 * s for s of 1, 2, 4 or 8 in one instruction,
     * covering multipliers of 2, 3, 5 and 9 without touching rs1.
     */
    if ((c == 2 || c == 3 || c == 5 || c == 9) && reg_low3(rs1) != 4) {
        emit_rex_sib(1, rd, rs1, rs1);
        emit_byte(0x8D);
        emit_mem_sib(rd, rs1, rs1, exact_log2(c - 1), 0);
        return true;
    }
    /* A power of two is a shift, and one is that shift by nothing. */
    if (k >= 0) {
        emit_mov_reg(rd, rs1);
        if (k) {
            emit_shift_imm(rd, SHIFT_EXT_SHL, k);
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
            emit_rex(1, rs1, 10);
            emit_byte(0x89); /* MOV r10, rs1 */
            emit_byte(modrm(MOD_DIRECT, reg_low3(rs1), 2));
            emit_shift_imm(rd, SHIFT_EXT_SHL, up > 1 ? up : down);
            emit_rex(1, 10, rd);
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

/* Integer arithmetic. */
void emit_arith(ph2_ir_t *ph2_ir,
                int rd,
                int rs1,
                int rs2,
                bool src1_const_known,
                int src1_const)
{
    switch (ph2_ir->op) {
    case OP_add: {
        /* A tracked literal becomes an immediate, which also makes the
         * instruction that materialised it dead.
         */
        if (src1_const_known) {
            /* If the only consumer is the load right after, that load can carry
             * the displacement and this address never needs a register. The
             * memory operand picks a byte or a doubleword displacement for
             * itself, so the constant does not have to be a small one.
             */
            if (!addr_fold && emit_next_ir &&
                (emit_next_ir->op == OP_read || emit_next_ir->op == OP_write) &&
                emit_next_ir->src0 == ph2_ir->dest &&
                emit_next_ir->src1 != ph2_ir->dest && reg_low3(rs1) != 4 &&
                reg_dead_after(emit_ir_index + 2, ph2_ir->dest)) {
                addr_fold = true;
                addr_fold_base = rs1;
                addr_fold_disp = src1_const;
                return;
            }
            if (rd != rs1 && emit_lea_disp(rd, rs1, src1_const))
                return;
            emit_mov_reg(rd, rs1);
            emit_alu_imm(rd, 0, src1_const); /* ADD rd, imm */
            return;
        }
        if (try_fold_sib_add(ph2_ir, rs1, rs2))
            return;

        /* x64 only has 2-operand ADD, so for rd = rs1 + rs2: MOV rd, rs1 ADD
         * rd, rs2
         *
         * Special case: if rd == rs2, then MOV rd, rs1 would overwrite rs2 In
         * this case, use: ADD rs2, rs1 (which is commutative for addition)
         */
        if (rd == rs2 && rd != rs1) {
            /* ADD rd, rs1 (since addition is commutative) */
            emit_rex(1, rs1, rd);
            emit_byte(0x01);
            int reg_rs1 = reg_low3(rs1);
            int reg_rd = reg_low3(rd);
            emit_byte(modrm(MOD_DIRECT, reg_rs1, reg_rd));
        } else if (rd != rs1 && rd != rs2 && emit_lea_sum(rd, rs1, rs2)) {
            /* Three distinct registers: one LEA replaces MOV plus ADD. */
        } else {
            /* Normal case: MOV rd, rs1; ADD rd, rs2 */
            emit_mov_reg(rd, rs1);
            /* ADD rd, rs2 */
            emit_rex(1, rs2, rd);
            emit_byte(0x01);
            int reg_rs2 = reg_low3(rs2);
            int reg_rd2 = reg_low3(rd);
            emit_byte(modrm(MOD_DIRECT, reg_rs2, reg_rd2));
        }
        return;
    }

    case OP_sub: {
        /* A tracked literal becomes an immediate, which also makes the
         * instruction that materialised it dead.
         */
        if (src1_const_known) {
            if (rd != rs1 && emit_lea_disp(rd, rs1, -src1_const))
                return;
            emit_mov_reg(rd, rs1);
            emit_alu_imm(rd, 5, src1_const); /* SUB rd, imm */
            return;
        }

        /* rd = rs1 - rs2.
         *
         * The staging below is only needed when rd and rs2 are the same
         * register, where "MOV rd, rs1" would destroy the subtrahend before it
         * is read. Every other case subtracts in place.
         */
        if (rd != rs2) {
            emit_mov_reg(rd, rs1);
            emit_rex(1, rs2, rd); /* SUB rd, rs2 */
            emit_byte(0x29);
            emit_byte(modrm(MOD_DIRECT, reg_low3(rs2), reg_low3(rd)));
            return;
        }

        /* rd == rs2: stage through R11, which reg_map never hands out, so any
         * aliasing is harmless. MOV r11, rs1
         */
        emit_rex(1, rs1, 11);
        emit_byte(0x89);
        emit_byte(modrm(MOD_DIRECT, reg_low3(rs1), 3));
        /* SUB r11, rs2 */
        emit_rex(1, rs2, 11);
        emit_byte(0x29);
        emit_byte(modrm(MOD_DIRECT, reg_low3(rs2), 3));
        /* MOV rd, r11 */
        emit_rex(1, 11, rd);
        emit_byte(0x89);
        emit_byte(modrm(MOD_DIRECT, 3, reg_low3(rd)));
        return;
    }
    case OP_mul: {
        /* rd = rs1 * rs2, then truncated to int width.
         *
         * shecc's int is 32 bits, and code such as the FNV-1a hash in
         * hashmap_hash_index() depends on multiplication wrapping at 32 bits.
         * This backend keeps values in 64-bit registers, so the product must be
         * narrowed explicitly with MOVSXD; without it the value keeps growing
         * and later comparisons against INT_MAX take the wrong branch. Pointers
         * are never multiplied, and pointer scaling (index * element size) is
         * far inside 32 bits, so narrowing here is safe.
         */
        if (src1_const_known && emit_mul_by_const(rd, rs1, src1_const)) {
            /* nothing further: the product is already in rd */
        } else if (src1_const_known) {
            /* The three-operand IMUL takes the multiplier as an immediate and
             * writes a different register, so neither the literal nor the MOV
             * that would stage it needs an instruction.
             */
            emit_rex(1, rd, rs1);
            if (src1_const >= -128 && src1_const <= 127) {
                emit_byte(0x6B); /* IMUL rd, rs1, imm8 */
                emit_byte(modrm(MOD_DIRECT, reg_low3(rd), reg_low3(rs1)));
                emit_byte(src1_const);
            } else {
                emit_byte(0x69); /* IMUL rd, rs1, imm32 */
                emit_byte(modrm(MOD_DIRECT, reg_low3(rd), reg_low3(rs1)));
                emit_dword(src1_const);
            }
        } else if (rd == rs2 && rd != rs1) {
            /* IMUL is commutative, so multiply by rs1 in place. */
            emit_rex(1, rd, rs1);
            emit_byte(0x0F);
            emit_byte(0xAF);
            emit_byte(modrm(MOD_DIRECT, reg_low3(rd), reg_low3(rs1)));
        } else {
            emit_mov_reg(rd, rs1);
            emit_rex(1, rd, rs2);
            emit_byte(0x0F);
            emit_byte(0xAF);
            emit_byte(modrm(MOD_DIRECT, reg_low3(rd), reg_low3(rs2)));
        }
        if (ph2_ir->size_bytes <= 4 &&
            !reg_low32_sufficient(emit_ir_index + 1, ph2_ir->dest, 0))
            wrap_to_int(rd, ph2_ir->is_pointer);
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
         * whereas a 64-bit DIV sees 2^64 - 1.
         */
        emit_rex(wide, 0, 10); /* MOV r10{d}, rax{d}  (save) */
        emit_byte(0x89);
        emit_byte(modrm(MOD_DIRECT, 0, 2));
        emit_push_reg(2);        /* PUSH rdx  (save) */
        emit_rex(wide, rs2, 11); /* MOV r11{d}, rs2{d} */
        emit_byte(0x89);
        emit_byte(modrm(MOD_DIRECT, reg_low3(rs2), 3));
        emit_rex(wide, rs1, -1); /* MOV rax{d}, rs1{d} */
        emit_byte(0x89);
        emit_byte(modrm(MOD_DIRECT, reg_low3(rs1), 0));
        if (is_unsigned) {
            /* Unsigned DIV consumes a zero-extended RDX:RAX dividend. */
            emit_byte(0x31); /* XOR edx, edx */
            emit_byte(modrm(MOD_DIRECT, 2, 2));
        } else {
            if (wide)
                emit_byte(REX_W); /* CQO */
            emit_byte(0x99);
        }
        emit_rex(wide, -1, 11); /* DIV/IDIV r11{d} */
        emit_byte(0xF7);
        emit_byte(modrm(MOD_DIRECT, is_unsigned ? 6 : 7, 3));

        /* Capture the result into R11 before restoring RAX/RDX, so rd may
         * itself be RAX or RDX.
         */
        emit_rex(wide, ph2_ir->op == OP_div ? 0 : 2, 11);
        /* MOV r11{d}, rax{d} | rdx{d} */
        emit_byte(0x89);
        emit_byte(modrm(MOD_DIRECT, ph2_ir->op == OP_div ? 0 : 2, 3));
        emit_rex(wide, 10, 0); /* MOV rax{d}, r10{d}  (restore) */
        emit_byte(0x89);
        emit_byte(modrm(MOD_DIRECT, 2, 0));
        emit_pop_reg(2);        /* POP rdx  (restore) */
        emit_rex(wide, 11, rd); /* MOV rd{d}, r11{d} */
        emit_byte(0x89);
        emit_byte(modrm(MOD_DIRECT, 3, reg_low3(rd)));
        return;
    }
    default:
        break;
    }
}

/* Bitwise operations and shifts. */
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
        /* The three bitwise operations differ only in the immediate group's
         * opcode extension and the r/m opcode byte, which alu_ext_for() and
         * alu_mem_opcode() already supply; all three are commutative.
         */
        int ext = alu_ext_for(ph2_ir->op);

        /* A tracked literal becomes an immediate, which also makes the
         * instruction that materialised it dead.
         */
        if (src1_const_known) {
            emit_mov_reg(rd, rs1);
            emit_alu_imm(rd, ext, src1_const);
            return;
        }

        /* Two-operand form: "rd = rs1 OP rs2" is MOV rd, rs1 followed by OP rd,
         * rs2. When rd already names rs2, commutativity lets the MOV go and rs1
         * becomes the source instead.
         */
        int src = rs2;
        if (rd == rs2 && rd != rs1)
            src = rs1;
        else
            emit_mov_reg(rd, rs1);
        emit_rex(1, src, rd);
        emit_byte(alu_mem_opcode(ext));
        emit_byte(modrm(MOD_DIRECT, reg_low3(src), reg_low3(rd)));
        return;
    }

    case OP_bit_not:
        /* rd = NOT rs1. Copy first: rd and rs1 differ whenever the operand is
         * not already in the destination register.
         */
        emit_mov_reg(rd, rs1);
        emit_rex(1, -1, rd);
        emit_byte(0xF7);
        emit_byte(modrm(MOD_DIRECT, 2, reg_low3(rd)));
        return;

    case OP_negate:
        /* rd = NEG rs1. Copy first: rd and rs1 differ whenever the operand is
         * not already in the destination register.
         */
        emit_mov_reg(rd, rs1);
        emit_rex(1, -1, rd);
        emit_byte(0xF7);
        emit_byte(modrm(MOD_DIRECT, 3, reg_low3(rd)));
        return;

    case OP_lshift: {
        /* A count the block already loaded as a literal needs neither CL nor
         * the save/restore around it: SHL r64, imm8 is one instruction.
         */
        if (src1_const_known && src1_const >= 0 && src1_const < 64) {
            int want_src = ph2_ir->src0;
            int dest_ir = ph2_ir->dest;

            /* The same shift of the same register may already sit in another
             * register; copying it is one instruction instead of three.
             */
            for (int i = 0; i < REG_CNT; i++) {
                if (!shift_valid[i] || shift_src[i] != want_src ||
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

            /* Narrowing back to int matters when the result is a value, but a
             * scaled index feeding pointer arithmetic is only ever added to an
             * address. Overflowing it is already undefined, and gcc likewise
             * scales the sign-extended index in 64 bits without re-wrapping.
             */
            bool addr_only =
                reg_feeds_address_only(emit_ir_index + 1, ph2_ir->dest);
            emit_mov_reg(rd, rs1);
            emit_shift_imm(rd, SHIFT_EXT_SHL, src1_const);
            if (ph2_ir->size_bytes <= 4 && !addr_only &&
                !reg_low32_sufficient(emit_ir_index + 1, ph2_ir->dest, 0))
                wrap_to_int(rd, ph2_ir->is_pointer);

            /* Recording needs the shift source to still be intact: if the
             * result landed in it, the pairing no longer describes anything.
             */
            if (dest_ir >= 0 && dest_ir < REG_CNT && dest_ir != want_src &&
                want_src >= 0 && want_src < REG_CNT) {
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
        emit_byte(REX_W | REX_B); /* MOV r10, rcx */
        emit_byte(0x89);
        emit_byte(modrm(MOD_DIRECT, 1, 2));
        emit_rex(1, rs1, 11); /* MOV r11, rs1 */
        emit_byte(0x89);
        emit_byte(modrm(MOD_DIRECT, reg_low3(rs1), 3));
        emit_rex(1, rs2, -1); /* MOV rcx, rs2 */
        emit_byte(0x89);
        emit_byte(modrm(MOD_DIRECT, reg_low3(rs2), 1));
        emit_byte(REX_W | REX_B); /* SHL r11, cl */
        emit_byte(0xD3);
        emit_byte(modrm(MOD_DIRECT, 4, 3));
        emit_byte(REX_W | REX_R); /* MOV rcx, r10 */
        emit_byte(0x89);
        emit_byte(modrm(MOD_DIRECT, 2, 1));
        emit_rex(1, 11, rd); /* MOV rd, r11 */
        emit_byte(0x89);
        emit_byte(modrm(MOD_DIRECT, 3, reg_low3(rd)));

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
        /* A count the block already loaded as a literal needs neither CL nor
         * the save/restore around it: SAR r64, imm8 is one instruction.
         */
        if (src1_const_known && src1_const >= 0 && src1_const < 64) {
            if (ph2_ir->is_unsigned)
                emit_zero_extend(rd, rs1, ph2_ir->size_bytes);
            else
                emit_mov_reg(rd, rs1);
            emit_shift_imm(rd,
                           ph2_ir->is_unsigned ? SHIFT_EXT_SHR : SHIFT_EXT_SAR,
                           src1_const);
            if (ph2_ir->size_bytes <= 4 &&
                !reg_low32_sufficient(emit_ir_index + 1, ph2_ir->dest, 0))
                wrap_to_int(rd, ph2_ir->is_pointer);
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
        emit_byte(REX_W | REX_B); /* MOV r10, rcx */
        emit_byte(0x89);
        emit_byte(modrm(MOD_DIRECT, 1, 2));
        emit_rex(1, rs1, 11); /* MOV r11, rs1 */
        emit_byte(0x89);
        emit_byte(modrm(MOD_DIRECT, reg_low3(rs1), 3));
        emit_rex(1, rs2, -1); /* MOV rcx, rs2 */
        emit_byte(0x89);
        emit_byte(modrm(MOD_DIRECT, reg_low3(rs2), 1));
        emit_byte(REX_W | REX_B); /* SAR/SHR r11, cl */
        emit_byte(0xD3);
        emit_byte(modrm(MOD_DIRECT, ph2_ir->is_unsigned ? 5 : 7, 3));
        emit_byte(REX_W | REX_R); /* MOV rcx, r10 */
        emit_byte(0x89);
        emit_byte(modrm(MOD_DIRECT, 2, 1));
        emit_rex(1, 11, rd); /* MOV rd, r11 */
        emit_byte(0x89);
        emit_byte(modrm(MOD_DIRECT, 3, reg_low3(rd)));
        return;
    }
    default:
        break;
    }
}

/* Comparisons, and the jumps and branches they feed. */
void emit_compare_jump(ph2_ir_t *ph2_ir, int rd, int rs1, int rs2)
{
    switch (ph2_ir->op) {
    case OP_eq:
    case OP_neq:
    case OP_lt:
    case OP_leq:
    case OP_gt:
    case OP_geq:
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
            ph2_ir_t *tail = NULL;
            int cnt = 0;
            for (ph2_ir_t *t = ph2_ir->next_bb->ph2_ir_list.head; t;
                 t = t->next) {
                tail = t;
                cnt++;
            }
            if (tail && tail->op == OP_branch && cnt <= 6) {
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
                    emit_next_ir = t->next;
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

        /* A jump to the instruction that follows it is already the way control
         * flows. reg_alloc() appends the jump from the CFG alone, and the
         * flattened order often puts the target next anyway.
         */
        if (bb_falls_through(ph2_ir->next_bb))
            return;

        /* JMP rel32, through the same target resolution the conditional edges
         * use, so a jump landing on nothing but another jump goes straight to
         * where that one ends up.
         */
        emit_byte(0xE9);
        emit_bb_rel32(ph2_ir->next_bb);
        return;
    }

    case OP_branch: {
        if (fused_cc_pending) {
            fused_cc_pending = false;
            emit_byte(0x0F);
            emit_byte(fused_cc); /* Jcc rel32 -> then_bb */
            emit_bb_rel32(ph2_ir->then_bb);

            if (!bb_falls_through(ph2_ir->else_bb)) {
                emit_byte(0xE9); /* JMP rel32 -> else_bb */
                emit_bb_rel32(ph2_ir->else_bb);
                emit_fell_through = false;
            }
            return;
        }

        /* TEST rs1, rs1 */
        emit_rex(1, rs1, rs1);
        emit_byte(0x85);
        emit_byte(modrm(MOD_DIRECT, reg_low3(rs1), reg_low3(rs1)));

        /* Emit both edges explicitly: JNZ then_bb, then JMP else_bb.
         *
         * Falling through to else_bb would be shorter, but it is only valid
         * when else_bb is literally the next block emitted. This backend
         * interleaves NOP sleds and re-emits blocks that the linear walk
         * missed, so that property does not hold reliably -- and when it
         * silently fails, a false condition runs the true branch. Short circuit
         * operators are where this shows up first: in "a && b" the false edge
         * would fall into b's evaluation and adopt its result.
         */
        emit_byte(0x0F);
        emit_byte(0x85); /* JNZ rel32 -> then_bb */
        emit_bb_rel32(ph2_ir->then_bb);

        if (!bb_falls_through(ph2_ir->else_bb)) {
            emit_byte(0xE9); /* JMP rel32 -> else_bb */
            emit_bb_rel32(ph2_ir->else_bb);
            emit_fell_through = false;
        }
        return;
    }
    default:
        break;
    }
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
                       ph2_ir->func_name);
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

            /* System V returns integers in RAX, but the shared IR expects a
             * call's result in register 0 (reg_map[0] == RDI): reg-alloc turns
             * OP_func_ret into OP_assign reading register 0. Bridge the two
             * conventions here, once, for every call.
             *
             * MOV RDI, RAX
             */
            emit_byte(REX_W);
            emit_byte(0x89);
            emit_byte(modrm(MOD_DIRECT, 0, 7)); /* reg=RAX -> rm=RDI */
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
            if (ret_reg != 0) { /* 0 is RAX, no move needed */
                /* MOV RAX, ret_reg.
                 *
                 * 0x89 is MOV r/m64, r64: the ModRM reg field holds the source
                 * (ret_reg) and rm holds the destination (RAX). The reg field
                 * is extended by REX.R, not REX.B -- setting REX.B here instead
                 * extended RAX, so a return value the allocator left in r8/r9
                 * emitted "MOV r8, rax", the exact opposite of what is
                 * intended.
                 */
                emit_rex(1, ret_reg, -1); /* REX.R for source reg >= 8 */
                emit_byte(0x89);
                int ret_reg_low = reg_low3(ret_reg);
                int rax_reg3 = 0;
                emit_byte(modrm(MOD_DIRECT, ret_reg_low, rax_reg3));
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
            {
                int aligned_ret =
                    x64_frame_bytes(stack_size_ret, cur_saved_regs);
                emit_byte(REX_W);
                if (aligned_ret <= 127) {
                    emit_byte(0x83); /* ADD r/m64, imm8 */
                    int rsp_reg = 4;
                    emit_byte(modrm(MOD_DIRECT, 0, rsp_reg));
                    emit_byte(aligned_ret);
                } else {
                    emit_byte(0x81); /* ADD r/m64, imm32 */
                    int rsp_reg = 4;
                    emit_byte(modrm(MOD_DIRECT, 0, rsp_reg));
                    emit_dword(aligned_ret);
                }
            }
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
        emit_byte(REX_W);
        if (alloc_bytes >= -128 && alloc_bytes <= 127) {
            emit_byte(0x83);
            int rsp_reg3 = 4;
            emit_byte(modrm(MOD_DIRECT, 5, rsp_reg3));
            emit_byte(alloc_bytes);
        } else {
            emit_byte(0x81);
            int rsp_reg3 = 4;
            emit_byte(modrm(MOD_DIRECT, 5, rsp_reg3));
            emit_dword(alloc_bytes);
        }
        return;
    }

    case OP_address_of: {
        /* LEA rd, [rsp + src0] */
        emit_rex(1, rd, -1);
        emit_byte(0x8D);
        emit_rsp_mem(rd, ph2_ir->src0);
        return;
    }

    case OP_cmov: {
        /* The value for the failing arm goes to the destination, and the other
         * moves over it only when the condition holds. No branch means nothing
         * to mispredict, which is the point: the arms were flattened precisely
         * because the test was unpredictable.
         */
        int cond = rs1, taken = map_ir_reg(ph2_ir->src1);
        int other = map_ir_reg(ph2_ir->src2);
        int move_in = taken, cc = 0x45; /* CMOVNE */

        /* The test comes first because the move below may land in the
         * condition's own register: the condition dies at this instruction, so
         * the allocator is free to hand its register to the destination.
         * Reading the flags before overwriting it costs nothing -- MOV leaves
         * them alone -- and testing after would have tested the wrong value.
         */
        emit_rex(1, cond, cond); /* TEST cond, cond */
        emit_byte(0x85);
        emit_byte(modrm(MOD_DIRECT, reg_low3(cond), reg_low3(cond)));

        /* When the destination already holds the value the test selects,
         * copying the other over it would destroy it; keep what is there and
         * bring the other in when the test fails instead.
         */
        if (rd == taken) {
            move_in = other;
            cc = 0x44; /* CMOVE */
        } else {
            emit_mov_reg(rd, other); /* nothing to do when they coincide */
        }

        emit_rex(1, rd, move_in); /* CMOVcc rd, move_in */
        emit_byte(0x0F);
        emit_byte(cc);
        emit_byte(modrm(MOD_DIRECT, reg_low3(rd), reg_low3(move_in)));
        return;
    }

    case OP_load:
        /* Load a local from its frame slot at the width of its type.
         *
         * A slot is pointer-sized, but an int occupies only its low 4 bytes.
         * Reading all 8 picks up whatever preceded it whenever the variable was
         * last written through a pointer -- a callee doing "val[0] = x" stores
         * 4 bytes -- which makes a zero read back as non-zero. That is how "#if
         * defined(__arm__)" ended up true when self-hosting.
         */
        {
            int stack_offset = ph2_ir->src0; /* frame offset from RSP */
            int eff_size = ph2_ir->size_bytes;
            if (eff_size == 1) {
                /* MOVSX rd, BYTE PTR [rsp+ofs]. char is signed here, and a
                 * _Bool only ever holds 0 or 1, which sign-extends to itself.
                 */
                emit_rex(1, rd, -1);
                emit_byte(0x0F);
                emit_byte(ph2_ir->is_unsigned ? 0xB6 : 0xBE);
            } else if (eff_size == 2) {
                emit_rex(1, rd, -1);
                emit_byte(0x0F);
                emit_byte(ph2_ir->is_unsigned ? 0xB7 : 0xBF);
            } else if (eff_size == 4) {
                /* A 32-bit MOV zero extends; signed int uses MOVSXD. */
                emit_rex(ph2_ir->is_unsigned ? 0 : 1, rd, -1);
                emit_byte(ph2_ir->is_unsigned ? 0x8B : 0x63);
            } else {
                emit_rex(1, rd, -1);
                emit_byte(0x8B);
            }
            emit_rsp_mem(rd, stack_offset);
        }
        return;

    case OP_store:
        /* MOV [rbp-offset], src_reg - Use type info for proper sizing */
        {
            int src_reg = rs1;
            int stack_offset = ph2_ir->src1; /* frame offset from RSP */

            /* Use type information to determine store size For x64, pointers
             * MUST be stored as 64-bit to preserve address
             */
            int eff_size = ph2_ir->size_bytes;
            if (ph2_ir->is_pointer && eff_size != PTR_SIZE)
                eff_size = PTR_SIZE;

            if (eff_size <= 4) {
                /* 32-bit store. An int occupies only the low four bytes of its
                 * slot and OP_load reads it back at that width, so writing
                 * eight would put bits there that nothing ever reads -- and it
                 * is what stopped the slot from being usable as a memory
                 * operand.
                 */
                if (src_reg >= 8)
                    emit_byte(REX_R);
            } else {
                /* Pointers and anything slot-width keep the 64-bit form. */
                emit_rex(1, src_reg, -1);
            }
            emit_byte(0x89);
            emit_rsp_mem(src_reg, stack_offset);
        }
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
            int mem_disp = 0;
            bool have_disp = false;
            int sib_base, sib_index, sib_scale, sib_disp;
            bool sib = sib_take(&sib_base, &sib_index, &sib_scale, &sib_disp);
            if (addr_fold) {
                rs1 = addr_fold_base;
                mem_disp = addr_fold_disp;
                have_disp = true;
                addr_fold = false;
            }
            bool unsigned_word = ph2_ir->is_unsigned && rsize == 4;
            if (sib)
                emit_rex_sib(unsigned_word ? 0 : 1, rd, sib_base, sib_index);
            else
                emit_rex(unsigned_word ? 0 : 1, rd, rs1);
            if (rsize == 1) {
                /* MOVSX r64, BYTE PTR [rs1]: char is signed here, as it is for
                 * every other narrow type, and RISC-V already uses lb rather
                 * than lbu. Zero-extending made a negative char read back
                 * through a pointer as a large positive value.
                 */
                emit_byte(0x0F);
                emit_byte(ph2_ir->is_unsigned ? 0xB6 : 0xBE);
            } else if (rsize == 2) {
                /* MOVSX r64, WORD PTR [rs1]: short is signed, so the upper bits
                 * must be a sign extension. ARM uses LDRSH and RISC-V uses lh
                 * here; zero-extending made a negative short read back as a
                 * large positive value.
                 */
                emit_byte(0x0F);
                emit_byte(ph2_ir->is_unsigned ? 0xB7 : 0xBF);
            } else if (rsize == 4) {
                /* MOVSXD r64, DWORD PTR [rs1]: int is signed, so the upper half
                 * must be a sign extension, not zero.
                 */
                emit_byte(ph2_ir->is_unsigned ? 0x8B : 0x63);
            } else {
                /* MOV r64, [rs1] */
                emit_byte(0x8B);
            }
            if (sib)
                emit_mem_sib(rd, sib_base, sib_index, sib_scale, sib_disp);
            else
                emit_mem_base(rd, rs1, mem_disp, have_disp);
        }
        return;

    case OP_write:
        /* Store value (src1) into [address (src0)] - IR uses src0 as address */
        {
            int addr_reg = rs1;
            int val_reg = rs2;
            int wr_disp = 0;
            bool wr_have_disp = false;
            int wsib_base, wsib_index, wsib_scale, wsib_disp;
            bool wsib =
                sib_take(&wsib_base, &wsib_index, &wsib_scale, &wsib_disp);
            if (addr_fold) {
                addr_reg = addr_fold_base;
                wr_disp = addr_fold_disp;
                wr_have_disp = true;
                addr_fold = false;
            }
            /* Debug: trace write width mismatches for pointers */
            if (ph2_ir->is_pointer && ph2_ir->size_bytes != PTR_SIZE) {
                if (x64_debug)
                    fprintf(
                        stderr,
                        "[EMIT] OP_write ptr width mismatch: size=%d ptr=%d "
                        "addr=%d val=%d\n",
                        ph2_ir->size_bytes, ph2_ir->is_pointer, ph2_ir->src0,
                        ph2_ir->src1);
            }

            /* The store width the IR asks for lives in dest (1, 2 or 4);
             * pointer-typed writes must store a full pointer. As with OP_read,
             * the IR width already accounts for pointers.
             */
            int wsize = ph2_ir->dest;
            if (wsize == 1) {
                /* MOV r/m8, r8. A REX prefix is mandatory once the source is
                 * RSP/RBP/RSI/RDI, otherwise the encoding names AH/CH/DH/BH.
                 */
                if (wsib)
                    emit_rex_sib(0, val_reg, wsib_base, wsib_index);
                else if (val_reg >= 4 || addr_reg >= 8)
                    emit_rex(0, val_reg, addr_reg);
                emit_byte(0x88);
            } else if (wsize == 2) {
                emit_byte(0x66); /* operand-size override */
                if (wsib)
                    emit_rex_sib(0, val_reg, wsib_base, wsib_index);
                else if (val_reg >= 8 || addr_reg >= 8)
                    emit_rex(0, val_reg, addr_reg);
                emit_byte(0x89);
            } else if (wsize == 4) {
                if (wsib) {
                    if (val_reg >= 8 || wsib_base >= 8 || wsib_index >= 8)
                        emit_rex_sib(0, val_reg, wsib_base, wsib_index);
                } else if (val_reg >= 8 || addr_reg >= 8)
                    emit_rex(0, val_reg, addr_reg);
                emit_byte(0x89); /* MOV r/m32, r32 */
            } else if (wsib) {
                emit_rex_sib(1, val_reg, wsib_base, wsib_index);
                emit_byte(0x89); /* MOV r/m64, r64 */
            } else {
                /* Built with statements rather than a conditional expression:
                 * folding the register tests into the argument of emit_byte
                 * came out with a corrupted high nibble when shecc compiled
                 * itself, so the store lost its REX.B and wrote the wrong
                 * register.
                 */
                emit_rex(1, val_reg, addr_reg);
                emit_byte(0x89); /* MOV r/m64, r64 */
            }
            if (wsib)
                emit_mem_sib(val_reg, wsib_base, wsib_index, wsib_scale,
                             wsib_disp);
            else
                emit_mem_base(val_reg, addr_reg, wr_disp, wr_have_disp);
        }
        return;

    case OP_indirect:
        /* CALL r11 (staged by the preceding OP_load_func) */
        emit_byte(REX_W | REX_B);
        emit_byte(0xFF);
        emit_byte(modrm(MOD_DIRECT, 2, 3));
        /* System V returns in RAX; the shared IR expects register 0. */
        emit_byte(REX_W);
        emit_byte(0x89);
        emit_byte(modrm(MOD_DIRECT, 0, 7));
        return;

    default:
        break;
    }
}

/* Globals, functions, and the data sections they live in. */
void emit_global(ph2_ir_t *ph2_ir, int rd, int rs1)
{
    switch (ph2_ir->op) {
    case OP_load_func:
    case OP_global_load_func:
        /* Stage the callee address in R11 for the OP_indirect that follows. R11
         * is outside reg_map, so nothing the allocator owns is disturbed.
         */
        emit_rex(1, rs1, 11);
        emit_byte(0x89);
        emit_byte(modrm(MOD_DIRECT, reg_low3(rs1), 3));
        return;

    case OP_address_of_func:
        /* Store the address of func_name into [rs1]. The address is not known
         * until every function has been emitted, so record it for patching.
         */
        {
            func_t *target = find_func(ph2_ir->func_name);
            emit_byte(REX_W | REX_B);
            emit_byte(0xB8 + 3); /* MOVABS r11, imm64 */
            if (target && target->bbs) {
                if (funcaddr_ref_count >= FUNCADDR_REF_MAX)
                    fatal("x64: too many function-address relocations");
                funcaddr_refs[funcaddr_ref_count].patch_location =
                    elf_code->size;
                funcaddr_refs[funcaddr_ref_count].target_bb = target->bbs;
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
        /* Calculate address using R15 + offset (LEA rd, [r15 + offset]).
         *
         * src0 is a byte offset into the data area, not a register index, so it
         * is read raw rather than through 'rs1', which emit_ph2_ir() has
         * already rewritten through reg_map[].
         */
        int data_ofs = ph2_ir->src0;
        if (data_ofs == 0) {
            /* Just copy R15 to dest register.
             *
             * 0x89 puts the source in the ModRM reg field and the destination
             * in r/m, so REX.R comes from R15 and REX.B from rd. Naming them
             * the other way round emitted "MOV r15, rd", clobbering the global
             * base pointer.
             */
            emit_rex(1, 15, rd); /* reg field is r15, r/m is rd */
            emit_byte(0x89);     /* MOV */
            int rd_low = reg_low3(rd);
            emit_byte(modrm(MOD_DIRECT, 7, rd_low)); /* MOV rd, r15 */
        } else {
            /* LEA rd, [r15 + offset] */
            emit_rex(1, rd, 11); /* r/m is r11 */
            emit_byte(0x8D);     /* LEA */
            int rd_low = reg_low3(rd);

            if (data_ofs < 128) {
                emit_byte(modrm(MOD_DISP8, rd_low, 7)); /* [R15 + disp8] */
                emit_byte(data_ofs);
            } else {
                emit_byte(modrm(MOD_DISP32, rd_low, 7)); /* [R15 + disp32] */
                emit_dword(data_ofs);
            }
        }
    }
        return;

    case OP_global_load: {
        /* Use R15-relative addressing for globals (like ARM uses R12) MOV
         * dest_reg, [R15 + offset]
         */
        int dest_reg = map_ir_reg(ph2_ir->dest);

        /* Pointers must always be loaded as 64-bit */
        int eff_size = ph2_ir->size_bytes;
        if (ph2_ir->is_pointer && eff_size != PTR_SIZE)
            eff_size = PTR_SIZE;

        /* Narrow globals must be loaded at their own width and extended,
         * exactly as OP_load does for locals: a store through a pointer writes
         * only the declared width, so an unconditional 8-byte load would pick
         * up whatever sits in the rest of the slot.
         */
        emit_rex(
            ph2_ir->is_unsigned && eff_size == 4 && !ph2_ir->is_pointer ? 0 : 1,
            dest_reg, 11); /* r/m is r11 */
        if (eff_size == 1 && !ph2_ir->is_pointer) {
            /* MOVSX, not MOVZX: 'char' is signed here, exactly as OP_load
             * treats a one-byte local. Zero-extending made a global char
             * holding -1 compare as 255.
             */
            emit_byte(0x0F); /* MOVSX dest, BYTE [r15 + ofs] */
            emit_byte(ph2_ir->is_unsigned ? 0xB6 : 0xBE);
        } else if (eff_size == 2 && !ph2_ir->is_pointer) {
            emit_byte(0x0F); /* MOVSX dest, WORD [r15 + ofs] */
            emit_byte(ph2_ir->is_unsigned ? 0xB7 : 0xBF);
        } else if (eff_size == 4 && !ph2_ir->is_pointer) {
            if (ph2_ir->is_unsigned) {
                emit_byte(0x8B); /* MOV dest32, DWORD [r15 + ofs] */
            } else
                emit_byte(0x63); /* MOVSXD dest, DWORD [r15 + ofs] */
        } else {
            emit_byte(0x8B); /* MOV dest, QWORD [r15 + ofs] */
        }
        emit_r15_mem(reg_low3(dest_reg), ph2_ir->src0);
    }
        return;

    case OP_global_store: {
        /* Use R15-relative addressing for globals (like ARM uses R12) MOV [R15
         * + offset], src_reg
         */
        int src_reg = rs1; /* honours a redirected source, as OP_store does */

        /* Pointers must always be stored as 64-bit */
        int eff_size = ph2_ir->size_bytes;
        if (ph2_ir->is_pointer && eff_size != PTR_SIZE)
            eff_size = PTR_SIZE;

        /* Store at the slot's declared width, mirroring OP_global_load. A wider
         * store would spill into the neighbouring global; a narrower one would
         * leave stale bytes for the matching load to pick up.
         */
        if (eff_size == 1 && !ph2_ir->is_pointer) {
            /* MOV BYTE [r15 + ofs], src8. REX is mandatory so that source
             * registers 4..7 name SPL/BPL/SIL/DIL rather than AH..BH.
             */
            emit_rex(0, src_reg, 15);
            emit_byte(0x88);
            emit_r15_mem(reg_low3(src_reg), ph2_ir->src1);
        } else if (eff_size == 2 && !ph2_ir->is_pointer) {
            /* MOV WORD [r15 + ofs], src16 */
            emit_byte(0x66); /* operand-size override, before REX */
            emit_rex(0, src_reg, 15);
            emit_byte(0x89);
            emit_r15_mem(reg_low3(src_reg), ph2_ir->src1);
        } else if (eff_size == 4 && !ph2_ir->is_pointer) {
            /* MOV DWORD [r15 + ofs], src32 */
            emit_rex(0, src_reg, 15);
            emit_byte(0x89);
            emit_r15_mem(reg_low3(src_reg), ph2_ir->src1);
        } else {
            /* MOV QWORD [r15 + ofs], src64 */
            emit_rex(1, src_reg, 15);
            emit_byte(0x89);
            emit_r15_mem(reg_low3(src_reg), ph2_ir->src1);
        }
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
        if (data_ofs == 0) {
            /* Just copy R15 to dest register.
             *
             * 0x89 puts the source in the ModRM reg field and the destination
             * in r/m, so REX.R comes from R15 and REX.B from rd. Naming them
             * the other way round emitted "MOV r15, rd", clobbering the global
             * base pointer.
             */
            emit_rex(1, 15, rd); /* reg field is r15, r/m is rd */
            emit_byte(0x89);     /* MOV */
            int rd_low = reg_low3(rd);
            emit_byte(modrm(MOD_DIRECT, 7, rd_low)); /* MOV rd, r15 */
        } else {
            /* LEA rd, [r15 + offset] */
            emit_rex(1, rd, 11); /* r/m is r11 */
            emit_byte(0x8D);     /* LEA */
            int rd_low = reg_low3(rd);

            if (data_ofs < 128) {
                emit_byte(modrm(MOD_DISP8, rd_low, 7)); /* [R15 + disp8] */
                emit_byte(data_ofs);
            } else {
                emit_byte(modrm(MOD_DISP32, rd_low, 7)); /* [R15 + disp32] */
                emit_dword(data_ofs);
            }
        }
    }
        return;

    case OP_load_rodata_address: {
        /* MOVABS rd, imm64 with a placeholder address, recorded for patching
         * once .rodata's final location is known.
         */
        emit_rex(1, -1, rd);
        emit_byte(0xB8 + reg_low3(rd));
        {
            if (rodata_ref_count >= RODATA_REF_MAX)
                fatal("x64: too many .rodata relocations");
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
        emit_rex(1, rs1, rs1);
        emit_byte(0x85);
        emit_byte(modrm(MOD_DIRECT, reg_low3(rs1), reg_low3(rs1)));

        emit_setcc_bool(rd, 0x94); /* SETE */
    }
        return;

    case OP_log_and: {
        /* Logical AND: result = (rs1 != 0) && (rs2 != 0) For now, just set
         * result to 1 - proper implementation needs control flow
         */
        emit_rex(1, -1, rd);
        emit_byte(0xC7); /* MOV rd, imm32 */
        int rd_low_and = reg_low3(rd);
        emit_byte(modrm(MOD_DIRECT, 0, rd_low_and));
        emit_dword(1); /* Always return 1 for now */
        return;
    }

    case OP_log_or: {
        /* Logical OR: result = (rs1 != 0) || (rs2 != 0) */
        /* For now, just set result to 1 - proper implementation needs control
         * flow
         */
        emit_rex(1, -1, rd);
        emit_byte(0xC7); /* MOV rd, imm32 */
        int rd_low_or = reg_low3(rd);
        emit_byte(modrm(MOD_DIRECT, 0, rd_low_or));
        emit_dword(1); /* Always return 1 for now */
        return;
    }

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
        emit_rex(1, rd, rs1);
        if (tsize == 1) {
            emit_byte(0x0F);
            emit_byte(0xBE); /* MOVSX r64, r/m8 */
        } else if (tsize == 2) {
            emit_byte(0x0F);
            emit_byte(0xBF); /* MOVSX r64, r/m16 */
        } else if (tsize == 4) {
            emit_byte(0x63); /* MOVSXD r64, r/m32 */
        } else {
            emit_byte(0x89); /* plain MOV */
            emit_byte(modrm(MOD_DIRECT, reg_low3(rs1), reg_low3(rd)));
            return;
        }
        emit_byte(modrm(MOD_DIRECT, reg_low3(rd), reg_low3(rs1)));
    }
        return;

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
        /* Equal-width signed-to-unsigned casts still need to discard the source
         * register's sign extension on LP64.
         */
        if (ph2_ir->is_unsigned && ph2_ir->size_bytes < PTR_SIZE)
            emit_zero_extend(rd, rs1, ph2_ir->size_bytes);
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
    if (try_fold_mem_dest(ph2_ir))
        return;
    if (try_fold_alu_to_slot(ph2_ir))
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
    if (ph2_ir->op == OP_load && emit_ir_index >= 0 && emit_next_ir &&
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
        if (ph2_ir->op == OP_load && ph2_ir->dest >= 0 &&
            ph2_ir->dest < REG_CNT) {
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
                 * same register -- src1, or a select's src2 -- would be left
                 * looking at whatever the load this skips was going to
                 * overwrite.
                 */
                if (!reg_mirror_sext[i] && src0_override < 0 && emit_next_ir &&
                    op_src0_is_reg(emit_next_ir->op) &&
                    emit_next_ir->src0 == ph2_ir->dest &&
                    emit_next_ir->src1 != ph2_ir->dest &&
                    !(op_src2_is_reg(emit_next_ir->op) &&
                      emit_next_ir->src2 == ph2_ir->dest) &&
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
                reg_mirror_valid[ph2_ir->src0] &&
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
     * and the boolean never needs to exist. The branch ends the block, so a
     * result that were live elsewhere would have been stored between the two
     * instructions -- and then this test would not fire.
     */
    int fuse_cc = branch_cc_for(
        ph2_ir->op, ph2_ir->src0_is_unsigned || ph2_ir->src1_is_unsigned);
    if (fuse_cc && emit_next_ir && emit_next_ir->op == OP_branch &&
        emit_next_ir->src0 == ph2_ir->dest) {
        emit_cmp(ph2_ir->size_bytes, rs1, rs2);
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
    if (ph2_ir->op == OP_bit_and && emit_next_ir &&
        emit_next_ir->op == OP_branch && emit_next_ir->src0 == ph2_ir->dest &&
        emit_ir_index >= 0 && reg_dead_after(emit_ir_index + 2, ph2_ir->dest)) {
        if (src1_const_known) {
            emit_rex(1, 0, rs1);
            emit_byte(0xF7); /* TEST rs1, imm32 */
            emit_byte(modrm(MOD_DIRECT, 0, reg_low3(rs1)));
            emit_dword(src1_const);
        } else {
            emit_rex(1, rs2, rs1);
            emit_byte(0x85); /* TEST rs1, rs2 */
            emit_byte(modrm(MOD_DIRECT, reg_low3(rs2), reg_low3(rs1)));
        }
        fused_cc = 0x85; /* JNZ */
        fused_cc_pending = true;
        return;
    }

    switch (ph2_ir->op) {
    case OP_load_constant: {
        if (ph2_ir->size_bytes == 8) {
            emit_rex(1, -1, rd);
            emit_byte(0xB8 + reg_low3(rd)); /* MOVABS r64, imm64 */
            emit_dword(ph2_ir->src0);
            emit_dword(ph2_ir->src1);
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
        if (ph2_ir->is_unsigned && ph2_ir->size_bytes <= 4) {
            emit_rex(0, -1, rd);
            emit_byte(0xC7);
            emit_byte(modrm(MOD_DIRECT, 0, reg_low3(rd)));
            emit_dword(ph2_ir->src0);
            return;
        }
        emit_rex(1, -1, rd);
        emit_byte(0xC7);
        emit_byte(modrm(MOD_DIRECT, 0, reg_low3(rd)));
        emit_dword(ph2_ir->src0);
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
    case OP_cmov:
    case OP_load:
    case OP_store:
        emit_memory(ph2_ir, rd, rs1);
        break;
    case OP_read:
    case OP_write:
    case OP_indirect:
        emit_read_write(ph2_ir, rd, rs1, rs2);
        break;
    case OP_load_func:
    case OP_global_load_func:
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
        emit_byte(0x55); /* PUSH rbp */
        emit_byte(REX_W);
        emit_byte(0x89);
        int rsp_reg2 = 4;
        int rbp_reg2 = 5;
        emit_byte(modrm(MOD_DIRECT, rsp_reg2, rbp_reg2)); /* MOV rbp, rsp */
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
        {
            int aligned_size = x64_frame_bytes(stack_size, cur_saved_regs);
            emit_byte(REX_W);
            if (aligned_size <= 127) {
                emit_byte(0x83); /* SUB rsp, imm8 */
                int rsp_reg3 = 4;
                emit_byte(modrm(MOD_DIRECT, 5, rsp_reg3));
                emit_byte(aligned_size);
            } else {
                emit_byte(0x81); /* SUB rsp, imm32 */
                int rsp_reg3 = 4;
                emit_byte(modrm(MOD_DIRECT, 5, rsp_reg3));
                emit_dword(aligned_size);
            }
        }
        return;

    case OP_ternary:
        /* Ternary operator - implemented as conditional move pattern This is
         * typically lowered to branches in earlier phases For now, just move
         * rs1 to rd
         */
        emit_mov_reg(rd, rs1);
        return;

    case OP_start:
        /* Start of basic block - update the BB's actual offset */
        if (ph2_ir->next_bb) {
            /* This marks the start of a new basic block */
            ph2_ir->next_bb->elf_offset = elf_code->size;
        }
        /* NOP for alignment */
        emit_byte(0x90);
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
        /* Arguments past MAX_ARGS_IN_REG stay in the caller's frame, and
         * reg-alloc records their offsets relative to it. Convert those to
         * offsets from this function's RSP, which sits below everything the
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
    emit_byte(REX_W | REX_B);
    emit_byte(0x89);
    emit_byte(modrm(MOD_DIRECT, 7, 3));
    /* MOV rdi, rsi (a1) */
    emit_byte(REX_W);
    emit_byte(0x89);
    emit_byte(modrm(MOD_DIRECT, 6, 7));
    /* MOV rsi, rdx (a2) */
    emit_byte(REX_W);
    emit_byte(0x89);
    emit_byte(modrm(MOD_DIRECT, 2, 6));
    /* MOV rdx, rcx (a3) */
    emit_byte(REX_W);
    emit_byte(0x89);
    emit_byte(modrm(MOD_DIRECT, 1, 2));
    /* MOV r10, r8 (a4) */
    emit_byte(REX_W | REX_R | REX_B);
    emit_byte(0x89);
    emit_byte(modrm(MOD_DIRECT, 0, 2));
    /* MOV r8, r9 (a5) */
    emit_byte(REX_W | REX_R | REX_B);
    emit_byte(0x89);
    emit_byte(modrm(MOD_DIRECT, 1, 0));

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
    emit_byte(REX_W | REX_R);
    emit_byte(0x89);
    emit_byte(modrm(MOD_DIRECT, 3, 0));

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

    /* Track which BB each instruction belongs to. This is built during
     * cfg_flatten and used during emission, so it must cover as many
     * instructions as PH2_IR_FLATTEN can hold; a shorter map silently loses the
     * tail of a large program. Build the instruction->BB mapping by traversing
     * in the same order as cfg_flatten
     */
    int instr_idx = 0;
    for (func_t *func = FUNC_LIST.head; func; func = func->next) {
        /* OP_define for the function; it belongs to no BB */
        if (instr_idx < MAX_IR_INSTR)
            instruction_to_bb[instr_idx] = NULL;
        instr_idx++;

        int blocks_base = instr_idx;
        for (basic_block_t *bb = func->bbs; bb; bb = bb->rpo_next) {
            bb->ph2_base = bb->ph2_ir_list.head ? instr_idx : -1;

            /* Map all instructions in this BB */
            for (ph2_ir_t *insn = bb->ph2_ir_list.head; insn;
                 insn = insn->next) {
                if (instr_idx < MAX_IR_INSTR)
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

    if (x64_map) {
        fprintf(stderr, "[count] flattened IR = %d (cap %d)\n", ph2_ir_idx,
                MAX_IR_INSTR);
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

        /* Check if we're starting a new function */
        if (ph2_ir->op == OP_define) {
            emit_func = find_func(ph2_ir->func_name);
            cur_saved_regs = emit_func ? emit_func->saved_regs : 1;
            cur_pinned_regs = emit_func ? emit_func->pinned_regs : 0;
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
        if (i < MAX_IR_INSTR && instruction_to_bb[i]) {
            basic_block_t *current_instr_bb = instruction_to_bb[i];

            /* If this is the first instruction of a BB and offset not set, set
             * it now
             */
            if (current_instr_bb != emit_bb) {
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

        /* Legacy OP_start handling (though it's never generated) */
        if (ph2_ir->op == OP_start && ph2_ir->next_bb) {
            /* OP_start marks beginning of a basic block */
            emit_bb = ph2_ir->next_bb;
            /* Offset will be set inside emit_ph2_ir for OP_start */
        }

        /* Track any BBs referenced by this instruction and set offsets if
         * needed
         */
        if (ph2_ir->next_bb) {
            if (ph2_ir->next_bb->elf_offset == -1) {
                /* This BB hasn't been reached yet in our sequential traversal.
                 * This can happen for BBs that are only reachable by jumps.
                 * We'll set a placeholder offset and let the forward ref system
                 * handle it.
                 */
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
        emit_byte(REX_W);
        emit_byte(0x89);
        emit_byte(modrm(MOD_DIRECT, 4, 6));
        /* Save argc/argv in r12/r13 */
        emit_byte(REX_W | REX_B);
        emit_byte(0x89);
        emit_byte(modrm(MOD_DIRECT, 7, 4));
        emit_byte(REX_W | REX_B);
        emit_byte(0x89);
        emit_byte(modrm(MOD_DIRECT, 6, 5));

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
            emit_byte(0xE8);
            int patch_at_g = elf_code->size;
            int disp_g = GLOBAL_FUNC->bbs->elf_offset - (patch_at_g + 4);
            emit_dword(disp_g);
        }
        /* Restore argc/argv */
        emit_byte(REX_W | REX_B);
        emit_byte(0x8B);
        emit_byte(modrm(MOD_DIRECT, 7, 4));
        emit_byte(REX_W | REX_B);
        emit_byte(0x8B);
        emit_byte(modrm(MOD_DIRECT, 6, 5));
        if (dynlink) {
            /* Hand control to the C runtime:
             *   __libc_start_main(main, argc, argv, 0, 0, 0, stack_end)
             * argc is in RDI and argv in RSI at this point, so both shift one
             * register along to make room for main in RDI.
             */
            emit_byte(REX_W);
            emit_byte(0x89);
            emit_byte(modrm(MOD_DIRECT, 6, 2));
            emit_byte(REX_W);
            emit_byte(0x89);
            emit_byte(modrm(MOD_DIRECT, 7, 6));
            emit_byte(REX_W);
            emit_byte(0xBF); /* movabs rdi, main_wrapper */
            emit_dword(elf_code_start + main_wrapper_ofs);
            emit_dword(0);
            /* init, fini and rtld_fini are unused by modern glibc. */
            emit_byte(0x31);
            emit_byte(0xC9); /* xor ecx,ecx */
            emit_byte(0x45);
            emit_byte(0x31);
            emit_byte(0xC0); /* xor r8d,r8d */
            emit_byte(0x45);
            emit_byte(0x31);
            emit_byte(0xC9); /* xor r9d,r9d */
            /* Align, then push a pad and stack_end so RSP is 16-byte aligned at
             * the call, as the ABI requires.
             */
            emit_byte(REX_W);
            emit_byte(0x83);
            emit_byte(modrm(MOD_DIRECT, 4, 4));
            emit_byte(0xF0); /* and rsp,-16 */
            emit_byte(0x50); /* push rax  (pad) */
            emit_byte(0x54); /* push rsp  (stack_end) */

            emit_byte(0xE8); /* call PLT[1], reserved for __libc_start_main */
            add_pltcall_ref(PLT_FIXUP_SIZE);
            emit_byte(0xF4); /* hlt: __libc_start_main does not return */
        } else {
            /* Align stack by 8 for call */
            emit_byte(REX_W);
            emit_byte(0x83);
            emit_byte(modrm(MOD_DIRECT, 5, 4));
            emit_byte(8);
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
            emit_byte(REX_W);
            emit_byte(0x83);
            emit_byte(modrm(MOD_DIRECT, 0, 4));
            emit_byte(8);
            /* Move exit code (rax) to rdi and syscall exit(60) */
            emit_byte(REX_W);
            emit_byte(0x89);
            emit_byte(modrm(MOD_DIRECT, 0, 7));
            emit_byte(REX_W);
            emit_byte(0xC7);
            emit_byte(modrm(MOD_DIRECT, 0, 0));
            emit_dword(60);
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
        int addr = elf_code_start + funcaddr_refs[i].target_bb->elf_offset;
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
}
