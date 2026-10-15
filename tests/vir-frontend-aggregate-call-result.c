struct pair {
    int value;
};

static struct pair make_pair(int value)
{
    struct pair result;

    result.value = value;
    return result;
}

int read_pair(void)
{
    return make_pair(31).value;
}

int main(void)
{
    return read_pair();
}
