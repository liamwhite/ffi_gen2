#ifndef _INCLUDE_TOOL_H_
#define _INCLUDE_TOOL_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct FFITypeRef;

enum FFIIntegerType {
    Bool,
    UInt8,
    Int8,
    UInt16,
    Int16,
    UInt32,
    Int32,
    UInt64,
    Int64,
    Int128
};

enum FFIFloatType {
    Half,
    Float,
    Double,
    LongDouble
};

enum FFIRefType {
    ENUM_REF,
    STRUCT_REF,
    UNION_REF,
    FUNCTION_REF,
    INTEGER_REF,
    FLOAT_REF,
    POINTER_REF,
    ARRAY_REF,
    FLEX_REF,
    VOID_REF
};

struct FFIIntegerRef {
    enum FFIIntegerType type;
};

struct FFIFloatRef {
    enum FFIFloatType type;
};

struct FFIFunctionRef {
    struct FFITypeRef *return_type; ///< Function return type
    struct FFITypeRef *param_types; ///< Types of the formal parameters
    size_t num_params;
};

struct FFIFlexRef {
    struct FFITypeRef *type; ///< Type of the flexible array
};

struct FFIArrayRef {
    struct FFITypeRef *type;
    size_t size;
};

struct FFIEnumMember {
    char *name;
    int64_t value;
};

struct FFIEnumRef {
    char *name;
    int anonymous;
};

struct FFIRecordMember {
    char *name; ///< Will be null if anonymous struct/union
    struct FFITypeRef *type;
};

struct FFIStructRef {
    char *name;
    struct FFIRecordMember *members;
    size_t num_members;
    int anonymous;
};

struct FFIUnionRef {
    char *name;
    struct FFIRecordMember *members;
    size_t num_members;
    int anonymous;
};

struct FFIPointerRef {
    struct FFITypeRef *pointed_type; ///< Pointed-to type
};

struct FFITypeRef {
    enum FFIRefType type;
    char *qual_name; ///< Qualified (typedef) name

    union {
        struct FFIEnumRef enum_type;
        struct FFIStructRef struct_type;
        struct FFIUnionRef union_type;
        struct FFIFunctionRef func_type;
        struct FFIIntegerRef int_type;
        struct FFIFloatRef float_type;
        struct FFIArrayRef array_type;
        struct FFIFlexRef flex_type;
        struct FFIPointerRef point_type;
    };
};

typedef void (*macro_callback)(const char *name, const char *definition, void *data);
typedef void (*typedef_callback)(const char *name, struct FFITypeRef *to, void *data);
typedef void (*function_callback)(const char *name, struct FFITypeRef *return_type, struct FFITypeRef *param_types, size_t num_params, void *data);
typedef void (*enum_callback)(const char *name, const char **member_names, int64_t *member_values, size_t num_members, void *data);
typedef void (*struct_callback)(const char *name, struct FFITypeRef *member_types, const char **member_names, size_t num_members, void *data);
typedef void (*union_callback)(const char *name, struct FFITypeRef *member_types, const char **member_names, size_t num_members, void *data);
typedef void (*variable_callback)(const char *name, struct FFITypeRef *type, void *data);

typedef struct {
    macro_callback mc;
    typedef_callback tc;
    function_callback fc;
    enum_callback ec;
    struct_callback sc;
    union_callback uc;
    variable_callback vc;

    void *user_data;
} callbacks;

void walk_file(const char *filename, const char **clangArgs, int argc, callbacks *c);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // _INCLUDE_TOOL_H_
