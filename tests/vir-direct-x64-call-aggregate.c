struct result {
    int value;
};

struct result helper(void)
{
    struct result r;

    r.value = 29;
    return r;
}

int main(void)
{
    return helper().value;
}
