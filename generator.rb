# frozen_string_literal: true

require 'active_support'
require 'active_support/core_ext/object/blank'
require 'active_support/inflector'
require 'ffi_gen'

class Generator
  attr_reader :ctx, :nodes

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

  def initialize(module_name)
    @module_name = module_name
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
    member_names = to_array_of_string(member_names, num_members)
    member_values = member_values.read_array_of_long(num_members)

    @nodes << EnumDeclNode.new(
      @ctx,
      untypedef_name(name),
      member_names.zip(member_values)
    )
  end

  def define_struct(name, member_types, member_names, num_members, _data)
    member_names = to_array_of_string(member_names, num_members)
    member_types = resolve_type_array(member_types, num_members)

    @nodes << StructDeclNode.new(
      @ctx,
      untypedef_name(name),
      member_names.zip(member_types).map { |n,t| RecordMember.new(n, t) }
    )
  end

  def define_union(name, member_types, member_names, num_members, _data)
    member_names = to_array_of_string(member_names, num_members)
    member_types = resolve_type_array(member_types, num_members)

    @nodes << UnionDeclNode.new(
      @ctx,
      untypedef_name(name),
      member_names.zip(member_types).map { |n,t| RecordMember.new(n, t) }
    )
  end

  def define_function(name, return_type, parameters, num_params, _data)
    @nodes << FunctionDeclNode.new(
      @ctx,
      name,
      resolve_type_ref(return_type),
      resolve_type_array(parameters, num_params)
    )
  end

  def define_variable(name, type, _data)
    @nodes << VariableDeclNode.new(
      @ctx,
      name,
      resolve_type_ref(type)
    )
  end

  def declare_forward(name, type, _data)
    if type == :UNION
      @nodes << UnionForwardDeclNode.new(@ctx, untypedef_name(name))
    else
      @nodes << StructForwardDeclNode.new(@ctx, untypedef_name(name))
    end
  end

  def parsed
    @parsed ||= begin
      @ctx.emit "module #{@module_name}"
      @ctx.emit "  extend FFI::Library"
      @nodes.each do |node|
        @ctx.emit node.to_ffi
      end
      @ctx.emit "end"

      @ctx.output
    end
  end

  private

  def resolve_type_array(types, num_types)
    types = to_array_of(FFIGen::FFITypeRef, types, num_types)
    types.map(&method(:resolve_type_ref))
  end

  def resolve_record_members(members, num_members)
    members = to_array_of(FFIGen::FFIRecordMember, members, num_members)
    members.map { |m| RecordMember.new(m[:name], resolve_type_ref(m[:type])) }
  end

  # To make FFI arrays easier to work with
  def to_array_of(type, base_ptr, num)
    base_ptr = base_ptr.to_ptr

    (0...num).map { |i| type.new(base_ptr + i*type.size) }
  end
  
  def to_array_of_string(base_ptr, num)
    base_ptr.read_array_of_pointer(num).map(&:read_string)
  end

  def untypedef_name(name)
    name.sub(/\A(enum|struct|union) /, '')
  end

  def pointer_to_function?(type)
    type[:type] == :pointer_ref && type[:kind][:point_type][:pointed_type][:type] == :function_ref
  end

  def resolve_type_ref(type)
    case type[:type]
    when :enum_ref
      EnumTypeNode.new(
        @ctx,
        untypedef_name(type[:qual_name]),
        type[:kind][:enum_type][:name]
      )
    when :struct_ref
      struct_type = type[:kind][:struct_type]
      member_types = struct_type[:members]

      StructTypeNode.new(
        @ctx,
        untypedef_name(type[:qual_name]),
        untypedef_name(struct_type[:name]),
        resolve_record_members(struct_type[:members], struct_type[:num_members])
      )
    when :union_ref
      union_type = type[:kind][:union_type]

      UnionTypeNode.new(
        @ctx,
        untypedef_name(type[:qual_name]),
        untypedef_name(union_type[:name]),
        resolve_record_members(union_type[:members], union_type[:num_members])
      )
    when :function_ref
      func_type = type[:kind][:func_type]

      FunctionTypeNode.new(
        @ctx,
        type[:qual_name],
        resolve_type_ref(func_type[:return_type]),
        resolve_type_array(func_type[:param_types], func_type[:num_params])
      )
    when :array_ref
      ArrayTypeNode.new(
        @ctx,
        resolve_type_ref(type[:kind][:array_type][:type]),
        type[:kind][:array_type][:size]
      )
    when :integer_ref
      BuiltinTypeNode.new(@ctx, type[:qual_name], type[:kind][:int_type][:type])
    when :float_ref
      BuiltinTypeNode.new(@ctx, type[:qual_name], type[:kind][:float_type][:type])
    when :void_ref
      BuiltinTypeNode.new(@ctx, 'void', :void)
    when :pointer_ref
      # Some annoying indirection (there is no "function type", it is always
      # a pointer in FFI context
      if pointer_to_function?(type)
        node = resolve_type_ref(type[:kind][:point_type][:pointed_type])
        node.qual_name = type[:qual_name]
        node
      else
        pointed_type = type[:kind][:point_type][:pointed_type]

        PointerTypeNode.new(
          @ctx,
          type[:qual_name],
          untypedef_name(pointed_type[:qual_name]),
          resolve_type_ref(pointed_type)
        )
      end
    else
      UnknownTypePoisonNode.new(@ctx, type[:qual_name])
    end
  end

  class OutputContext
    attr_reader :output

    def initialize
      @known_types = {}
      @known_symbols = {}
      @known_declarations = {}
      @known_macros = {}
      @output = []
    end

    def known_type?(name)
      @known_types.key?(name.to_s)
    end

    def declare_type(name)
      @known_types[name.to_s] = true
    end

    def known_symbol?(name)
      @known_symbols.key?(name.to_s)
    end

    def declare_symbol(name)
      @known_symbols[name.to_s] = true
    end

    def known_declaration?(name)
      @known_declarations.key?(name.to_s)
    end

    def declare_forward(name)
      @known_declarations[name.to_s] = true
    end

    def macro_defined?(name)
      @known_macros.key?(name)
    end

    def define_macro(name)
      @known_macros[name.to_s] = true
    end

    def emit(ruby)
      @output << ruby
    end

    def generate_name(name)
      @count ||= 0
      "#{name}#{@count += 1}"
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
    def class_name(name)
      ActiveSupport::Inflector.classify(name)
    end
  end

  class MacroNode < Node
    def initialize(ctx, name, definition)
      @ctx = ctx
      @name = name.upcase
      @definition = definition
    end

    def to_ffi
      if @ctx.macro_defined?(@name)
        ''
      else
        @ctx.define_macro(@name)
        "#{@name} = 1 # placeholder"
      end
    end
  end

  class EnumDeclNode < Node
    def initialize(ctx, name, members)
      @ctx = ctx
      @name = name
      @members = members
    end

    def to_ffi
      @ctx.declare_type(@name)

      members = @members.map { |n,v| ":#{n}, #{v}" }.join(", ")

      <<~RUBY
        enum :#{@name}, [#{members}]
      RUBY
    end
  end

  class StructDeclNode < Node
    def initialize(ctx, name, members)
      @ctx = ctx
      @name = class_name(name)
      @members = members
    end

    def to_ffi
      @ctx.declare_type(@name)

      member_names = @members.map { |m| m.name.presence || anonymous_name }
      member_types = @members.map { |m| m.child.to_param }

      member_string = member_names.zip(member_types).map { |n,t| ":#{n}, #{t}" }.join(", ")

      <<~RUBY
        class #{@name} < FFI::Struct
          #{"layout #{member_string}" if member_string.present?}
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
      @name = class_name(name)
      @members = members
    end

    def to_ffi
      @ctx.declare_type(@name)

      member_names = @members.map { |m| m.name.presence || anonymous_name }
      member_types = @members.map { |m| m.child.to_param }

      member_string = member_names.zip(member_types).map { |n,t| ":#{n}, #{t}" }.join(", ")

      <<~RUBY
        class #{@name} < FFI::Union
          #{"layout #{member_string}" if member_string.present?}
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
      return_type = @return_type.to_param
      param_types = @parameters.map(&:to_param).join(", ")

      <<~RUBY
        attach_function :#{@name}, [#{param_types}], #{return_type}
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
      @ctx.declare_symbol(@name)

      return callback_to_ffi if @type.is_a?(FunctionTypeNode)

      old_name = @type.to_param
      new_name = ":#{@name}"

      # Don't emit anything if the names are identical
      if old_name == new_name || old_name == ":#{class_name(@name)}"
        ''
      else
        <<~RUBY
          typedef #{old_name}, #{new_name}
        RUBY
      end
    end

    def callback_to_ffi
      return_type = @type.return_type.to_param
      param_types = @type.param_types.map(&:to_param).join(", ")

      <<-RUBY
        callback :#{@name}, [#{param_types}], #{return_type}
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
      <<~RUBY
        attach_variable :#{@name}, #{@type.to_param}
      RUBY
    end
  end

  class UnionForwardDeclNode < Node
    def initialize(ctx, name)
      @ctx = ctx
      @name = class_name(name)
    end

    def to_ffi
      @ctx.declare_forward(@name)

      <<-RUBY
        class #{@name} < FFI::Union
        end
      RUBY
    end
  end

  class StructForwardDeclNode < Node
    def initialize(ctx, name)
      @ctx = ctx
      @name = class_name(name)
    end

    def to_ffi
      @ctx.declare_forward(@name)

      <<~RUBY
        class #{@name} < FFI::Struct
        end
      RUBY
    end
  end

  class EnumTypeNode < Node
    def initialize(ctx, qual_name, name)
      @ctx = ctx
      @qual_name = qual_name
      @name = name
    end

    def to_param
      return ":#{@qual_name}" if @ctx.known_symbol?(@qual_name)

      ':int64'
    end
  end

  class StructTypeNode < Node
    def initialize(ctx, qual_name, name, members)
      @ctx = ctx
      @qual_name = class_name(qual_name)
      @name = class_name(name)
      @members = members
    end

    def to_param
      return "#{@qual_name}.by_value" if @ctx.known_type?(@qual_name)
      return ":#{@qual_name}" if @ctx.known_symbol?(@qual_name)

      @name = @ctx.generate_name("UnnamedStruct") if @name.blank?

      emit_definition unless @ctx.known_type?(@name)

      "#{@name}.by_value"
    end

    def emit_definition
      member_names = @members.map { |m| m.name.presence || anonymous_name }
      member_types = @members.map { |m| m.child.to_param }

      member_string = member_names.zip(member_types).map { |n,t| ":#{n}, #{t}" }.join(", ")

      @ctx.emit <<~RUBY
        class #{@name} < FFI::Struct
          #{"layout #{member_string}" if member_string.present?}
        end
      RUBY
    end

    def anonymous_name
      @count ||= 0
      "field#{@count += 1}"
    end
  end

  class UnionTypeNode < Node
    def initialize(ctx, qual_name, name, members)
      @ctx = ctx
      @qual_name = class_name(qual_name)
      @name = class_name(name)
      @members = members
    end

    def to_param
      return "#{@qual_name}.by_value" if @ctx.known_type?(@qual_name)
      return ":#{@qual_name}" if @ctx.known_symbol?(@qual_name)

      @name = @ctx.generate_name("UnnamedUnion") if @name.blank?

      emit_definition unless @ctx.known?(@name)

      "#{@name}.by_value"
    end

    def emit_definition
      member_names = @members.map { |m| m.name.presence || anonymous_name }
      member_types = @members.map { |m| m.child.to_param }

      member_string = member_names.zip(member_types).map { |n,t| ":#{n}, #{t}" }.join(", ")

      @ctx.emit <<~RUBY
        class #{@name} < FFI::Union
          #{"layout #{member_string}" if member_string.present?}
        end
      RUBY
    end

    def anonymous_name
      @count ||= 0
      "field#{@count += 1}"
    end
  end

  class FunctionTypeNode < Node
    attr_accessor :qual_name, :return_type, :param_types

    def initialize(ctx, qual_name, return_type, param_types)
      @ctx = ctx
      @qual_name = qual_name
      @return_type = return_type
      @param_types = param_types
    end

    def to_param
      return ":#{@qual_name}" if @ctx.known_symbol?(@qual_name)

      return_type = @return_type.to_param
      param_types = @param_types.map(&:to_param).join(", ")

      "callback([#{param_types}], #{return_type})"
    end
  end

  class ArrayTypeNode < Node
    def initialize(ctx, underlying_type, size)
      @ctx = ctx
      @underlying = underlying_type
      @size = size
    end

    def to_param
      "[#{@underlying.to_param}, #{@size}]"
    end
  end

  class BuiltinTypeNode < Node
    def initialize(ctx, qual_name, type)
      @ctx = ctx
      @qual_name = qual_name
      @type = type
    end

    def to_param
      return ":#{@qual_name}" if @ctx.known_symbol?(@qual_name)

      ":#{@type}"
    end
  end

  class PointerTypeNode < Node
    def initialize(ctx, qual_name, qual_name_underlying, underlying_type)
      @ctx = ctx
      @qual_name = qual_name
      @qual_name_u = class_name(qual_name_underlying)
      @underlying = underlying_type
    end

    def to_param
      return ":#{@qual_name}" if @ctx.known_symbol?(@qual_name)
      return ':string' if @qual_name_u == 'char'
      return "#{@qual_name_u}.by_ref" if @ctx.known_type?(@qual_name_u) || @ctx.known_declaration?(@qual_name_u)

      ":pointer"
    end
  end

  class UnknownTypePoisonNode < Node
    def initialize(ctx, qual_name)
      @ctx = ctx
      @qual_name = qual_name
    end

    def to_param
      fail ArgumentError, "Tried to use type `#{@qual_name}' by value, but missing the definition for that type!"
    end
  end
end
