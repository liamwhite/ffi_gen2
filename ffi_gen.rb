require 'ffi'

module FFIGen
  extend FFI::Library

  ffi_lib ["ffi_gen", "./libffi_gen.so"]

  enum :FFIIntegerType, [
    :bool,
    :uint8,
    :int8,
    :uint16,
    :int16,
    :uint32,
    :int32,
    :uint64,
    :int64,
    :int128
  ]

  enum :FFIFloatType, [
    :half,
    :float,
    :double,
    :long_double
  ]

  enum :FFIRefType, [
    :enum_ref,
    :struct_ref,
    :union_ref,
    :function_ref,
    :integer_ref,
    :float_ref,
    :pointer_ref,
    :array_ref,
    :flex_ref,
    :void_ref
  ]

  enum :FFIForwardType, [
    :STRUCT,
    :UNION
  ]

  class FFITypeRef < FFI::Struct
  end

  class FFIVoidRef < FFI::Struct
  end

  class FFIIntegerRef < FFI::Struct
    layout :type, :FFIIntegerType
  end

  class FFIFloatRef < FFI::Struct
    layout :type, :FFIFloatType
  end

  class FFIFunctionRef < FFI::Struct
    layout :return_type, FFITypeRef.by_ref,
           :param_types, FFITypeRef.by_ref,
           :num_params, :size_t
  end

  class FFIFlexRef < FFI::Struct
    layout :type, FFITypeRef.by_ref
  end

  class FFIArrayRef < FFI::Struct
    layout :type, FFITypeRef.by_ref,
           :size, :size_t
  end

  class FFIEnumRef < FFI::Struct
    layout :name, :string,
           :anonymous, :int
  end

  class FFIRecordMember < FFI::Struct
    layout :name, :string,
           :type, FFITypeRef.by_ref
  end

  class FFIStructRef < FFI::Struct
    layout :name, :string,
           :members, FFIRecordMember.by_ref,
           :num_members, :size_t,
           :anonymous, :int,
           :defined, :int
  end

  class FFIUnionRef < FFI::Struct
    layout :name, :string,
           :members, FFIRecordMember.by_ref,
           :num_members, :size_t,
           :anonymous, :int,
           :defined, :int
  end

  class FFIPointerRef < FFI::Struct
    layout :pointed_type, FFITypeRef.by_ref
  end

  class FFITypeUnion < FFI::Union
    layout :enum_type, FFIEnumRef.by_value,
           :struct_type, FFIStructRef.by_value,
           :union_type, FFIUnionRef.by_value,
           :func_type, FFIFunctionRef.by_value,
           :int_type, FFIIntegerRef.by_value,
           :float_type, FFIFloatRef.by_value,
           :array_type, FFIArrayRef.by_value,
           :flex_type, FFIFlexRef.by_value,
           :point_type, FFIPointerRef.by_value
  end

  # Redefinition to pick up the now-defined FFITypeUnion type
  class FFITypeRef
    layout :type, :FFIRefType,
           :qual_name, :string,
           :kind, FFITypeUnion.by_value
  end

  # typedef void (*macro_callback)(const char *name, const char *definition, void *data);
  callback :macro_callback, [:string, :string, :pointer], :void

  # typedef void (*typedef_callback)(const char *name, FFITypeRef *to, void *data);
  callback :typedef_callback, [:string, FFITypeRef.by_ref, :pointer], :void

  # typedef void (*function_callback)(const char *name, FFITypeRef *return_type, FFITypeRef *param_types, size_t num_params, void *data);
  callback :function_callback, [:string, FFITypeRef.by_ref, FFITypeRef.by_ref, :size_t, :pointer], :void

  # typedef void (*enum_callback)(const char *name, const char **member_names, int64_t *member_values, size_t num_members, void *data);
  callback :enum_callback, [:string, :pointer, :pointer, :size_t, :pointer], :void

  # typedef void (*struct_callback)(const char *name, struct FFITypeRef *member_types, const char **member_names, size_t num_members, int defined, void *data);
  callback :struct_callback, [:string, FFITypeRef.by_ref, :pointer, :size_t, :int, :pointer], :void

  # typedef void (*union_callback)(const char *name, FFITypeRef *member_types, const char **member_names, size_t num_members, int defined, void *data);
  callback :union_callback, [:string, FFITypeRef.by_ref, :pointer, :size_t, :int, :pointer], :void

  # typedef void (*variable_callback)(const char *name, struct FFITypeRef *type, void *data);
  callback :variable_callback, [:string, FFITypeRef.by_ref, :pointer], :void

  # typedef void (*forward_callback)(const char *name, enum FFIForwardType type, void *data);
  callback :forward_callback, [:string, :FFIForwardType, :pointer], :void

  class Callbacks < FFI::Struct
    layout :mc, :macro_callback,
           :tc, :typedef_callback,
           :fc, :function_callback,
           :ec, :enum_callback,
           :sc, :struct_callback,
           :uc, :union_callback,
           :vc, :variable_callback,
           :fdc, :forward_callback,
           :data, :pointer
  end

  # void walk_file(const char *filename, const char **clang_args, int argc, const char **source_locations, int nloc, callbacks *c);
  attach_function :walk_file, [:string, :pointer, :int, :pointer, :int, :pointer], :void

  def self.inspect_file(filename, source_filter, args, callback)
    argv = FFI::MemoryPointer.new(:pointer, args.count)
    args.map!{|a| FFI::MemoryPointer.from_string(a) }
    argv.write_array_of_pointer(args)

    sources = FFI::MemoryPointer.new(:pointer, source_filter.count)
    source_filter.map!{|f| FFI::MemoryPointer.from_string(f) }
    sources.write_array_of_pointer(source_filter)

    cb = Callbacks.new
    cb[:mc] = callback.method(:define_macro)
    cb[:tc] = callback.method(:define_typedef)
    cb[:fc] = callback.method(:define_function)
    cb[:ec] = callback.method(:define_enum)
    cb[:sc] = callback.method(:define_struct)
    cb[:uc] = callback.method(:define_union)
    cb[:fdc] = callback.method(:declare_forward)

    walk_file(filename, argv, args.size, sources, source_filter.size, cb)
  end
end
