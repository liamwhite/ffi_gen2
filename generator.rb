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
    @ctx = OutputContext.new
    @nodes = []
  end

  def define_macro(name, definition, _data)
    @nodes << MacroNode.new(@ctx, name, definition)
  end

  def define_typedef(name, type, _data)
    @nodes << TypedefDeclNode.new(@ctx, name, resolve_type_ref(type))
  end

  def define_enum(name, member_names, member_values, num_members, _data)
    @nodes << EnumDeclNode.new(
      @ctx,
      untypedef_name(name),
      enum_members(member_names, member_values, num_members)
    )
  end

  def define_struct(name, member_types, member_names, num_members, _data)
    @nodes << StructDeclNode.new(
      @ctx,
      untypedef_name(name),
      resolve_record_types(member_types, member_names, num_members)
    )
  end

  def define_union(name, member_types, member_names, num_members, _data)
    @nodes << UnionDeclNode.new(
      @ctx,
      untypedef_name(name),
      resolve_record_types(member_types, member_names, num_members)
    )
  end

  def define_function(name, return_type, parameters, num_params, _data)
    @nodes << FunctionDeclNode.new(
      @ctx,
      name,
      resolve_type_ref(return_type),
      resolve_function_params(parameters, num_params)
    )
  end

  def define_variable(name, type, _data)
    @nodes << VariableDeclNode.new(
      @ctx,
      name,
      resolve_type_ref(type)
    )
  end

  private

  # To make FFI arrays easier to work with
  def to_array_of(type, base_ptr, num)
    base_ptr = base_ptr.to_ptr

    (0...num).map { |i| type.new(base_ptr + i*type.size) }
  end
  
  def to_array_of_string(base_ptr, num)
    base_ptr.read_array_of_pointer(num).map(&:read_string)
  end

  # Don't even bother for now
  def try_evaluate(definition)
    definition
  end

  def enum_members(member_names, member_values, num_members)
    member_names = to_array_of_string(member_names, num_members)
    member_values = member_values.read_array_of_long(num_members)

    member_names.zip(member_values)
  end

  def resolve_function_params(parameters, num_params)
    read_array_of_type(FFIGen::FFITypeRef, parameters, num_params).map do |t|
      resolve_type_ref(t)
    end
  end

  def untypedef_name(name)
    name.sub(/\A(enum|struct|union) /, '').to_sym
  end

  def callback_definition?(type)
    func_ptr = type[:type] == :pointer_ref && type[:kind][:point_type][:pointed_type][:type] == :function_ref
    func = type[:type] == :function_ref

    func_ptr || func
  end

  def resolve_record_types(member_types, member_names, num_members)
    member_names = to_array_of_string(member_names, num_members)
    member_types = read_array_of_type(FFIGen::FFITypeRef, member_types, num_members).map do |t|
      resolve_type_ref(t)
    end

    member_names.zip(member_types)
  end

  def resolve_type_ref(type)
    if @known_types[type[:qual_name].to_sym]
      return type[:qual_name].to_sym
    end

    case type[:type]
    when :enum_ref then (type[:kind][:enum_type][:name] || :int64).to_sym
    when :struct_ref then resolve_struct_ref(type)
    when :union_ref then resolve_union_ref(type)
    when :integer_ref then type[:kind][:int_type][:type]
    when :float_ref then type[:kind][:int_type][:type]
    when :array_ref then [:array, resolve_type_ref(type[:kind][:array_type][:type]), type[:kind][:array_type][:size]]
    when :flex_ref then [:flex, resolve_type_ref(type[:kind][:array_type][:type])]
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

  def resolve_union_ref(type)
    union_type = type[:kind][:union_type]

    if union_type[:anonymous] != 0
      member_records = read_array_of_recordmember(union_type[:members], union_type[:num_members]).map(&:values)
      member_records.map! { |name, type| [name.to_sym, resolve_type_ref(type)] }

      [:anonymous_union, member_records]
    else
      union_type[:name].to_sym
    end
  end

  def resolve_struct_ref(type)
    struct_type = type[:kind][:struct_type]

    if struct_type[:anonymous] != 0
      member_records = read_array_of_recordmember(struct_type[:members], struct_type[:num_members]).map(&:values)
      member_records.map! { |name, type| [name.to_sym, resolve_type_ref(type)] }

      [:anonymous_struct, member_records]
    else
      struct_type[:name].to_sym
    end
  end

  def resolve_func_ref(type)
    func_ty = type[:kind][:func_type]
    return_ty = resolve_type_ref(func_ty[:return_type])
    param_tys = read_array_of_typeref(func_ty[:param_types], func_ty[:num_params]).map{|t| resolve_type_ref(t) }

    [:callback, param_tys, return_ty]
  end

  class OutputContext
    def initialize
      @known_types = {}
      @output = []
    end

    def known?(name)
      @known_types.key?(name)
    end

    def declare_type(name)
      @known_types[name] = true
    end

    def emit(ruby)
      @output << ruby
    end
  end

  class RecordMember
    attr_reader :name, :child

    def initialize(name, child)
      @name = name
      @child = child
    end
  end

  class Node
  end

  class MacroNode < Node
    def initialize(ctx, name, definition)
      @ctx = ctx
      @name = name
      @definition = definition
    end

    def to_ffi
      "#{@name.upcase} = 1 # placeholder"
    end
  end

  class EnumDeclNode < Node
    def initialize(ctx, name, members)
      @ctx = ctx
      @name = name
      @members = members
    end

    def to_ffi
      @ctx.declare_type(name)

      members = @members.map { |n,v| ":#{n}, #{v}" }.join(",")

      <<-RUBY
        enum :#{@name}, [#{members}]
      RUBY
    end
  end

  class StructDeclNode < Node
    def initialize(ctx, name, members)
      @ctx = ctx
      @name = name
      @members = members
    end

    def to_ffi
      @ctx.declare_type(name)

      member_names = @members.map { |m| m.name || anonymous_name }
      member_types = @members.map { |m| m.child.to_param }

      member_string = member_names.zip(member_types).map { |n,t| ":#{n}, #{t}" }.join(",")

      <<-RUBY
        class #{@name} < FFI::Struct
          layout #{member_string}
        end
      RUBY
    end

    def anonymous_name
      @count ||= 0
      "field#{@count += 1}"
    end
  end

  class UnionDeclNode < Node
    def initialize(ctx, name, members)
      @ctx = ctx
      @name = name
      @members = members
    end

    def to_ffi
      @ctx.declare_type(name)

      member_names = @members.map { |m| m.name || anonymous_name }
      member_types = @members.map { |m| m.child.to_param }

      member_string = member_names.zip(member_types).map { |n,t| ":#{n}, #{t}" }.join(",")

      <<-RUBY
        class #{@name} < FFI::Union
          layout #{member_string}
        end
      RUBY
    end

    def anonymous_name
      @count ||= 0
      "field#{@count += 1}"
    end
  end

  class FunctionDeclNode < Node
    def initialize(ctx, name, return_type, params)
      @ctx = ctx
      @name = name
      @return_type = return_type
      @parameters = params
    end

    def to_ffi
      <<-RUBY
        attach_function :#{@name}, [#{@parameters.map(&:to_param)}], #{@return_type.to_param}
      RUBY
    end
  end

  class TypedefDeclNode < Node
    def initialize(ctx, name, type)
      @ctx = ctx
      @name = name
      @type = type
    end

    def to_ffi
      @ctx.declare_type(name)

      <<-RUBY
        typedef #{@type.to_param}, :#{@name}
      RUBY
    end
  end

  class VariableDeclNode < Node
    def initialize(ctx, name, type)
      @ctx = ctx
      @name = name
      @type = type
    end

    def to_ffi
      <<-RUBY
        attach_variable :#{@name}, #{@type.to_param}
      RUBY
    end
  end

  class StructTypeNode < Node
    def initialize(ctx, name, members)
      @ctx = ctx
      @name = name
      @members = members
    end

    def to_param
      if @name.nil?
        @name = @ctx.generate_anonymous_name("UnnamedStruct")
      end

      emit_definition

      "#{@name}.by_value"
    end

    def emit_definition
      member_names = members.map { |m| m.name || anonymous_name }
      member_types = members.map { |m| m.child.to_param }

      member_string = member_names.zip(member_types).map { |n,t| ":#{n}, #{t}" }.join(",")

      @ctx.emit <<-RUBY
        class #{@name} < FFI::Struct
          layout #{member_string}
        end
      RUBY
    end

    def anonymous_name
      @count ||= 0
      "field#{@count += 1}"
    end
  end

  class UnionTypeNode < Node
    def initialize(ctx, name, members)
      @ctx = ctx
      @name = name
      @members = members
    end

    def to_param
      if @name.nil?
        @name = @ctx.generate_anonymous_name("UnnamedUnion")
      end

      emit_definition

      "#{@name}.by_value"
    end

    def emit_definition
      member_names = @members.map { |m| m.name || anonymous_name }
      member_types = @members.map { |m| m.child.to_param }

      member_string = member_names.zip(member_types).map { |n,t| ":#{n}, #{t}" }.join(",")

      @ctx.emit <<-RUBY
        class #{@name} < FFI::Union
          layout #{member_string}
        end
      RUBY
    end

    def anonymous_name
      @count ||= 0
      "field#{@count += 1}"
    end
  end

  class FunctionTypeNode < Node
    def initialize(ctx, return_type, param_types)
      @ctx = ctx
      @return_type = return_type
      @param_types = param_types
    end

    def to_param
      return_type = @return_type.to_param
      param_types = @param_types.map { |t| t.to_param }

      "callback([#{param_types.map(&:to_param).join(",")}], #{return_type})"
    end
  end

  class BuiltinTypeNode < Node
    def initialize(ctx, type)
      @ctx = ctx
      @type = type
    end

    def to_param
      ":#{@type}"
    end
  end

  class TypedefTypeNode < Node
    def initialize(ctx, name)
      @ctx = ctx
      @name = name
    end

    def to_param
      ":#{@type}"
    end
  end

  class UnknownTypePoisonNode < Node
    def initialize(name)
      @ctx = ctx
      @name = name
    end

    def to_param
      fail ArgumentError, "Tried to use type #{@name} by value, but missing the definition for that type!"
    end
  end
end
