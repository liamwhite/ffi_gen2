#ifndef _INCLUDE_TOOL_H_
#define _INCLUDE_TOOL_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct FFITypeRef;

struct FFIVoidRef {
};

struct FFIIntegerRef {
    int width;   ///< Bit width
};

struct FFIFloatRef {
    int width; ///< Bit width
};

struct FFIFunctionRef {
    FFITypeRef *return_type; ///< Function return type
    FFITypeRef *param_types; ///< Types of the formal parameters
    size_t num_params;
};

struct FFIEnumRef {
    char *name;
};

struct FFIStructRef {
    char *name;
};

struct FFIUnionRef {
    char *name;
};

struct FFIPointerRef {
    FFITypeRef *pointed_type; ///< Pointed-to type
};

struct FFITypeRef {
    enum {
        ENUM_REF,
        STRUCT_REF,
        UNION_REF,
        FUNCTION_REF,
        INTEGER_REF,
        FLOAT_REF,
        POINTER_REF,
        VOID_REF
    } type;

    union {
        struct FFIEnumRef enum_type;
        struct FFIStructRef struct_type;
        struct FFIUnionRef union_type;
        struct FFIFunctionRef func_type;
        struct FFIIntegerRef int_type;
        struct FFIFloatRef float_type;
        struct FFIPointerRef point_type;
    };
};

typedef void (*macro_callback)(const char *name, const char *definition, void *data);
typedef void (*typedef_callback)(const char *name, FFITypeRef *to, void *data);
typedef void (*function_callback)(const char *name, FFITypeRef *return_type, FFITypeRef *param_types, size_t num_params, void *data);
typedef void (*enum_callback)(const char *name, const char **member_names, int64_t *member_values, size_t num_members, void *data);
typedef void (*struct_callback)(const char *name, FFITypeRef *member_types, const char **member_names, size_t num_members, void *data);
typedef void (*union_callback)(const char *name, FFITypeRef *member_types, const char **member_names, size_t num_members, void *data);

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
