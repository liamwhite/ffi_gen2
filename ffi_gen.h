#ifndef _INCLUDE_TOOL_H_
#define _INCLUDE_TOOL_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*macro_callback)(const char *name, const char *definition, void *data);
typedef void (*typedef_callback)(const char *name_from, const char *name_to, void *data);
typedef void (*function_callback)(const char *name, const char *type, const char **arg_spellings, size_t num_args, void *data);
typedef void (*enum_callback)(const char *name, const char **member_spellings, int64_t *member_values, size_t num_members, void *data);
typedef void (*struct_callback)(const char *name, const char **member_types, const char **member_names, size_t num_members, void *data);
typedef void (*union_callback)(const char *name, const char **member_types, const char **member_names, size_t num_members, void *data);

typedef struct {
    macro_callback mc;
    typedef_callback tc;
    function_callback fc;
    enum_callback ec;
    struct_callback sc;
    union_callback uc;
    void *user_data;
} callbacks;

void walk_file(const char *filename, const char **clangArgs, int argc, callbacks *c);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // _INCLUDE_TOOL_H_
