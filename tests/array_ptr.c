#include <stdio.h>

int add(int a, int b)
{
    return a + b;
}

int main(void)
{
    int *a = (int[]){1, 2, 3, 4, 5};

    printf("Testing basic array compound literal:\n");
    for (int i = 0; i < 5; i++)
        printf("  a[%d] = %d\n", i, a[i]);

    int sum = a[0] + a[4];
    printf("Sum = %d (expect 6)\n", sum);

    int base = 50;
    int combined = base + (int[]){100, 200};
    printf("base + (int[]){100,200} = %d (expect 150)\n", combined);

    int acc = 25;
    acc += (int[]){100, 200};
    printf("acc after += array literal = %d (expect 125)\n", acc);

    int flag = 1;
    int ternary_val = flag ? (int[]){25, 50} : (int){15};
    printf("ternary true branch = %d (expect 25)\n", ternary_val);

    flag = 0;
    ternary_val = flag ? (int[]){25, 50} : (int){15};
    printf("ternary false branch = %d (expect 15)\n", ternary_val);

    int func_val = add((int){5}, (int[]){10, 20, 30});
    printf("add((int){5}, (int[]){10,20,30}) = %d (expect 15)\n", func_val);

    return 0;
}
