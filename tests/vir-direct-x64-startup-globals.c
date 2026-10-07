int seed = 29;
int *saved_seed = &seed;

int main(int argc, char **argv)
{
    return *saved_seed + (argv[0] != (char *) 0) - argc;
}
