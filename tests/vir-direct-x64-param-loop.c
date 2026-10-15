int main(int argc, char **argv)
{
    int i = 0;
    int sum = 0;

    while (i < 3) {
        sum += argc;
        i++;
    }
    return sum;
}
