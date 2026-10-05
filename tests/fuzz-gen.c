/*
 * Random program generator for tests/fuzz.sh.
 *
 * Each seed yields one C program that prints a checksum of everything it
 * computed. The programs lean on the shapes the back end rewrites: copies
 * between variables, post-increments whose old value is used, literal stores
 * through a variable index at every width, compares feeding branches, masked
 * tests, and short loops. Every operation is defined under -fwrapv (shift
 * counts are masked, divisors are forced odd and unsigned), so the host
 * compiler's output is the reference.
 */
#include <stdio.h>
#include <stdlib.h>

#define NVARS 10

static unsigned int state;

static int rnd(int n)
{
    state = state * 1103515245u + 12345u;
    return (int) ((state >> 16) % (unsigned) n);
}

static const char *var(void)
{
    static const char *names[NVARS] = {"a", "b", "c", "s", "t",
                                       "u", "v", "w", "x", "y"};
    return names[rnd(NVARS)];
}

/* A variable other than @v, for statements that would be unsequenced if both
 * sides named the same one.
 */
static const char *other(const char *v)
{
    const char *w;

    do
        w = var();
    while (w == v);
    return w;
}

static void expr(int depth);

/* Everything the generator prints is decided one rnd() call at a time, into
 * locals, before the printf that uses it: the order a compiler evaluates
 * printf's arguments in is unspecified, and a seed has to mean one program
 * whichever compiler built the generator.
 */
static void operand(void)
{
    int kind = rnd(9);

    switch (kind) {
    case 0: {
        int small = rnd(4);
        int value = small ? rnd(16) : rnd(100000) - 50000;

        printf("%d", value);
        break;
    }
    case 1:
        printf("i");
        break;
    case 2:
        printf("arr[(");
        expr(0);
        printf(") & 15]");
        break;
    case 3: {
        int index = rnd(16);

        printf("g[%d]", index);
        break;
    }
    case 4:
        printf("gc[(");
        expr(0);
        printf(") & 15]");
        break;
    case 5: {
        /* Near the limits of int, where a wrap or a lost high word shows. */
        int below = rnd(1000);

        printf("%uu", 0x7fffffffu - (unsigned) below);
        break;
    }
    default:
        printf("%s", var());
        break;
    }
}

static void expr(int depth)
{
    static const char *ops[] = {"+",  "-",  "*", "&",  "|", "^",
                                "==", "!=", "<", "<=", ">", ">="};

    if (depth <= 0 || rnd(3) == 0) {
        operand();
        return;
    }

    int kind = rnd(10);

    switch (kind) {
    case 0:
        printf("((");
        expr(depth - 1);
        printf(") << ((");
        expr(depth - 1);
        printf(") & 31))");
        return;
    case 1:
        printf("((");
        expr(depth - 1);
        printf(") >> ((");
        expr(depth - 1);
        printf(") & 31))");
        return;
    case 2: {
        int divide = rnd(2);

        printf("((unsigned) (");
        expr(depth - 1);
        printf(") %s ((unsigned) (", divide ? "/" : "%");
        expr(depth - 1);
        printf(") | 1))");
        return;
    }
    case 3:
        printf("(-(");
        expr(depth - 1);
        printf("))");
        return;
    case 4:
        printf("(!(");
        expr(depth - 1);
        printf("))");
        return;
    case 5:
        printf("((int) (");
        expr(depth - 1);
        printf(") < (int) (");
        expr(depth - 1);
        printf("))");
        return;
    default: {
        printf("(");
        expr(depth - 1);
        int op = rnd(12);
        printf(" %s ", ops[op]);
        expr(depth - 1);
        printf(")");
        return;
    }
    }
}

