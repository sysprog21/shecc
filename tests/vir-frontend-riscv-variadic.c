#include <stdarg.h>

long long fixed_target(long long value)
{
    return value * 3;
}

long long fixed_forward(long long value)
{
    return fixed_target(value);
}

long long variadic_target(int count, ...)
{
    va_list args;
    long long value;

    va_start(args, count);
    value = va_arg(args, long long);
    va_end(args);
    return count == 1 ? value * 5 : 0;
}

/* A variadic callee must keep this otherwise fixed-width forwarder out of VIR.
 * RV32 realigns an unnamed i64 differently from a named one.
 */
long long variadic_forward(long long value)
{
    return variadic_target(1, value);
}

int main(void)
{
    return fixed_forward(0x100000003LL) == 0x300000009LL &&
                   variadic_forward(0x100000003LL) == 0x50000000fLL
               ? 47
               : 1;
}
