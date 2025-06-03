#include "../src/globals.c"

/*
 * Simple assert‑style helper.
 */
void ensure(bool cond, char *message)
{
    if (!cond) {
        printf("ERROR: %s\n", message);
        exit(1);
    }
}

/* === Test: initialization and basic properties === */
void test_init_and_properties(arena_t *arena)
{
    printf("=== Running test_init_and_properties ===\n");

    dynarr_t *bytes = dynarr_init(arena, 0, 1);
    ensure(bytes->size == 0, "bytes initial size == 0");
    ensure(bytes->capacity == 0, "bytes initial capacity == 0");
    ensure(bytes->elem_size == 1, "bytes elem_size == 1");

    dynarr_t *words = dynarr_init(arena, 8, sizeof(int));
    ensure(words->size == 0, "words initial size == 0");
    ensure(words->capacity >= 8, "words capacity >= init_cap (8)");
    ensure(words->elem_size == sizeof(int), "words elem_size == sizeof(int)");

    printf("[OK] init & property checks passed\n\n");
}

/* === Test: push_byte / get_byte === */
void test_push_and_get_byte(arena_t *arena)
{
    printf("=== Running test_push_and_get_byte ===\n");

    dynarr_t *arr = dynarr_init(arena, 4, 1);

    char sample[] = "DynamicArray";
    for (int i = 0; sample[i] != '\0'; ++i) {
        dynarr_push_byte(arr, sample[i]);
    }

    ensure(arr->size == strlen(sample), "size after pushes == strlen(sample)");
    for (int i = 0; i < arr->size; ++i) {
        char c = dynarr_get_byte(arr, i);
        ensure(c == sample[i], "push/get byte round-trip");
    }

    printf("[OK] push_byte / get_byte passed\n\n");
}

/* === Test: push_word / get_word === */
void test_push_and_get_word(arena_t *arena)
{
    printf("=== Running test_push_and_get_word ===\n");

    dynarr_t *arr = dynarr_init(arena, 0, sizeof(int));

    for (int i = 0; i < 32; ++i)
        dynarr_push_word(arr, i * 3);

    ensure(arr->size == 32, "size after 32 pushes == 32");

    for (int i = 0; i < 32; ++i) {
        int v = dynarr_get_word(arr, i);
        ensure(v == i * 3, "push/get word round‑trip");
    }

    printf("[OK] push_word / get_word passed\n\n");
}

/* === Test: push_raw, set_raw, get_raw === */
typedef struct {
    int a;
    int b;
} Pair;

void test_push_raw_and_set_raw(arena_t *arena)
{
    printf("=== Running test_push_raw_and_set_raw ===\n");

    dynarr_t *arr = dynarr_init(arena, 0, sizeof(Pair));

    Pair p1;
    p1.a = 1;
    p1.b = 2;
    Pair p2;
    p2.a = 3;
    p2.b = 4;
    dynarr_push_raw(arr, &p1);
    dynarr_push_raw(arr, &p2);

    Pair *got1 = dynarr_get_raw(arr, 0);
    Pair *got2 = dynarr_get_raw(arr, 1);
    ensure(got1->a == 1 && got1->b == 2, "get_raw element 0 matches");
    ensure(got2->a == 3 && got2->b == 4, "get_raw element 1 matches");

    got1->a = 100;
    got1->b = 200;
    got1 = dynarr_get_raw(arr, 0);
    ensure(got1->a == 100 && got1->b == 200,
           "get_raw element 0 reflects modification");

    Pair p3;
    p3.a = 7;
    p3.b = 8;
    dynarr_set_raw(arr, 0, &p3);
    got1 = dynarr_get_raw(arr, 0);
    ensure(got1->a == 7 && got1->b == 8, "set_raw overwrote element 0");

    printf("[OK] push_raw / set_raw / get_raw passed\n\n");
}

/* === Test: extend and resize === */
void test_extend_and_resize(arena_t *arena)
{
    printf("=== Running test_extend_and_resize ===\n");

    dynarr_t *arr = dynarr_init(arena, 2, 1);

    char buf1[5];
    buf1[0] = 'h';
    buf1[1] = 'e';
    buf1[2] = 'l';
    buf1[3] = 'l';
    buf1[4] = 'o';
    dynarr_extend(arr, buf1, 5);

    ensure(arr->size == 5, "size after first extend == 5");
    ensure(dynarr_get_byte(arr, 0) == 'h' && dynarr_get_byte(arr, 4) == 'o',
           "extend copied bytes matches");

    dynarr_resize(arr, 10);
    ensure(arr->size == 10, "resize enlarged size to 10");
    ensure(arr->capacity >= 10, "capacity grew to >= 10");

    dynarr_resize(arr, 4);
    ensure(arr->size == 4, "resize shrink to 4");

    printf("[OK] extend / resize passed\n\n");
}

/* === Test: reallocation move & data preservation === */
void test_realloc_move_and_preserve(arena_t *arena)
{
    printf("=== Running test_realloc_move_and_preserve ===\n");

    dynarr_t *arr = dynarr_init(arena, 2, 1);
    dynarr_push_byte(arr, 'x');
    dynarr_push_byte(arr, 'y');

    void *old_ptr = arr->elements;

    /* trigger growth several times */
    for (int i = 0; i < 100; ++i)
        dynarr_push_byte(arr, ('a' + (i % 26)));

    /* data still valid */
    ensure(dynarr_get_byte(arr, 0) == 'x', "data before reallocation intact");
    ensure(dynarr_get_byte(arr, 1) == 'y', "data before reallocation intact");

    /* ensure capacity >= size */
    ensure(arr->capacity >= arr->size, "capacity >= size after growth");

    if (arr->elements != old_ptr)
        printf(
            "[Info] internal buffer moved after reallocation, as expected.\n");

    printf("[OK] realloc move & data preservation passed\n");
}

int main(void)
{
    /* 1 MiB arena for tests */
    arena_t *arena = arena_init(1 << 20);

    test_init_and_properties(arena);
    test_push_and_get_byte(arena);
    test_push_and_get_word(arena);
    test_push_raw_and_set_raw(arena);
    test_extend_and_resize(arena);
    test_realloc_move_and_preserve(arena);

    printf("\nAll dynamic array tests passed!\n");
    arena_free(arena);
    return 0;
}
