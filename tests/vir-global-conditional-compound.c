struct small {
    int x;
};
struct layout {
    char c;
    long long n;
    int x;
};
struct small *true_arm = 1 ? &(struct small) {1} : 0;
struct small *false_arm = 0 ? 0 : &(struct small) {2};
struct small *nested_true = 1 ? (0 ? 0 : &(struct small) {3}) : 0;
struct small *nested_false = 0 ? 0 : (1 ? &(struct small) {4} : 0);
struct layout *record = 0 ? 0 : &(struct layout) {7, 4294967299LL, 5};
struct layout *both_records =
    0 ? &(struct layout) {1, 2, 3} : &(struct layout) {9, 4294967301LL, 10};
const struct small *qualified = 1 ? &(struct small) {6} : 0;

int plus(int x)
{
    return x + 1;
}
int minus(int x)
{
    return x - 1;
}
typedef int (*callback_t)(int);

/* Reading the initializer of the callback compound literal temporarily changes
 * the destination. Restore it before reading another outer conditional arm.
 */
callback_t *callback = 1 ? &(callback_t) {1 ? plus : minus} : 0;
callback_t *other_callback = 0 ? 0 : &(callback_t) {0 ? minus : plus};
callback_t *both_callbacks = 0 ? &(callback_t) {minus} : &(callback_t) {plus};
struct small *after_callback = 0 ? 0 : &(struct small) {8};

int main(void)
{
    return true_arm->x != 1 || false_arm->x != 2 || nested_true->x != 3 ||
           nested_false->x != 4 || record->c != 7 ||
           record->n != 4294967299LL || record->x != 5 ||
           both_records->c != 9 || both_records->n != 4294967301LL ||
           both_records->x != 10 || qualified->x != 6 ||
           (*callback)(10) != 11 || (*other_callback)(10) != 11 ||
           (*both_callbacks)(10) != 11 || after_callback->x != 8;
}
