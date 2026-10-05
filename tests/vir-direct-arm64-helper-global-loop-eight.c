static int slot;

static int helper(int value)
{
    int i = 0;

    while (i < value) {
        int j = 0;

        while (j < value) {
            int k = 0;

            while (k < value) {
                int m = 0;

                while (m < value) {
                    int n = 0;

                    while (n < value) {
                        int p = 0;

                        while (p < value) {
                            int q = 0;

                            while (q < value) {
                                int r = 0;

                                while (r < value) {
                                    slot = 1;
                                    r++;
                                }
                                q++;
                            }
                            p++;
                        }
                        n++;
                    }
                    m++;
                }
                k++;
            }
            j++;
        }
        i++;
    }
    return slot;
}

int main(int argc, char **argv)
{
    (void) argv;
    return helper(argc);
}