static void stmt(int depth, int indent)
{
    const char *v = var();
    int kind = rnd(depth > 0 ? 24 : 12);

    printf("%*s", indent, "");
    switch (kind) {
    case 0: {
        const char *w = var();

        printf("%s = %s;\n", v, w);
        return;
    }
    case 1: {
        const char *w = other(v);

        printf("%s += %s++;\n", v, w);
        return;
    }
    case 2: {
        const char *w = var();
        int zero = rnd(3);
        int value = zero ? 0 : rnd(1000);

        printf("arr[(%s) & 15] = %d;\n", w, value);
        return;
    }
    case 3: {
        const char *w = var();
        int value = rnd(512) - 256;

        printf("gc[(%s) & 15] = %d;\n", w, value);
        return;
    }
    case 4: {
        const char *w = var();
        int value = rnd(140000) - 70000;

        printf("gs[(%s) & 15] = %d;\n", w, value);
        return;
    }
    case 5: {
        const char *w = var(), *x = var();
        int scale = rnd(9) - 4;
        const char *y = var();

        printf("q[(%s) & 7] = q[(%s) & 7] * %d + %s;\n", w, x, scale, y);
        return;
    }
    case 6: {
        const char *w = var();

        printf("g[(%s) & 15] = ", w);
        expr(2);
        printf(";\n");
        return;
    }
    case 7: {
        const char *w = var(), *x = var();
        int value = rnd(64);

        printf("%s = %s; %s = %d;\n", v, w, x, value);
        return;
    }
    case 8: {
        const char *w = var();

        printf("%s = k(%s);\n", v, w);
        return;
    }
    case 9: {
        const char *w = var(), *x = var(), *y = var();

        printf("%s = h(%s, %s) + %s;\n", v, w, x, y);
        return;
    }
    case 10:
    case 11:
        printf("%s = ", v);
        expr(3);
        printf(";\n");
        return;
    case 12:
        printf("if (");
        expr(2);
        printf(") {\n");
        stmt(depth - 1, indent + 4);
        printf("%*s} else {\n", indent, "");
        stmt(depth - 1, indent + 4);
        printf("%*s}\n", indent, "");
        return;
    case 13: {
        const char *w = var();
        int bit = 1 << rnd(8);

        printf("if (%s & %d)\n", w, bit);
        stmt(depth - 1, indent + 4);
        return;
    }
    case 14: {
        int trips = 1 + rnd(5);
        const char *w = var();

        printf("for (int j = 0; j < %d; j++) {\n", trips);
        printf("%*s%s += j;\n", indent + 4, "", w);
        stmt(depth - 1, indent + 4);
        printf("%*s}\n", indent, "");
        return;
    }
    case 15: {
        /* A walk along one row, which strength reduction turns into pointers
         * and whose exit test can then move onto one of them. Sometimes the
         * counter is read after the loop.
         */
        int from = rnd(4), to = rnd(33), row = rnd(4);
        int scale = 1 + rnd(5);
        int keep = rnd(2);

        printf("{\n%*sint j;\n", indent + 4, "");
        printf("%*sfor (j = %d; j < %d; j++)\n", indent + 4, "", from, to);
        printf("%*s%s += m[%d][j] * %d + n[j][%d];\n", indent + 8, "", v, row,
               scale, row);
        if (keep) {
            const char *w = var();

            printf("%*s%s += j;\n", indent + 4, "", w);
        }
        printf("%*s}\n", indent, "");
        return;
    }
    case 16: {
        int row = rnd(4);
        const char *w = var();

        printf("for (int j = 0; j < 32; j++)\n");
        printf("%*sm[%d][j] = %s + j;\n", indent + 4, "", row, w);
        return;
    }
    case 17: {
        /* The counter advances before the access reads it. */
        int to = rnd(31), row = rnd(4);

        printf("{\n%*sint j = -1;\n", indent + 4, "");
        printf("%*swhile (j < %d) {\n", indent + 4, "", to);
        printf("%*sj++;\n", indent + 8, "");
        printf("%*s%s += m[%d][j];\n", indent + 8, "", v, row);
        printf("%*s}\n%*s}\n", indent + 4, "", indent, "");
        return;
    }
    case 18: {
        /* An unsigned counter starting below zero never enters its loop. */
        int below = 1 + rnd(8), to = rnd(24), row = rnd(4);

        printf("for (unsigned int j = -%d; j < %d; j++)\n", below, to);
        printf("%*s%s += n[(j + %d) & 31][%d];\n", indent + 4, "", v, below,
               row);
        return;
    }
    case 19: {
        int to = rnd(33), row = rnd(4);

        printf("for (long long j = 0; j < %d; j++)\n", to);
        printf("%*s%s += m[%d][j];\n", indent + 4, "", v, row);
        return;
    }
    case 20: {
        /* A reduced loop left early. */
        int row = rnd(4), stop = rnd(40);

        printf("for (int j = 0; j < 32; j++) {\n");
        printf("%*sif (m[%d][j] == %d)\n", indent + 4, "", row, stop);
        printf("%*sbreak;\n", indent + 8, "");
        printf("%*s%s += m[%d][j];\n", indent + 4, "", v, row);
        printf("%*s}\n", indent, "");
        return;
    }
    case 21: {
        /* An int stored into a long long object: the store must widen. */
        const char *w = var(), *x = var();

        printf("q[(%s) & 7] = (int) (%s * 70001u);\n", w, x);
        return;
    }
    default: {
        const char *w = var();

        printf("%s = h(%s, ", v, w);
        expr(1);
        printf(");\n");
        return;
    }
    }
}

