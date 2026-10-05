static int helper(int value)
{
    int i = 0;

    while (i < value) {
        i++;
        if (i == 2)
            break;
    }
    return i;
}

int main(int argc)
{
    return helper(3 * argc - 2);
}
