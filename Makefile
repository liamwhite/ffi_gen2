CXX      := g++
CXXLD    := clang++-7
CXXFLAGS := -I/usr/lib/llvm-7/include -O0 -g3 -fPIC
LDFLAGS  := -L/usr/lib/llvm-7/lib
LIBS     := $(LIBS) -lclangTooling -lclangFrontendTool -lclangFrontend \
	    -lclangDriver -lclangSerialization -lclangCodeGen -lclangParse \
	    -lclangSema -lclangStaticAnalyzerFrontend \
	    -lclangStaticAnalyzerCheckers -lclangStaticAnalyzerCore \
	    -lclangAnalysis -lclangARCMigrate -lclangRewrite \
	    -lclangRewriteFrontend -lclangEdit -lclangAST -lclangLex \
	    -lclangBasic -lclang -lLLVM-7
RM       ?= rm

.PHONY: all clean

all: libffi_gen.so

libffi_gen.so: ffi_gen.cpp ffi_gen.h
	$(CXX) ffi_gen.cpp -shared -o libffi_gen.so $(CXXFLAGS) $(LDFLAGS) $(LIBS)

clean:
	$(RM) -fr libffi_gen.so
