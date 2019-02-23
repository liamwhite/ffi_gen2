require 'ffi'

module FFIGen
  extend FFI::Library

  ffi_lib ["ffi_gen", "./libffi_gen.so"]

  # typedef void (*macro_callback)(const char *name, const char *definition, void *data);
  callback :macro_callback, [:string, :string, :pointer], :void

  # typedef void (*typedef_callback)(const char *name_from, const char *name_to, void *data);
  callback :typedef_callback, [:string, :string, :pointer], :void

  # typedef void (*function_callback)(const char *name, const char *type, const char **arg_spellings, size_t num_args, void *data);
  callback :function_callback, [:string, :string, :pointer, :size_t, :pointer], :void

  # typedef void (*enum_callback)(const char *name, const char **member_spellings, int64_t *member_values, size_t num_members, void *data);
  callback :enum_callback, [:string, :pointer, :pointer, :size_t, :pointer], :void

  # typedef void (*struct_callback)(const char *name, const char **member_types, const char **member_names, size_t num_members, void *data);
  callback :struct_callback, [:string, :pointer, :pointer, :size_t, :pointer], :void

  # typedef void (*union_callback)(const char *name, const char **member_types, const char **member_names, size_t num_members, void *data);
  callback :union_callback, [:string, :pointer, :pointer, :size_t, :pointer], :void

  class Callbacks < FFI::Struct
    layout :mc, :macro_callback, 8*0,
           :tc, :typedef_callback, 8*1,
           :fc, :function_callback, 8*2,
           :ec, :enum_callback, 8*3,
           :sc, :struct_callback, 8*4,
           :uc, :union_callback, 8*5
  end

  # void walk_file(const char *filename, const char **clangArgs, int argc, callbacks c);
  attach_function :walk_file, [:string, :pointer, :int, :pointer], :void

  def self.inspect_file(filename, args)
    argv = FFI::MemoryPointer.new(:pointer, args.count)
    args.map!{|a| FFI::MemoryPointer.from_string(a) }
    argv.write_array_of_pointer(args)

    mc = proc { |name| puts "Emitting macro definition for #{name}" }
    tc = proc { |name| puts "Emitting typedef definition for #{name}" }
    fc = proc { |name| puts "Emitting function declaration for #{name}" }
    ec = proc { |name| puts "Emitting enum definition for #{name}" }
    sc = proc { |name| puts "Emitting struct definition for #{name}" }
    uc = proc { |name| puts "Emitting union definition for #{name}" }

    callbacks = Callbacks.new
    callbacks[:mc] = mc
    callbacks[:tc] = tc
    callbacks[:fc] = fc
    callbacks[:ec] = ec
    callbacks[:sc] = sc
    callbacks[:uc] = uc

    walk_file(filename, argv, args.size, callbacks)
  end
end
