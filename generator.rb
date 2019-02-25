require 'ffi_gen'

class Generator
  attr_reader :macros

  BUILTIN_TYPES = [
    :bool,
    :uint8,
    :int8,
    :uint16,
    :int16,
    :uint32,
    :int32,
    :int64,
    :int128,
    :half,
    :float,
    :double,
    :long_double,
    :pointer,
    :void
  ].freeze

  def initialize
    @known_types = BUILTIN_TYPES.dup.map{|x| [x, :builtin] }.to_h
    @output = []
  end

  def define_macro(name, definition, _data)
    @output << [:macro, name, try_evaluate(definition)]
  end

  def define_typedef(name, type, _data)
    @known_types[name.to_sym] = :typedef
    @output << [:typedef, name.to_sym, resolve_type_ref(type)]
  end

  def define_enum(name, member_names, member_values, num_members, _data)
    @known_types[name.to_sym] = :enum
    @output << [:enum, untypedef_name(name), enum_members(member_names, member_values, num_members)]
  end

  def define_struct(name, member_types, member_names, num_members, _data)
    @known_types[name.to_sym] = :struct
    @output << [:struct, untypedef_name(name), resolve_record_types(member_types, member_names, num_members)]
  end

  def define_union(name, member_types, member_names, num_members, _data)
    @known_types[name.to_sym] = :union
    @output << [:union, untypedef_name(name), resolve_record_types(member_types, member_names, num_members)]
  end

  def define_function(name, return_type, parameters, num_params, _data)
    @known_types[name.to_sym] = :function
    @output << [:function, name.to_sym, resolve_function_params(parameters, num_params), resolve_type_ref(return_type)]
  end

  def define_variable(name, type, _data)
    @output << [:variable, name.to_sym, resolve_type(type)]
  end

  private

  # don't even bother for now
  def try_evaluate(definition)
    definition
  end

  def enum_members(member_names, member_values, num_members)
    member_names = member_names.read_array_of_pointer(num_members).map { |x| x.read_string.to_sym }
    member_values = member_values.read_array_of_long(num_members)

    member_names.zip(member_values)
  end

  def resolve_function_params(parameters, num_params)
    read_array_of_typeref(parameters, num_params).map { |t| resolve_type_ref(t) }
  end

  def untypedef_name(name)
    name.sub(/\A(enum|struct|union) /, '').to_sym
  end

  def known_type?(name)
    @known_types.key?(name.to_sym)
  end

  def callback_definition?(type)
    func_ptr = type[:type] == :pointer_ref && type[:kind][:point_type][:pointed_type][:type] == :function_ref
    func = type[:type] == :function_ref

    func_ptr || func
  end

  def resolve_record_types(member_types, member_names, num_members)
    member_names = member_names.read_array_of_pointer(num_members).map { |x| x.read_string.to_sym }
    member_types = read_array_of_typeref(member_types, num_members).map { |t| resolve_type_ref(t) }

    member_names.zip(member_types)
  end

  def resolve_type_ref(type)
    case type[:type]
    when :enum_ref then type[:kind][:enum_type][:name].to_sym
    when :struct_ref then type[:kind][:struct_type][:name].to_sym
    when :union_ref then type[:kind][:union_type][:name].to_sym
    when :integer_ref then type[:kind][:int_type][:type]
    when :float_ref then type[:kind][:int_type][:type]
    when :void_ref then :void
    when :function_ref then resolve_func_ref(type)
    when :pointer_ref
      if type[:kind][:point_type][:pointed_type][:type] == :function_ref
        resolve_func_ref(type[:kind][:point_type][:pointed_type])
      else
        :pointer
      end
    end
  end

  def resolve_func_ref(type)
    func_ty = type[:kind][:func_type]
    return_ty = resolve_type_ref(func_ty[:return_type])
    param_tys = read_array_of_typeref(func_ty[:param_types], func_ty[:num_params]).map{|t| resolve_type_ref(t) }

    [:callback, param_tys, return_ty]
  end

  def read_array_of_typeref(base_ptr, num)
    base_ptr = base_ptr.to_ptr

    (0...num).map do |i|
      FFIGen::FFITypeRef.new(base_ptr + i*FFIGen::FFITypeRef.size)
    end
  end
end
