/* Check folded global comparisons after the usual arithmetic conversions. */
#define RELATIONS(name, a, b, less, equal)                    \
    static int name[] = {(a) < (b),  (a) <= (b), (a) > (b),   \
                         (a) >= (b), (a) == (b), (a) != (b)}; \
    static int name##_expected[] = {less,                     \
                                    (less) || (equal),        \
                                    !(less) && !(equal),      \
                                    !(less),                  \
                                    equal,                    \
                                    !(equal)}

RELATIONS(s32, (-2147483647 - 1), 2147483647, 1, 0);
RELATIONS(u32, 0xffffffffU, 0U, 0, 0);
RELATIONS(mixed32, -1, 0U, 0, 0);
RELATIONS(widen_signed, -1LL, 0xffffffffU, 1, 0);
RELATIONS(widen_unsigned, -1, 0ULL, 0, 0);
RELATIONS(s64, (-9223372036854775807LL - 1LL), 9223372036854775807LL, 1, 0);
RELATIONS(u64, 18446744073709551615ULL, 0ULL, 0, 0);
RELATIONS(mixed64, -1LL, 18446744073709551615ULL, 0, 1);
RELATIONS(sign_bit, 9223372036854775808ULL, 9223372036854775807LL, 0, 0);
RELATIONS(equal32, -1, 0xffffffffU, 0, 1);
RELATIONS(equal_widen, 0xffffffffU, 4294967295LL, 0, 1);
RELATIONS(negative64, -4294967296LL, -1LL, 1, 0);
RELATIONS(low_word, -2147483649LL, -1LL, 1, 0);

#if (-9223372036854775807LL - 1LL) >= 9223372036854775807LL
#error signed preprocessor comparison
#endif
#if - 1LL != 18446744073709551615ULL
#error unsigned preprocessor conversion
#endif
#if 9223372036854775808ULL <= 9223372036854775807LL
#error unsigned preprocessor ordering
#endif

#if - 2147483649LL >= -1LL
#error signed low-word ordering
#endif

#define CHECK(name)                        \
    for (int i = 0; i < 6; i++)            \
        if (name[i] != name##_expected[i]) \
    return 1

int main(void)
{
    CHECK(s32);
    CHECK(u32);
    CHECK(mixed32);
    CHECK(widen_signed);
    CHECK(widen_unsigned);
    CHECK(s64);
    CHECK(u64);
    CHECK(mixed64);
    CHECK(sign_bit);
    CHECK(equal32);
    CHECK(equal_widen);
    CHECK(negative64);
    CHECK(low_word);
    return 0;
}
