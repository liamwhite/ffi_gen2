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

static FFITypeRef type_for_qual(QualType qt, ASTContext *ctx);
static void get_types_for_func(FunctionDecl *fd, FFITypeRef &returnTy, std::vector<FFITypeRef> &paramTys, ASTContext *ctx);

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
    MacroParseAction(callbacks &cb, std::vector<std::string> &sources) : cb(cb), sources(sources) {}

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

                for (auto &t : tokens) {
                    tokenPaste.append(p.getSpelling(t));
                    tokenPaste.append(" ");
                }

                cb.mc(m.first.c_str(), tokenPaste.c_str(), cb.user_data);
            } catch (std::invalid_argument &ex) {
                // do nothing
            }
        }
    }

private:
    std::vector<Token> fixMacrosRecursive(Preprocessor &p, std::vector<Token> input, size_t recursionNum = 0)
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

            if (!mi || recursionNum > 5) {
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

            childTokens = fixMacrosRecursive(p, childTokens, recursionNum + 1);

            for (auto &k : childTokens)
                ret.push_back(k);
        }

        return ret;
    }

    callbacks &cb;
    std::vector<std::string> &sources;
};

class FFIGenVisitor : public RecursiveASTVisitor<FFIGenVisitor>
{
public:
    explicit FFIGenVisitor(ASTContext *Context, callbacks &cb, std::vector<std::string> &sources)
        : Context(Context), cb(cb), sources(sources) {}

    bool isInRequestedSourceFiles(SourceLocation l)
    {
        clang::SourceManager &sm { Context->getSourceManager() };

        l = sm.getExpansionLoc(l);

        for (auto &f : sources)
            if (sm.getFilename(l) == f)
                return true;

        return false;
    }

    virtual bool VisitFunctionDecl(FunctionDecl *func)
    {
        if (!isInRequestedSourceFiles(func->getLocStart()))
            return true;

        FFITypeRef returnTy;
        std::string funcName = func->getNameInfo().getName().getAsString();
        std::vector<FFITypeRef> paramTys;

        get_types_for_func(func, returnTy, paramTys, Context);

        cb.fc(funcName.c_str(), &returnTy, &paramTys[0], paramTys.size(), cb.user_data);

        return true;
    }

    virtual bool VisitVarDecl(VarDecl *vd)
    {
        if (!isInRequestedSourceFiles(vd->getLocStart()))
            return true;

        // Don't try to do binding for non-exported variables
        if (!vd->isExternC())
            return true;

        std::string name = vd->getNameAsString();
        FFITypeRef varTy = type_for_qual(vd->getType(), Context);

        cb.vc(name.c_str(), &varTy, cb.user_data);

        return true;
    }

    virtual bool VisitEnumDecl(EnumDecl *ed)
    {
        // Don't visit forward declarations
        if (!ed->isThisDeclarationADefinition())
            return true;

        ed = ed->getCanonicalDecl();

        if (!isInRequestedSourceFiles(ed->getLocStart()))
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
        if (!isInRequestedSourceFiles(td->getLocStart()))
            return true;

        std::string aliasName = td->getNameAsString();
        FFITypeRef type = type_for_qual(td->getUnderlyingType(), Context);

        cb.tc(aliasName.c_str(), &type, cb.user_data);

        return true;
    }

