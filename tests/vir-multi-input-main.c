#include "../lib/c.h"

#ifdef VIR_MULTI_INPUT_LOCAL
#error macro state leaked between input files
#endif

extern int shared_root;
extern int vir_multi_input_enum_root_init;

enum { vir_multi_input_shared_enum = 23 };
int vir_multi_input_root_enum_only;
int vir_multi_input_enum_init = vir_multi_input_shared_enum;

struct vir_multi_input_tag;
int vir_multi_input_main_probe(struct vir_multi_input_tag *value);
struct vir_multi_input_tag *main_tag_pointer;

struct vir_multi_input_tag {
    char main_member;
    int other_member;
};

int vir_multi_input_tag_root(void);

int vir_multi_input_main_probe(struct vir_multi_input_tag *value)
{
    return value ? value->other_member : 0;
}

int main(void)
{
    shared_root = 41;
    struct vir_multi_input_tag value;

    value.main_member = 9;
    value.other_member = 23;
    vir_multi_input_root_enum_only = 41;
    return shared_root == 41 && value.main_member == 9 &&
                   value.other_member == 23 &&
                   vir_multi_input_tag_root() == 17 &&
                   vir_multi_input_main_probe(0) == 0 &&
                   vir_multi_input_shared_enum == 23 &&
                   vir_multi_input_enum_init == 23 &&
                   vir_multi_input_enum_root_init == 13 &&
                   vir_multi_input_enum_root() == 30 &&
                   vir_multi_input_root_enum_only == 41
               ? 0
               : 1;
}
