char padding[4048];
volatile int target;

int main(int argc)
{
    volatile int *p = &target;

    *p = argc + 5;
    return *p;
}
