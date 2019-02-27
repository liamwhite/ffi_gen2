#!/usr/bin/env ruby
$:.unshift(File.dirname(__FILE__))

require 'ffi_gen'
require 'generator'

# Invocation: ruby task.rb <modulename> <filename> [<clang args>]

default_arguments = "-I/usr/lib/llvm-7/lib/clang/7.0.1/include -I/usr/include/x86_64-linux-gnu -include stddef.h -include stdio.h".split(" ")
module_name, file_name, *args = ARGV

g = Generator.new(module_name)
default_arguments.concat args

FFIGen.inspect_file(file_name, default_arguments, g)
puts g.parsed.join("\n")
