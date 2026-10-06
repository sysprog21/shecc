int data[2][3] = {{1, 2, 3}, {4, 5, 6}};
int inc(int x)
{
    return x + 1;
}
int (*callback)(int) = inc;
int (**slot)(int) = &callback;
int (*cast_callback)(int) = (int (*)(int)) inc;
int (**literal_slot)(int) = &(int (*)(int)) {inc};
typedef int (*Callback)(int);
Callback callbacks[2] = {inc, inc};
Callback *typed_literal_slot = &(Callback) {inc};
Callback *array_slot = callbacks + 1;
int *row = data[1];
int (*whole)[3] = &data[1];
char *byte = (char *) data + sizeof(int);
int *literal = &(int) {8};
struct Record {
    int n;
    int *p;
    int (*f)(int);
};
struct Record record = {9, data[0] + 2, inc};
struct Record *record_literal = &(struct Record) {11, data[1], inc};
const char *string = "ab" + 1;
const char *string_element = &"cd"[1];
_Bool high = 0x100000000ULL;
_Bool high_only = 1ULL << 63;
_Bool high_plus = 0x100000001ULL;
_Bool zero = 0ULL;
_Bool address_true = data || 0;
_Bool string_true = "x" && 1;
int narrow = 1 ? 7 : 1 / 0;
unsigned long long wide = 1 ? 0x100000000ULL : 1 / 0;
int scope(void)
{
    enum { N = 2 };
    static int *p = data[0] + N;
    return *p;
}
int twice(int n)
{
    return 2 * n;
}
int (*raw_callbacks[3])(int) = {inc, twice, inc};
int (**raw_middle)(int) = raw_callbacks + 1;
int (**raw_end)(int) = raw_callbacks + 3;
int (*raw_rows[2][2])(int) = {{inc, twice}, {twice, inc}};
typedef int (*stride_callback)(int);
stride_callback typed_callbacks[3] = {inc, twice, inc};
stride_callback *typed_middle = typed_callbacks + 1;
stride_callback *typed_end = typed_callbacks + 3;
stride_callback typed_rows[2][2] = {{inc, twice}, {twice, inc}};
int (**raw_row)(int) = raw_rows[1];
stride_callback *typed_row = typed_rows[1];
typedef int direct_callback(int);
direct_callback *null_direct = 0;
Callback null_typed = 0;
int (**null_slot)(int) = &(int (*)(int)) {0};
Callback null_void = (void *) 0;
Callback null_chain = (void *) (char *) 0;
Callback null_callbacks[3] = {0, (void *) 0, (void *) (char *) 0};
int (*nested_null)(void) = (int (*)(void))(int (*)(int)) 0;
int (*nested_integer)(void) = (int (*)(void))(int (*)(int)) 1;
struct callback_cast_box {
    int x;
    int (*callback)(int);
} cast_box = {0, inc};
char *cast_member = (char *) &cast_box.callback;
void *cast_member_void = (void *) &cast_box.callback;
char *cast_element = (char *) &raw_callbacks[1];
int (**cast_member_slot)(int) = (int (**)(int))(char *) &cast_box.callback;
long long wide_callback(int n)
{
    return n + 1LL;
}
long long (*wide_callbacks[3])(int) = {wide_callback, wide_callback,
                                       wide_callback};
long long (**wide_middle)(int) = wide_callbacks + 1;
long long (**wide_element)(int) = &wide_callbacks[1];
int main(void)
{
    if (wide_middle != wide_element || (*wide_middle)(3) != 4 ||
        (*wide_element)(5) != 6)
        return 11;
    if (nested_null || nested_integer != (int (*)(void)) 1 ||
        cast_member != (char *) &cast_box.callback ||
        cast_member_void != (void *) &cast_box.callback ||
        cast_element != (char *) &raw_callbacks[1] ||
        (*cast_member_slot)(3) != 4)
        return 10;
    if (null_direct || null_typed || *null_slot || null_void || null_chain ||
        null_callbacks[0] || null_callbacks[1] || null_callbacks[2])
        return 9;
    if ((*raw_middle)(3) != 6 || raw_end - raw_middle != 2 ||
        (*raw_row)(3) != 6 || (*typed_middle)(3) != 6 ||
        typed_end - typed_middle != 2 || (*typed_row)(3) != 6)
        return 8;
    if ((*slot)(4) != 5 || cast_callback(4) != 5 || (*literal_slot)(4) != 5 ||
        (*typed_literal_slot)(4) != 5 || (*array_slot)(4) != 5)
        return 1;
    if (*row != 4 || (*whole)[2] != 6 || *(int *) byte != 2 || *literal != 8)
        return 2;
    if (record.n != 9 || *record.p != 3 || record.f(1) != 2)
        return 3;
    if (record_literal->n != 11 || *record_literal->p != 4 ||
        record_literal->f(2) != 3)
        return 4;
    if (*string != 'b' || *string_element != 'd' || scope() != 3)
        return 5;
    if (!high || !high_only || !high_plus || zero || !address_true ||
        !string_true)
        return 6;
    if (narrow != 7 || wide != 0x100000000ULL)
        return 7;
    return 0;
}
