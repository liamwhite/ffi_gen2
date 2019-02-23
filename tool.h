#ifndef _INCLUDE_TOOL_H_
#define _INCLUDE_TOOL_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct FFIMacroInfo {
    char *macro_name;

    enum {
        STRING_MACRO,
        CHAR_MACRO,
        UINT_MACRO,
        SINT_MACRO
    } type;

    union {
        char    *str_value;
        char     chr_value;
        uint64_t uint64_value;
        int64_t  sint64_value;
    };
};

struct FFIType;

struct FFIPrimitiveType {
    char *name;  ///< Name of this type
    int width;   ///< Bit width, or 0 if void
    int pointer; ///< Is this type a pointer?
};

struct FFIFunctionType {
    char *name;              ///< Optional name of this type
    FFIType *return_type;    ///< Function return type
    FFIType *argument_types; ///< Types of the arguments
    size_t num_args;
};

struct FFIEnumMember {
    char *name;
    int64_t value;
};

struct FFIEnum {
    char *name; ///< Optional name
    struct FFIEnumMember *members;
    size_t num_members;
};

struct FFIStructMember {
    char *name;    ///< Name of the struct member
    FFIType *type; ///< Type of this struct member
    int bit_width; ///< Bit width, if specified, otherwise 0
    int bitfield;  ///< Is this member a bitfield?
};

struct FFIStruct {
    char *name;
    FFIStructMember *members;
    size_t num_members;
    int vla; ///< Whether the last member is a flexible array member
};

struct FFIUnion {
    char *name;
    FFIStructMember *members;
    size_t num_members;
};

struct FFIType {
    enum {
        ENUM_TYPE,
        STRUCT_TYPE,
        UNION_TYPE,
        FUNCTION_TYPE,
        PRIMITIVE_TYPE
    } type;

    union {
        FFIEnum enum_type;
        FFIStruct struct_type;
        FFIUnion union_type;
        FFIFunctionType func_type;
        FFIPrimitiveType prim_type;
    };
};

struct FFITypedef {
    struct FFIType from;
    struct FFIType to;
};

struct FFIFunctionDeclaration {
    char *name;
    struct FFIFunctionType type;
};

struct FFIFileInfo {
    struct FFIMacroInfo *object_macros;
    size_t num_object_macros;

    struct FFIType *types;
    size_t num_types;

    struct FFIFunctionDeclaration *functions;
    size_t num_functions;
};

struct FFIFileInfo *get_file_info(const char *filename);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // _INCLUDE_TOOL_H_
