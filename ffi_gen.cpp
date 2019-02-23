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

#include "ffi_gen.h"

using namespace clang;
using namespace clang::driver;
using namespace clang::tooling;
using namespace llvm;

class GetMacros : public PPCallbacks
{
public:
    Preprocessor &pp;
    std::map<std::string, std::vector<Token> > macros;

    GetMacros(Preprocessor &p) : pp(p) {}

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

class MacroParseAction : public clang::PreprocessOnlyAction
{
public:
    MacroParseAction(callbacks &cb) : cb(cb) {}

    virtual void ExecuteAction()
    {
        Preprocessor &p = getCompilerInstance().getPreprocessor();

        std::unique_ptr<GetMacros> get_macros { new GetMacros { p } };
        p.addPPCallbacks(std::move(get_macros));

        clang::PreprocessOnlyAction::ExecuteAction();

        GetMacros *m = static_cast<GetMacros *>(p.getPPCallbacks());

        for (auto &m : m->macros) {
            try {
                std::vector<Token> tokens = fixMacrosRecursive(p, m.second);
                std::string tokenPaste;

                for (auto &t : tokens)
                    tokenPaste.append(p.getSpelling(t));

                cb.mc(m.first.c_str(), tokenPaste.c_str(), cb.user_data);
            } catch (std::invalid_argument &ex) {
                // do nothing
            }
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

            childTokens = fixMacrosRecursive(p, childTokens);

            for (auto &k : childTokens)
                ret.push_back(k);
        }

        return ret;
    }

    callbacks &cb;
};

class FFIGenVisitor : public RecursiveASTVisitor<FFIGenVisitor>
{
public:
    explicit FFIGenVisitor(ASTContext *Context, callbacks &cb) : Context(Context), cb(cb) {}

    virtual bool VisitFunctionDecl(FunctionDecl *func)
    {
        // Don't grab functions that aren't in the main file
        clang::SourceManager &sm { Context->getSourceManager() };
        if (!sm.isInMainFile(sm.getExpansionLoc(func->getLocStart())))
            return true;

        std::string funcName = func->getNameInfo().getName().getAsString();
        std::string qualReturn = func->getReturnType().getAsString();
        std::vector<std::string> paramTypeStrings;
        std::vector<const char *> paramTypes;

        for (auto v : func->parameters()) {
            std::string paramName = v->getOriginalType().getAsString();

            paramTypeStrings.push_back(paramName);
            paramTypes.push_back(paramName.c_str());
        }

        cb.fc(funcName.c_str(), qualReturn.c_str(), &paramTypes[0], paramTypes.size(), cb.user_data);

        return true;
    }

    virtual bool VisitEnumDecl(EnumDecl *ed)
    {
        ed = ed->getCanonicalDecl();

        // Don't grab enums that aren't in the main file
        clang::SourceManager &sm { Context->getSourceManager() };
        if (!sm.isInMainFile(sm.getExpansionLoc(ed->getLocStart())))
            return true;

        // Don't try to do binding for non-exported enums
        if (!ed->hasNameForLinkage())
            return true;


        std::string name = ed->getNameAsString();
        if (name.size() == 0)
            name = ed->getTypedefNameForAnonDecl()->getUnderlyingType().getAsString();

        std::vector<std::string> memberNameStrings;
        std::vector<const char *> memberNames;
        std::vector<int64_t> memberValues;

        for (auto d : ed->enumerators()) {
            std::string memberName = d->getNameAsString();

            memberNameStrings.push_back(memberName);
            memberNames.push_back(memberName.c_str());
            memberValues.push_back(d->getInitVal().getExtValue());
        }

        cb.ec(name.c_str(), &memberNames[0], &memberValues[0], memberValues.size(), cb.user_data);

        return true;
    }

    virtual bool VisitTypedefDecl(TypedefDecl *td)
    {
        // Don't grab typedefs that aren't in the main file
        clang::SourceManager &sm { Context->getSourceManager() };
        if (!sm.isInMainFile(sm.getExpansionLoc(td->getLocStart())))
            return true;

        std::string aliasName = td->getNameAsString();
        std::string origName = td->getUnderlyingType().getAsString();

        cb.tc(origName.c_str(), aliasName.c_str(), cb.user_data);

        return true;
    }

    virtual bool VisitRecordDecl(RecordDecl *rd)
    {
        rd = rd->getDefinition();

        // Don't grab structs that aren't defined or not in the main file
        clang::SourceManager &sm { Context->getSourceManager() };
        if (!rd || !sm.isInMainFile(sm.getExpansionLoc(rd->getLocStart())))
            return true;

        // Don't try to do binding for non-exported structs/unions
        if (!rd->hasNameForLinkage())
            return true;

        std::string name = rd->getNameAsString();
        if (name.size() == 0)
            name = rd->getTypedefNameForAnonDecl()->getUnderlyingType().getAsString();

        std::vector<std::string> memberNameStrings;
        std::vector<const char *> memberNames;
        std::vector<std::string> memberTypeStrings;
        std::vector<const char *> memberTypes;
        std::vector<size_t> memberWidths;

        for (auto f : rd->fields()) {
            std::string memberType = f->getType().getAsString();
            std::string memberName = f->getNameAsString();

            memberTypeStrings.push_back(memberType);
            memberTypes.push_back(memberType.c_str());
            memberNameStrings.push_back(memberName);
            memberNames.push_back(memberName.c_str());
        }

        if (rd->isUnion()) {
            cb.uc(name.c_str(), &memberNames[0], &memberTypes[0], memberTypes.size(), cb.user_data);
        } else {
            cb.sc(name.c_str(), &memberNames[0], &memberTypes[0], memberTypes.size(), cb.user_data);
        }

        return true;
    }

private:
    ASTContext *Context;
    callbacks &cb;
};


class FFIParseConsumer : public clang::ASTConsumer
{
public:
    explicit FFIParseConsumer(ASTContext *Context, callbacks &cb) : Visitor(Context, cb)
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
    FFIParseAction(callbacks &cb) : cb(cb) {}

    virtual std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(clang::CompilerInstance &Compiler, llvm::StringRef InFile)
    {
        return std::unique_ptr<clang::ASTConsumer> { new FFIParseConsumer { &Compiler.getASTContext(), cb } };
    }

private:
    callbacks &cb;
};

void walk_file(const char *filename, const char **clangArgs, int argc, callbacks *c)
{
    std::ifstream t { filename };
    std::string inFile { std::istreambuf_iterator<char>(t), std::istreambuf_iterator<char>() };
    std::vector<std::string> args;

    for (int i = 0; i < argc; ++i)
        args.push_back(std::string { clangArgs[i] });

    clang::tooling::runToolOnCodeWithArgs(new MacroParseAction { *c }, inFile, args, filename);
    clang::tooling::runToolOnCodeWithArgs(new FFIParseAction { *c }, inFile, args, filename);
}
