int scalar = 9;
unsigned int unsigned_scalar = 11;
long long wide = 17;
unsigned long long unsigned_wide = 19;
struct S {
    int n;
} record = {23};
int array[3] = {2, 4, 6};
int index = 1;
volatile int calls;
int fn(void)
{
    calls++;
    return 29;
}

int undefined(void);
int declared_false = 1 ? 7 : undefined();
int declared_true = 0 ? undefined() : 7;

int call_false = 1 ? 7 : fn();
int call_true = 0 ? fn() : 7;
int scalar_false = 1 ? 7 : scalar;
int scalar_true = 0 ? scalar : 7;
int unsigned_result = 1 ? 7 : unsigned_scalar;
int wide_result = 1 ? 7 : wide;
int unsigned_wide_result = 0 ? unsigned_wide : 7;
int field_result = 1 ? 7 : record.n;
int index_result = 1 ? 7 : array[index];

char *null_arithmetic = 1 ? "a" : 1 - 1;
char *null_cast = 1 ? "b" : (int) 0;
char *null_sizeof = 1 ? "c" : sizeof(int) - 4;
char *null_conditional = 1 ? "d" : (0 ? 0 : 0);
char *null_array[] = {1 ? "e" : 1 - 1, 1 ? "f" : (int) 0,
                      1 ? "g" : sizeof(int) - 4, 1 ? "h" : (0 ? 0 : 0)};

char *mixed_unsigned_dead = 1 ? "i" : (1 ? 0 : 0U);
char *mixed_wide_dead = 1 ? "j" : (1 ? 0 : 0LL);
char *mixed_unsigned_selected = 0 ? "k" : (1 ? 0 : 0U);
char *mixed_wide_selected = 0 ? "l" : (1 ? 0 : 0LL);
char *signed_zero_dead = 1 ? "m" : (1 ? -1 : 0LL) + 1;
char *unsigned_zero_dead = 1 ? "n" : (1 ? -1 : 0ULL) + 1;
long long signed_conditional = 1 ? -1 : 0LL;
unsigned long long unsigned_conditional = 1 ? -1 : 0ULL;

char *wide_compare_null = 1 ? "o" : (1ULL == 1 ? 0 : 5);
char *wide_negation_null = 1 ? "p" : !(1ULL + 0);
char *unsigned_comparison_null = 1 ? "q" : (3U == 3 ? 0 : 5);
char *wide_compare_array[] = {1 ? "r" : (1ULL == 1 ? 0 : 5)};

extern int discarded_unsized_array[];
int unsized_selected = 41;
int *unsized_true = 1 ? &unsized_selected : discarded_unsized_array;
int *unsized_false = 0 ? discarded_unsized_array : &unsized_selected;

int first_array[3] = {2, 3, 5};
int second_array[3] = {7, 11, 13};
int *chosen_first_array = 1 ? first_array : second_array;
int *chosen_second_array = 0 ? first_array : second_array;
int rows[2][3] = {{17, 19, 23}, {29, 31, 37}};
int *chosen_row = 1 ? rows[0] : rows[1];
int (*chosen_whole_rows)[3] = 1 ? rows : rows;
int array_increment(int value)
{
    return value + 1;
}
typedef int (*array_callback_t)(int);
array_callback_t first_callbacks[2] = {array_increment, array_increment};
array_callback_t second_callbacks[2] = {array_increment, array_increment};
array_callback_t *chosen_callbacks = 1 ? first_callbacks : second_callbacks;
typedef int *element_pointer_t;
element_pointer_t pointer_elements[2] = {first_array, second_array};
element_pointer_t other_elements[2] = {second_array, first_array};
element_pointer_t *chosen_pointer_elements =
    1 ? pointer_elements : other_elements;

int main(void)
{
    return *unsized_true != 41 || *unsized_false != 41 ||
           chosen_whole_rows[1][2] != 37 || chosen_callbacks[1](4) != 5 ||
           chosen_first_array[2] != 5 || chosen_second_array[2] != 13 ||
           chosen_row[2] != 23 || chosen_pointer_elements[1][2] != 13 ||
           wide_compare_null[0] != 'o' || wide_negation_null[0] != 'p' ||
           unsigned_comparison_null[0] != 'q' ||
           wide_compare_array[0][0] != 'r' || mixed_unsigned_dead[0] != 'i' ||
           mixed_wide_dead[0] != 'j' || mixed_unsigned_selected != 0 ||
           mixed_wide_selected != 0 || signed_zero_dead[0] != 'm' ||
           unsigned_zero_dead[0] != 'n' || signed_conditional != -1LL ||
           unsigned_conditional != ~0ULL || null_arithmetic[0] != 'a' ||
           null_cast[0] != 'b' || null_sizeof[0] != 'c' ||
           null_conditional[0] != 'd' || null_array[0][0] != 'e' ||
           null_array[1][0] != 'f' || null_array[2][0] != 'g' ||
           null_array[3][0] != 'h' || declared_false != 7 ||
           declared_true != 7 || calls != 0 || call_false != 7 ||
           call_true != 7 || scalar_false != 7 || scalar_true != 7 ||
           unsigned_result != 7 || wide_result != 7 ||
           unsigned_wide_result != 7 || field_result != 7 || index_result != 7;
}
