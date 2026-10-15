typedef int *pointer_t;
typedef pointer_t (*const callback_t)(int);
static callback_t callback = 0;
int main(void)
{
    callback = 0;
    return 0;
}