    virtual bool VisitRecordDecl(RecordDecl *rd)
    {
        // Don't visit forward declarations
        if (!rd->isThisDeclarationADefinition())
            return tag_forward_decl(rd);

        rd = rd->getDefinition();

        if (!rd || !isInRequestedSourceFiles(rd->getLocStart()))
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

            memberTypes.push_back(type_for_qual(f->getType(), Context));
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
    bool tag_forward_decl(TagDecl *td)
    {
        // Don't try to do binding for non-exported names
        if (!td->hasNameForLinkage())
            return true;

        if (!isInRequestedSourceFiles(td->getLocStart()))
            return true;

        std::string name = td->getNameAsString();
        if (name.size() == 0)
            name = td->getTypedefNameForAnonDecl()->getUnderlyingType().getAsString();

        FFIForwardType t;

        if (td->isUnion())
            t = FFIForwardType::UNION;
        else
            t = FFIForwardType::STRUCT;

        cb.fdc(name.c_str(), t, cb.user_data);

        return true;
    }

    ASTContext *Context;
    callbacks &cb;
    std::vector<std::string> &sources;
};


class FFIParseConsumer : public clang::ASTConsumer
{
public:
    explicit FFIParseConsumer(ASTContext *Context, callbacks &cb, std::vector<std::string> &sources) : Visitor(Context, cb, sources)
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
    FFIParseAction(callbacks &cb, std::vector<std::string> &sources) : cb(cb), sources(sources) {}

