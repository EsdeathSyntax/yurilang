#pragma once

#include <string>
#include <vector>
#include <memory>
#include <utility>

namespace Yuri::AST {

struct Node {
    virtual ~Node() = default;
};

struct Expr : public Node {
    virtual ~Expr() = default;
};

struct NullPtrExpr : public Expr {};

struct BoolLiteralExpr : public Expr {
    bool value;
};

struct Stmt : public Node {
    virtual ~Stmt() = default;
};

struct IfStmt : public Stmt {
    std::unique_ptr<Expr> condition;
    std::vector<std::unique_ptr<Node>> then_branch;
    std::vector<std::unique_ptr<Node>> else_branch;
};

struct BreakStmt : public Stmt {};

struct ForNumericStmt : public Stmt {
    std::string var_name;
    std::unique_ptr<Expr> start;
    std::unique_ptr<Expr> end;
    std::unique_ptr<Expr> step; // Optional, defaults to 1
    std::vector<std::unique_ptr<Node>> body;
};

struct ForInStmt : public Stmt {
    std::string var_name;
    std::unique_ptr<Expr> iterable;
    std::vector<std::unique_ptr<Node>> body;
};
    
struct WhileStmt : public Stmt {
    std::unique_ptr<Expr> condition;
    std::vector<std::unique_ptr<Node>> body;
};

struct Program : public Node {
    std::string module_name;
    std::string entry_function;
    std::vector<std::unique_ptr<Node>> statements;
};

struct TypeDecl : public Expr {
    std::string name;
};

struct TypeAliasDecl : public TypeDecl {
    std::string target_type;
};

struct StructField {
    std::string name;
    std::string type_str;
};

struct StructTypeDecl : public TypeDecl {
    std::vector<StructField> fields;
};

struct Function : public Stmt {
    std::string visibility;
    std::string name;
    std::vector<std::pair<std::string, std::string>> params;
    std::string ret_type;
    std::vector<std::unique_ptr<Node>> body;
};

struct LiteralExpr : public Expr {
    int64_t value;
};

struct FloatLiteralExpr : public Expr {
    double value;
};

struct StringLiteralExpr : public Expr {
    std::string value;
};

struct VariableExpr : public Expr {
    std::string name;
};

struct BinaryExpr : public Expr {
    std::unique_ptr<Expr> left;
    std::string op;
    std::unique_ptr<Expr> right;
};

struct AssignExpr : public Expr {
    std::string name;
    std::unique_ptr<Expr> value;
};

struct CallExpr : public Expr {
    std::string module;
    std::string method;
    std::vector<std::unique_ptr<Expr>> arguments;
};

struct IndexExpr : public Expr {
    std::unique_ptr<Expr> target;
    std::unique_ptr<Expr> index;
};

struct ArrayLiteralExpr : public Expr {
    std::vector<std::unique_ptr<Expr>> elements;
};

struct TableEntryNode {
    std::string key;
    std::unique_ptr<Expr> value;
};

struct TableLiteralExpr : public Expr {
    std::vector<TableEntryNode> entries;
};

struct BitCastExpr : public Expr {
    std::unique_ptr<Expr> left;
    std::string op;
    std::unique_ptr<Expr> right;
};

struct VarDecl : public Stmt {
    std::string type;
    std::string name;
    std::unique_ptr<Expr> initializer;
};

struct ReturnStmt : public Stmt {
    std::unique_ptr<Expr> value;
};

struct Namespace : public Node {
    std::string name;
    std::vector<std::unique_ptr<Node>> members;
};

struct Class : public Node {
    std::string name;
    std::vector<std::unique_ptr<Node>> members;
};

struct ImportItem {
    std::string name;
    std::string alias;
    std::string source_mod;
};

struct ImportStmt : public Node {
    std::vector<ImportItem> items;
};

} // namespace Yuri::AST