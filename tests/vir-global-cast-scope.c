int true_value = (int *) "a" && 1;
int *null_value = (int *) "a" && 0;
int *outer_value = (int *) ("a" && 1);
int *nested_value = (int *) (char *) ("a" && 1);
int *conditional_value = (int *) ("a" ? 1 : 0);
char *offset_value = (char *) "abc" + 1;

char *element_value = (char *) "ab"[1];
int element_tail = (char *) "ab"[1] + 1 && 0;
int element_or_tail = (char *) "ab"[1] + 1 || 0;
int adjacent_tail = (char *) "a" "b"[1] && 1;
int element_comparison = (char *) "ab"[1] + 1 == (char *) 99;
char *outer_element = (char *) ("ab"[1] && 1);
unsigned long long scalar_high = (unsigned long long) (char *) 0x100000000ULL;
int high_pointer_equal = (char *) 0x100000000ULL == 0;
int high_pointer_unequal = (char *) 0x100000000ULL != 0;
int carried_pointer_equal = (char *) 0xffffffffU + 1 == 0;
unsigned int shifted32 = (unsigned int) 1 << 31;
long long shifted64 = (long long) 1 << 32;
unsigned long long shifted63 = (unsigned long long) 1 << 63;
int unused_and = 0 && (1 << 32);
int unused_or = 1 || (1 >> 32);
int unused_conditional = 1 ? 7 : (1 << 32);
int logical_signed = ((unsigned long *) "a" && 1) - 2 < 0;
int logical_wide = ((unsigned long long *) "a" || 0) - 2 < 0;
int function_pointer_high_equal = (void (*)(void)) 0x100000000ULL == 0;
int function_pointer_high_truth = (void (*)(void)) 0x100000000ULL && 1;
int answer(void)
{
    return 42;
}
int (*selected)(void) = (int *) "a" ? answer : answer;
void (*null_callback)(void) = (void (*)(void)) "a" && 0;

int main(void)
{
    static int *local_null = (int *) "a" && 0;
    static int *local_outer = (int *) ("a" && 1);
    return logical_signed != 1 || logical_wide != 1 ||
           function_pointer_high_equal != (sizeof(void *) == 4) ||
           function_pointer_high_truth != (sizeof(void *) == 8) ||
           unused_and != 0 || unused_or != 1 || unused_conditional != 7 ||
           shifted32 != 0x80000000U || shifted64 != 0x100000000LL ||
           shifted63 != 0x8000000000000000ULL ||
           scalar_high != (sizeof(void *) == 8 ? 0x100000000ULL : 0ULL) ||
           high_pointer_equal != (sizeof(void *) == 4) ||
           high_pointer_unequal != (sizeof(void *) == 8) ||
           carried_pointer_equal != (sizeof(void *) == 4) || true_value != 1 ||
           null_value || local_null || outer_value != (int *) 1 ||
           nested_value != (int *) 1 || conditional_value != (int *) 1 ||
           local_outer != (int *) 1 || *offset_value != 'b' ||
           element_value != (char *) 98 || element_tail != 0 ||
           element_or_tail != 1 || adjacent_tail != 1 ||
           element_comparison != 1 || outer_element != (char *) 1 ||
           selected() != 42 || null_callback;
}
