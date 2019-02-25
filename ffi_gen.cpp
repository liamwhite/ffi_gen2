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

static FFITypeRef type_for_qual(QualType qt);
static void get_types_for_func(FunctionDecl *fd, FFITypeRef &returnTy, std::vector<FFITypeRef> &paramTys);

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

        FFITypeRef returnTy;
        std::string funcName = func->getNameInfo().getName().getAsString();
        std::vector<FFITypeRef> paramTys;

        get_types_for_func(func, returnTy, paramTys);

        cb.fc(funcName.c_str(), &returnTy, &paramTys[0], paramTys.size(), cb.user_data);

        return true;
    }

    virtual bool VisitVarDecl(VarDecl *vd)
    {
        // Don't grab variables that aren't in the main file
        clang::SourceManager &sm { Context->getSourceManager() };
        if (!sm.isInMainFile(sm.getExpansionLoc(vd->getLocStart())))
            return true;

        // Don't try to do binding for non-exported variables
        if (!vd->isExternC())
            return true;

        std::string name = vd->getNameAsString();
        FFITypeRef varTy = type_for_qual(vd->getType());

        cb.vc(name.c_str(), &varTy, cb.user_data);

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
            memberValues.push_back(d->getInitVal().getExtValue());
        }

        for (auto &s : memberNameStrings)
            memberNames.push_back(s.c_str());

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
        FFITypeRef type = type_for_qual(td->getUnderlyingType());

        cb.tc(aliasName.c_str(), &type, cb.user_data);

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
        std::vector<FFITypeRef> memberTypes;

        for (auto f : rd->fields()) {
            std::string memberName = f->getNameAsString();

            memberTypes.push_back(type_for_qual(f->getType()));
            memberNameStrings.push_back(memberName);
        }

        for (auto &s : memberNameStrings)
            memberNames.push_back(s.c_str());

        if (rd->isUnion()) {
            cb.uc(name.c_str(), &memberTypes[0], &memberNames[0], memberTypes.size(), cb.user_data);
        } else {
            cb.sc(name.c_str(), &memberTypes[0], &memberNames[0], memberTypes.size(), cb.user_data);
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

static FFITypeRef type_for_qual(QualType qt)
{
    FFITypeRef returnTy;

    if (qt->isVoidType()) {
        returnTy.type = FFIRefType::VOID_REF;
    } else if (qt->isPointerType()) {
        // LEAK
        FFITypeRef *pointee = new FFITypeRef { type_for_qual(qt->getPointeeType()) };

        returnTy.type = FFIRefType::POINTER_REF;
        returnTy.point_type.pointed_type = pointee;
    } else if (qt->isEnumeralType()) {
        const EnumDecl *ed = qt->castAs<EnumType>()->getDecl();

        std::string name = ed->getNameAsString();
        if (name.size() == 0)
            name = ed->getTypedefNameForAnonDecl()->getUnderlyingType().getAsString();

        returnTy.type = FFIRefType::ENUM_REF;
        returnTy.enum_type.name = strdup(name.c_str()); // LEAK
    } else if (qt->isRecordType()) {
        const RecordDecl *rd = qt->castAs<RecordType>()->getDecl();

        std::string name = rd->getNameAsString();
        if (name.size() == 0)
            name = rd->getTypedefNameForAnonDecl()->getUnderlyingType().getAsString();

        if (qt->isUnionType()) {
            returnTy.type = FFIRefType::UNION_REF;
            returnTy.union_type.name = strdup(name.c_str()); // LEAK
        } else {
            returnTy.type = FFIRefType::STRUCT_REF;
            returnTy.struct_type.name = strdup(name.c_str()); // LEAK
        }
    } else if (qt->isFunctionProtoType()) {
        const FunctionProtoType *ft = qt->castAs<FunctionProtoType>();

        FFITypeRef *ret_type = new FFITypeRef { type_for_qual(ft->getReturnType()) }; // LEAK
        std::vector<FFITypeRef> *param_types = new std::vector<FFITypeRef>; // LEAK
        for (size_t i = 0; i < ft->getNumParams(); ++i)
            param_types->push_back(FFITypeRef { type_for_qual(ft->getParamType(i)) } );

        returnTy.type = FFIRefType::FUNCTION_REF;
        returnTy.func_type.return_type = ret_type;
        returnTy.func_type.param_types = &((*param_types)[0]);
        returnTy.func_type.num_params = ft->getNumParams();
    } else if (qt->isFunctionNoProtoType()) {
        const FunctionNoProtoType *ft = qt->castAs<FunctionNoProtoType>();

        FFITypeRef *ret_type = new FFITypeRef { type_for_qual(ft->getReturnType()) }; // LEAK
        returnTy.type = FFIRefType::FUNCTION_REF;
        returnTy.func_type.return_type = ret_type;
        returnTy.func_type.param_types = nullptr;
        returnTy.func_type.num_params = 0;
    } else if (qt->isBuiltinType()) {
        const BuiltinType *bt = qt->castAs<BuiltinType>();

        switch (bt->getKind()) {
        case BuiltinType::Kind::Bool:
            returnTy.type = FFIRefType::INTEGER_REF;
            returnTy.int_type.type = FFIIntegerType::Bool;
            break;

        case BuiltinType::Kind::Char_U:
        case BuiltinType::Kind::UChar:
            returnTy.type = FFIRefType::INTEGER_REF;
            returnTy.int_type.type = FFIIntegerType::UInt8;
            break;

        case BuiltinType::Kind::Char8:
        case BuiltinType::Kind::Char_S:
        case BuiltinType::Kind::SChar:
            returnTy.type = FFIRefType::INTEGER_REF;
            returnTy.int_type.type = FFIIntegerType::Int8;
            break;

        case BuiltinType::Kind::WChar_U:
        case BuiltinType::Kind::UShort:
            returnTy.type = FFIRefType::INTEGER_REF;
            returnTy.int_type.type = FFIIntegerType::UInt16;
            break;

        case BuiltinType::Kind::WChar_S:
        case BuiltinType::Kind::Char16:
        case BuiltinType::Kind::Short:
            returnTy.type = FFIRefType::INTEGER_REF;
            returnTy.int_type.type = FFIIntegerType::Int16;
            break;

        case BuiltinType::Kind::Char32:
        case BuiltinType::Kind::Int:
            returnTy.type = FFIRefType::INTEGER_REF;
            returnTy.int_type.type = FFIIntegerType::Int32;
            break;

        case BuiltinType::Kind::UInt:
            returnTy.type = FFIRefType::INTEGER_REF;
            returnTy.int_type.type = FFIIntegerType::UInt32;
            break;

        case BuiltinType::Kind::Long:
        case BuiltinType::Kind::LongLong:
            returnTy.type = FFIRefType::INTEGER_REF;
            returnTy.int_type.type = FFIIntegerType::Int64;
            break;

        case BuiltinType::Kind::ULong:
        case BuiltinType::Kind::ULongLong:
            returnTy.type = FFIRefType::INTEGER_REF;
            returnTy.int_type.type = FFIIntegerType::UInt64;
            break;

        case BuiltinType::Kind::Int128:
            returnTy.type = FFIRefType::INTEGER_REF;
            returnTy.int_type.type = FFIIntegerType::Int128;
            break;

        case BuiltinType::Kind::Float:
            returnTy.type = FFIRefType::FLOAT_REF;
            returnTy.float_type.type = FFIFloatType::Float;
            break;

        case BuiltinType::Kind::Double:
            returnTy.type = FFIRefType::FLOAT_REF;
            returnTy.float_type.type = FFIFloatType::Double;
            break;

        case BuiltinType::Kind::LongDouble:
            returnTy.type = FFIRefType::FLOAT_REF;
            returnTy.float_type.type = FFIFloatType::LongDouble;
            break;
        default:
            fprintf(stderr, "unknown type %s\n", qt.getAsString().c_str());
            abort();
        };
    } else {
        fprintf(stderr, "unknown type %s\n", qt.getAsString().c_str());
        abort();
    }

    return returnTy;
}

static void get_types_for_func(FunctionDecl *fd, FFITypeRef &returnTy, std::vector<FFITypeRef> &paramTys)
{
    returnTy = type_for_qual(fd->getReturnType());

    const FunctionProtoType *ft = fd->getType()->getAs<FunctionProtoType>();
    if (ft) {
        for (size_t i = 0; i < ft->getNumParams(); ++i)
            paramTys.push_back(FFITypeRef { type_for_qual(ft->getParamType(i)) });
    }
}


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
