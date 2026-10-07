int main(int argc)
{
    int i = argc;
    int x = 1;
    int y = 2;

    while (i > 0) {
        int t = x;

        x = y;
        y = t;
        i = i - 1;
    }
    return x * 10 + y;
}
