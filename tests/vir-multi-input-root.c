#include "../lib/c.h"

#define VIR_MULTI_INPUT_LOCAL 1

int shared_root;

enum {
    vir_multi_input_shared_enum = 13,
    vir_multi_input_root_enum_only = 17,
};
int vir_multi_input_enum_root_init = vir_multi_input_shared_enum;

int vir_multi_input_enum_root(void)
{
    return vir_multi_input_shared_enum + vir_multi_input_root_enum_only;
}

struct vir_multi_input_tag;
int vir_multi_input_root_probe(struct vir_multi_input_tag *value);
struct vir_multi_input_tag *root_tag_pointer;

struct vir_multi_input_tag {
    int root_member;
};

int vir_multi_input_tag_root(void)
{
    struct vir_multi_input_tag value;

    value.root_member = 17;
    return value.root_member;
}

int vir_multi_input_root_probe(struct vir_multi_input_tag *value)
{
    return value ? value->root_member : 0;
}
