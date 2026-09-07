#include <vector>
#include <string>
#include <memory>
#include "yuri/lexer.hpp"
#include "yuri/ast.hpp"
#include "yuri/types.hpp"

namespace Yuri {


class Parser {
public:
    Parser(std::vector<Token> t, std::string filename);
    std::unique_ptr<AST::Program> parse();

private:
    Token peek() const;
    Token previous() const;
    bool is_at_end() const;
    Token advance();
    bool check(TokenType type) const;
    bool match(TokenType type);

    std::unique_ptr<AST::Node> parse_import();
    std::unique_ptr<AST::IfStmt> parse_if_stmt();
    std::unique_ptr<AST::VarDecl> parse_var_decl();
    std::unique_ptr<AST::Node> parse_declaration();
    std::unique_ptr<AST::Namespace> parse_namespace();
    std::unique_ptr<AST::Class> parse_class();
    std::unique_ptr<AST::Function> parse_function();
    std::shared_ptr<Type> parse_type();

    std::unique_ptr<AST::Expr> parse_expression();
    std::unique_ptr<AST::Expr> parse_comparison();
    std::unique_ptr<AST::Expr> parse_string_concat();
    std::unique_ptr<AST::Expr> parse_additive();
    std::unique_ptr<AST::Expr> parse_bitwise();
    std::unique_ptr<AST::Expr> parse_factor();
    std::unique_ptr<AST::Expr> parse_primary();
    std::unique_ptr<AST::WhileStmt> parse_while_stmt();
    std::unique_ptr<AST::Stmt> parse_for_stmt();
    std::unique_ptr<AST::Expr> parse_type_definition();
    std::string parse_type_string();

    std::vector<Token> tokens;
    size_t current = 0;
    std::string file_path;
};

} // namespace Yuri