int main(int argc, char **argv)
{
    int nfuncs;

    if (argc != 2) {
        fprintf(stderr, "usage: %s <seed>\n", argv[0]);
        return 1;
    }
    state = (unsigned) atoi(argv[1]);
    nfuncs = 1 + rnd(3);

    printf("unsigned g[16];\n");
    printf("int m[4][32];\n");
    printf("int n[32][4];\n");
    printf("signed char gc[16];\n");
    printf("short gs[16];\n");
    printf("long long q[8];\n\n");
    int kmul = 1 + rnd(9);
    printf("unsigned k(unsigned x)\n{\n");
    printf("    return x * %d + 1;\n}\n\n", kmul);
    int hmul = 1 + rnd(9);
    printf("unsigned h(unsigned x, unsigned y)\n{\n");
    printf("    return (x ^ (y << 3)) + (x >> 2) * %d;\n}\n\n", hmul);

    for (int f = 0; f < nfuncs; f++) {
        printf("unsigned f%d(unsigned a, unsigned b, unsigned c)\n{\n", f);
        int s0 = rnd(10), u0 = rnd(10);
        printf("    unsigned s = %d, t = a, u = %d, i = 0;\n", s0, u0);
        int v0 = rnd(10), w0 = rnd(10), x0 = rnd(1000);
        printf("    unsigned v = b + %d, w = c * %d, x = %d, y = a ^ b;\n", v0,
               w0, x0);
        printf("    unsigned arr[16];\n");
        printf("    for (int k = 0; k < 16; k++)\n");
        int amul = 1 + rnd(7);
        printf("        arr[k] = k * %d;\n", amul);
        int trips = 1 + rnd(12);
        printf("    for (i = 0; i < %d; i++) {\n", trips);
        for (int n = 3 + rnd(12); n > 0; n--)
            stmt(2, 8);
        printf("    }\n");
        for (int n = rnd(3); n > 0; n--)
            stmt(1, 4);
        printf(
            "    return a ^ b ^ c ^ s ^ t ^ u ^ v ^ w ^ x ^ y ^ "
            "arr[s & 15];\n}\n\n");
    }

    printf("int main()\n{\n    unsigned acc = 0;\n\n");
    printf("    for (int k = 0; k < 4; k++)\n");
    printf("        for (int j = 0; j < 32; j++) {\n");
    printf("            m[k][j] = k * 7 + j;\n");
    printf("            n[j][k] = j - k;\n");
    printf("        }\n");
    for (int k = 0; k < 3; k++) {
        int callee = rnd(nfuncs), a0 = rnd(1000);
        int small = rnd(2);
        int b0 = small ? rnd(10) : rnd(100000);
        int c0 = rnd(50);
        printf("    acc = acc * 31 + f%d(%d, %d, %d);\n", callee, a0, b0, c0);
    }
    printf("    for (int k = 0; k < 16; k++)\n");
    printf("        acc = acc * 7 + g[k] + gc[k] * 3 + gs[k];\n");
    printf("    for (int k = 0; k < 8; k++)\n");
    printf(
        "        acc = acc * 5 + (unsigned) q[k] + (unsigned) (q[k] >> "
        "32);\n");
    printf("    for (int k = 0; k < 4; k++)\n");
    printf("        for (int j = 0; j < 32; j++)\n");
    printf("            acc = acc * 3 + m[k][j];\n");
    printf("    printf(\"%%x\\n\", acc);\n    return 0;\n}\n");
    return 0;
}
