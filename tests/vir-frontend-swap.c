/*
 * The back edge passes the two loop-carried values in swapped order. Edge
 * arguments must capture both values before assigning either destination.
 */
int swap_loop(int n)
{
    int a = 1;
    int b = 2;

    for (int i = 0; i < n; i++) {
        int t = a;

        a = b;
        b = t;
    }
    return a * 10 + b;
}

int main(int argc)
{
    return swap_loop(argc + 2);
}
