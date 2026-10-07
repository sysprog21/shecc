int value = 7;
int *pointer = &value;
volatile int *get(void)
{
    return &value;
}
int **get_pointer(void)
{
    return &pointer;
}
volatile int *(*global_get)(void) = (volatile int *(*) (void) ) get;
volatile int *(**global_slot)(void) = &(volatile int *(*) (void) ) {get};
int *(*outer_cast)(void) = (int *(*) (void) )(const int *(*) (void) ) get;
int touch(void)
{
    volatile int *(*local)(void) = (volatile int *(*) (void) ) get;
    *(1 ? (volatile int *(*) (void) ) get : local)();
    *global_get();
    *(*global_slot)();
    return **((int *volatile *(*) (void) ) get_pointer)();
}
int main(void)
{
    return touch() != 7 || *outer_cast() != 7;
}
