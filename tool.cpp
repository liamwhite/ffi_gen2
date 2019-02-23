#include "clang/Driver/Options.h"
#include "clang/AST/AST.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/ASTConsumer.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Frontend/ASTConsumers.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"
#include "clang/Rewrite/Core/Rewriter.h"

#include <iostream>
#include <fstream>
#include <streambuf>

using namespace clang;
using namespace clang::driver;
using namespace clang::tooling;
using namespace llvm;

class GetMacros : public PPCallbacks
{
public:
    Preprocessor &pp;
    std::map<std::string, std::vector<Token> > macros;

    GetMacros(Preprocessor &p) : pp(p)
    {}

    virtual void MacroDefined(const Token &MacroNameTok, const MacroDirective *MD)
	{
        const MacroInfo *i = MD->getMacroInfo();
        const IdentifierTable &table = pp.getIdentifierTable();

        // Ignore function-like macros and macros not in the
        // file we wanted to parse
	    if (i->isObjectLike() && pp.isInPrimaryFile()) {
	        std::vector<Token> tokens;

	        for (auto &t : i->tokens())
                tokens.push_back(Token { t });

            // Ignore macros with no tokens
            if (tokens.size() == 0)
                return;

    	    macros[pp.getSpelling(MacroNameTok)] = tokens;
	    }
	}

    virtual void MacroUndefined(const Token &MacroNameTok, const MacroDefinition &MD, const MacroDirective *Undef)
    {
        macros.erase(MacroNameTok.getName());
    }
};

class MacroParseConsumer : public clang::ASTConsumer
{
public:
    virtual void HandleTranslationUnit(clang::ASTContext &Context)
    {
    }
};

class MacroParseAction : public clang::PreprocessOnlyAction
{
public:
    virtual std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(clang::CompilerInstance &Compiler, llvm::StringRef InFile)
    {
        return std::unique_ptr<clang::ASTConsumer> { new MacroParseConsumer };
    }

    virtual void ExecuteAction()
    {
        Preprocessor &p = getCompilerInstance().getPreprocessor();

        std::unique_ptr<GetMacros> get_macros { new GetMacros { p } };
        p.addPPCallbacks(std::move(get_macros));

        clang::PreprocessOnlyAction::ExecuteAction();

        GetMacros *m = static_cast<GetMacros *>(p.getPPCallbacks());

        for (auto &m : m->macros) {
            std::cout << "Macro: " << m.first << ": ";
            std::vector<Token> tokens = fixMacrosRecursive(p, m.second);

            for (auto &t : tokens)
                std::cout << p.getSpelling(t) << " ";

            std::cout << "\n";
        }
    }

private:
    std::vector<Token> fixMacrosRecursive(Preprocessor &p, std::vector<Token> input)
    {
        std::vector<Token> ret;

        for (auto &t : input) {
            IdentifierInfo *i;
            MacroInfo *mi;

            if (t.getKind() != tok::identifier) {
                ret.push_back(t);
                continue;
            }

            i  = t.getIdentifierInfo();
            mi = p.getMacroInfo(i);

            if (!mi) {
                ret.push_back(t);
                continue;
            }

            if (!mi->isObjectLike()) {
                // need to propagate up to the root in order to not emit this
                // macro
                throw std::invalid_argument { "Macro child is not object-like" };
            }

            std::vector<Token> childTokens;

            for (auto &k : mi->tokens())
                childTokens.push_back(k);

            try {
                childTokens = fixMacrosRecursive(p, childTokens);

                for (auto &k : childTokens)
                    ret.push_back(k);
            } catch (std::invalid_argument &ex) {
                // do nothing
            }
        }

        return ret;
    }
};

class FFIGenVisitor : public RecursiveASTVisitor<FFIGenVisitor>
{
public:
    explicit FFIGenVisitor(ASTContext *Context) : Context(Context) {}

    virtual bool VisitFunctionDecl(FunctionDecl *func)
    {
        // Don't grab functions that aren't in the main file
        clang::SourceManager &sm { Context->getSourceManager() };
        if (!sm.isInMainFile(sm.getExpansionLoc(func->getLocStart())))
            return true;

        std::string funcName = func->getNameInfo().getName().getAsString();
        std::string qualReturn = func->getReturnType().getAsString();

        std::cout << "Function declaration: " << qualReturn << " " << funcName << " ( ";

        for (auto v : func->parameters())
            std::cout << v->getOriginalType().getAsString() << ", ";

        std::cout << "\b\b ) \n";

        return true;
    }

private:
    ASTContext *Context;
};


class FFIParseConsumer : public clang::ASTConsumer
{
public:
    explicit FFIParseConsumer(ASTContext *Context) : Visitor(Context)
    {}

    virtual void HandleTranslationUnit(clang::ASTContext &Context)
    {
        Visitor.TraverseDecl(Context.getTranslationUnitDecl());
    }

private:
    FFIGenVisitor Visitor;
};

class FFIParseAction : public clang::ASTFrontendAction {
public:
    virtual std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(clang::CompilerInstance &Compiler, llvm::StringRef InFile)
    {
        return std::unique_ptr<clang::ASTConsumer> { new FFIParseConsumer { &Compiler.getASTContext() } };
    }
};


int main(int argc, const char **argv)
{
    if (argc <= 1) {
        std::cerr << "Invoke with file name\n";
        return 1;
    }

    std::ifstream t { argv[1] };
    std::string inFile { std::istreambuf_iterator<char>(t), std::istreambuf_iterator<char>() };
    std::vector<std::string> args;

    for (int i = 2; i < argc; ++i)
        args.push_back(std::string { argv[i] });

    clang::tooling::runToolOnCodeWithArgs(new MacroParseAction, inFile, args, argv[1]);
    clang::tooling::runToolOnCodeWithArgs(new FFIParseAction, inFile, args, argv[1]);

    return 0;
}
