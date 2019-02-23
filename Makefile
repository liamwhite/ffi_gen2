CXX      := clang++-7
CXXFLAGS := -I/usr/lib/llvm-7/include
LIBS     := $(LIBS) -lclangTooling -lclangFrontendTool -lclangFrontend \
	    -lclangDriver -lclangSerialization -lclangCodeGen -lclangParse \
	    -lclangSema -lclangStaticAnalyzerFrontend \
	    -lclangStaticAnalyzerCheckers -lclangStaticAnalyzerCore \
	    -lclangAnalysis -lclangARCMigrate -lclangRewrite \
	    -lclangRewriteFrontend -lclangEdit -lclangAST -lclangLex \
	    -lclangBasic -lclang -lLLVM-7
RM       ?= rm

.PHONY: all clean

all: tool

tool: tool.cpp tool.h
	$(CXX) tool.cpp -o tool $(CXXFLAGS) $(LIBS)

clean:
	$(RM) -fr tool
