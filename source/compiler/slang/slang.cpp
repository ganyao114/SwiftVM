//
// Created by 甘尧 on 2023/11/24.
//

#include "base/logging.h"
#include "slang.h"
#include "slangLexer.h"
#include "slangParser.h"
#include "slangVisitor.h"

namespace swift::slang {

class SlangVisitor : public slangVisitor {
public:
    explicit SlangVisitor(Context &context) : ctx(context) {};

    std::any visitFile(slangParser::FileContext* context) override {
        return visitChildren(context);
    }

    std::any visitNamespaceDef(slangParser::NamespaceDefContext* context) override {
        auto name_space = context->namespaceIdentifier()->getText();
        ctx.SetNamespace(name_space);
        return visitChildren(context);
    }

    std::any visitFunctionHeader(slangParser::FunctionHeaderContext* context) override {
        return visitChildren(context);
    }

    std::any visitFunctionDef(slangParser::FunctionDefContext* context) override {
        return visitChildren(context);
    }

    std::any visitFieldDef(slangParser::FieldDefContext* context) override {
        auto acc_flag = context->accFlag->getText();
        auto filed_name = context->fieldName->getText();
        auto field_type = context->fieldType->getText();
        auto has_const = context->Const() != nullptr;
        return visitExpression(context->expression());
    }

    std::any visitFunctionImpl(slangParser::FunctionImplContext* context) override {
        auto functionName = context->functionHeader()->functionName->getText();
        return visitChildren(context);
    }

    std::any visitInterfaceDef(slangParser::InterfaceDefContext* context) override {
        return visitChildren(context);
    }

    std::any visitClassDef(slangParser::ClassDefContext* context) override {
        return visitChildren(context);
    }

    std::any visitAccessModifier(slangParser::AccessModifierContext* context) override {
        return visitChildren(context);
    }

    std::any visitFunctionParams(slangParser::FunctionParamsContext* context) override {
        return visitChildren(context);
    }

    std::any visitFunctionParam(slangParser::FunctionParamContext* context) override {
        return visitChildren(context);
    }

    std::any visitNamespaceIdentifier(slangParser::NamespaceIdentifierContext* context) override {
        return visitChildren(context);
    }

    std::any visitCodeBlock(slangParser::CodeBlockContext* context) override {
        return visitChildren(context);
    }

    std::any visitStatement(slangParser::StatementContext* context) override {
        return visitChildren(context);
    }

    std::any visitAssignmentStatement(slangParser::AssignmentStatementContext* context) override {
        auto receiver = context->receiver->getText();
        return visitChildren(context);
    }

    std::any visitLocalVariableDeclarationStatement(
            slangParser::LocalVariableDeclarationStatementContext* context) override {
        auto receiver = context->localName->getText();
        return visitChildren(context);
    }

    std::any visitExpressionStatement(slangParser::ExpressionStatementContext* context) override {
        return visitChildren(context);
    }

    std::any visitReturnStatement(slangParser::ReturnStatementContext* context) override {
        return visitChildren(context);
    }

    std::any visitArguments(slangParser::ArgumentsContext* context) override {
        return visitChildren(context);
    }

    std::any visitExpression(slangParser::ExpressionContext* context) override {
        return visitChildren(context);
    }

    std::any visitLiteral(slangParser::LiteralContext* context) override {
        return visitChildren(context);
    }

    std::any visitConditionExpression(slangParser::ConditionExpressionContext* context) override {
        return visitChildren(context);
    }

    std::any visitAdditionExpression(slangParser::AdditionExpressionContext* context) override {
        return visitChildren(context);
    }

    std::any visitMethodCallExpression(slangParser::MethodCallExpressionContext* context) override {
        auto method_name = context->methodName->getText();
        auto args = context->arguments();
        auto fist_arg = args->argFirst->getText();
        return visitChildren(context);
    }

    std::any visitMultiplicationExpression(
            slangParser::MultiplicationExpressionContext* context) override {
        return visitChildren(context);
    }

    std::any visitPrimaryExpression(slangParser::PrimaryExpressionContext* context) override {
        return visitChildren(context);
    }

private:
    Context &ctx;
};

void CompileFile(const std::string &path, Context &context) {
    std::ifstream file(path);
    ASSERT(!file.bad());
    antlr4::ANTLRInputStream input(file);
    slangLexer lexer(&input);
    antlr4::CommonTokenStream tokens(&lexer);
    slangParser parser(&tokens);
    auto file_tree = parser.file();
    SlangVisitor visitor{context};
    visitor.visit(file_tree);
    file.close();
}

void CompileContent(const std::string &content, Context &context) {
    antlr4::ANTLRInputStream input(content);
    slangLexer lexer(&input);
    antlr4::CommonTokenStream tokens(&lexer);
    slangParser parser(&tokens);
    auto file_tree = parser.file();
    SlangVisitor visitor{context};
    visitor.visit(file_tree);
}

}  // namespace swift::slang
