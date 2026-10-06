int arm(int x)
{
    if (x)
        return 3;
    else
        return 4;
}
int walk(int n)
{
    int s = 0;
    for (int i = 0; i < n; i++) {
        if (i == 2)
            continue;
        else if (i == 6)
            break;
        else
            s += i;
    }
    return s;
}
int choose(int x)
{
    int n = 1;
    if (x) {
        n = 2;
        goto end;
    } else {
        n = 3;
    }
    n += 4;
end:
    return n;
}
int crossed(int x)
{
    int n = 0;
    if (x) {
        goto right;
    left:
        n += 3;
        goto done;
    } else {
    right:
        n += 4;
        if (x)
            goto left;
    }
done:
    return n;
}
int main(void)
{
    int a = 0, b = 0;
    if (1)
        if (0)
            a = 2;
        else
            a = 3;
    else
        a = 4;
    if (0)
        b = 2;
    b += 7;
    if (a != 3 || b != 7)
        return 1;
    if (arm(0) != 4 || arm(1) != 3)
        return 2;
    if (walk(9) != 13)
        return 3;
    if (choose(0) != 7 || choose(1) != 2)
        return 4;
    if (crossed(0) != 4 || crossed(1) != 7)
        return 5;
    return 0;
}