    virtual std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(clang::CompilerInstance &Compiler, llvm::StringRef InFile)
    {
        return std::unique_ptr<clang::ASTConsumer> { new FFIParseConsumer { &Compiler.getASTContext(), cb, sources } };
    }

private:
    callbacks &cb;
    std::vector<std::string> &sources;
};

static FFITypeRef type_for_qual(QualType qt, ASTContext *ctx)
{
    FFITypeRef returnTy;
    returnTy.qual_name = strdup(qt.getAsString().c_str()); // LEAK

    if (qt->isVoidType()) {
        returnTy.type = FFIRefType::VOID_REF;
    } else if (qt->isPointerType()) {
        // LEAK
        FFITypeRef *pointee = new FFITypeRef { type_for_qual(qt->getPointeeType(), ctx) };

        returnTy.type = FFIRefType::POINTER_REF;
        returnTy.point_type.pointed_type = pointee;
    } else if (qt->isEnumeralType()) {
        const EnumDecl *ed = qt->castAs<EnumType>()->getDecl();

        returnTy.type = FFIRefType::ENUM_REF;

        if (ed->hasNameForLinkage()) {
            std::string name = ed->getNameAsString();
            if (name.size() == 0)
                name = ed->getTypedefNameForAnonDecl()->getUnderlyingType().getAsString();

            returnTy.enum_type.name = strdup(name.c_str()); // LEAK
            returnTy.enum_type.anonymous = 0;            
        } else {
            returnTy.enum_type.name = NULL;
            returnTy.enum_type.anonymous = 1;
        }
    } else if (qt->isRecordType()) {
        const RecordDecl *rd = qt->castAs<RecordType>()->getDecl();

        std::string name;
        std::vector<FFIRecordMember> *members = NULL;

        if (rd->isAnonymousStructOrUnion()) {
            // Only add fields in an anonymous record!
            members = new std::vector<FFIRecordMember>; // LEAK
            std::vector<FFITypeRef> *memberTypes = new std::vector<FFITypeRef>; // LEAK
            std::vector<std::string> memberNames;

            for (auto f : rd->fields()) {
                FFITypeRef type = type_for_qual(f->getType(), ctx);
                std::string memberName = f->getNameAsString();

                memberTypes->push_back(type);
                memberNames.push_back(memberName);
            }

            for (size_t i = 0; i < memberNames.size(); ++i) {
                FFIRecordMember m;
                if (memberNames[i].size() > 0)
                    m.name = strdup(memberNames[i].c_str()); // LEAK
                m.type = &((*memberTypes)[i]);

                members->push_back(m);
            }
        } else if (rd->hasNameForLinkage()) {
            name = rd->getNameAsString();
            if (name.size() == 0)
                name = rd->getTypedefNameForAnonDecl()->getUnderlyingType().getAsString();
        }

        if (qt->isUnionType()) {
            returnTy.type = FFIRefType::UNION_REF;
            returnTy.union_type.anonymous = rd->isAnonymousStructOrUnion();
            returnTy.union_type.members = NULL;
            returnTy.union_type.num_members = 0;
            returnTy.union_type.name = NULL;
            if (members) {
                returnTy.union_type.members = &((*members)[0]);
                returnTy.union_type.num_members = members->size();
            }
            if (name.size() > 0)
                returnTy.union_type.name = strdup(name.c_str()); // LEAK
        } else {
            returnTy.type = FFIRefType::STRUCT_REF;
            returnTy.struct_type.anonymous = rd->isAnonymousStructOrUnion();
            returnTy.struct_type.members = NULL;
            returnTy.struct_type.num_members = 0;
            returnTy.struct_type.name = NULL;
            if (members) {
                returnTy.struct_type.members = &((*members)[0]);
                returnTy.struct_type.num_members = members->size();
            }
            if (name.size() > 0)
                returnTy.struct_type.name = strdup(name.c_str()); // LEAK
        }
    } else if (qt->isFunctionProtoType()) {
        const FunctionProtoType *ft = qt->castAs<FunctionProtoType>();

        FFITypeRef *ret_type = new FFITypeRef { type_for_qual(ft->getReturnType(), ctx) }; // LEAK
        std::vector<FFITypeRef> *param_types = new std::vector<FFITypeRef>; // LEAK
        for (size_t i = 0; i < ft->getNumParams(); ++i)
            param_types->push_back(FFITypeRef { type_for_qual(ft->getParamType(i), ctx) } );

        returnTy.type = FFIRefType::FUNCTION_REF;
        returnTy.func_type.return_type = ret_type;
        returnTy.func_type.param_types = &((*param_types)[0]);
        returnTy.func_type.num_params = ft->getNumParams();
    } else if (qt->isFunctionNoProtoType()) {
        const FunctionNoProtoType *ft = qt->castAs<FunctionNoProtoType>();

        FFITypeRef *ret_type = new FFITypeRef { type_for_qual(ft->getReturnType(), ctx) }; // LEAK
        returnTy.type = FFIRefType::FUNCTION_REF;
        returnTy.func_type.return_type = ret_type;
        returnTy.func_type.param_types = nullptr;
        returnTy.func_type.num_params = 0;
    } else if (qt->isConstantArrayType()) {
        const ConstantArrayType *at = ctx->getAsConstantArrayType(qt);

        FFITypeRef *var_type = new FFITypeRef { type_for_qual(at->getElementType(), ctx) }; // LEAK
        returnTy.type = FFIRefType::ARRAY_REF;
        returnTy.array_type.type = var_type;
        returnTy.array_type.size = at->getSize().getZExtValue();
    } else if (qt->isIncompleteArrayType()) {
        const IncompleteArrayType *at = ctx->getAsIncompleteArrayType(qt);

        returnTy.type = FFIRefType::FLEX_REF;
        returnTy.flex_type.type = new FFITypeRef { type_for_qual(at->getElementType(), ctx) }; // LEAK
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

static void get_types_for_func(FunctionDecl *fd, FFITypeRef &returnTy, std::vector<FFITypeRef> &paramTys, ASTContext *ctx)
{
    returnTy = type_for_qual(fd->getReturnType(), ctx);

    const FunctionProtoType *ft = fd->getType()->getAs<FunctionProtoType>();
    if (ft) {
        for (size_t i = 0; i < ft->getNumParams(); ++i)
            paramTys.push_back(FFITypeRef { type_for_qual(ft->getParamType(i), ctx) });
    }
}


void walk_file(const char *filename, const char **clangArgs, int argc, const char **sourceLocations, int nloc, callbacks *c)
{
    std::ifstream t { filename };
    std::string inFile { std::istreambuf_iterator<char>(t), std::istreambuf_iterator<char>() };
    std::vector<std::string> args;
    std::vector<std::string> sources;

    for (int i = 0; i < argc; ++i)
        args.push_back(std::string { clangArgs[i] });

    for (int i = 0; i < nloc; ++i)
        sources.push_back(std::string { sourceLocations[i] });

    clang::tooling::runToolOnCodeWithArgs(new MacroParseAction { *c, sources }, inFile, args, filename);
    clang::tooling::runToolOnCodeWithArgs(new FFIParseAction { *c, sources }, inFile, args, filename);
}
