#pragma once
#include "yuri/ast.hpp"
#include <vector>
#include <string>
#include <memory>
#include <unordered_map>
#include <ffi.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>

namespace Yuri {

class CodeGenerator {
private:
    std::unique_ptr<llvm::LLVMContext> context;
    std::unique_ptr<llvm::Module> module;
    std::unique_ptr<llvm::IRBuilder<>> builder;
    std::vector<llvm::BasicBlock*> loop_exit_stack;
    
    std::unordered_map<std::string, llvm::Value*> named_values;
    std::unordered_map<std::string, std::string> type_aliases;
    std::unordered_map<std::string, std::vector<std::pair<std::string, std::string>>> struct_definitions;

    std::string target_entry_function = "main";
    size_t entry_param_count = 0;
    std::vector<std::string> entry_param_types;
    bool entry_returns_void = true;

    llvm::Value* codegen_expr(AST::Expr* expr);
    void codegen_stmt(AST::Node* stmt_node, llvm::Type* ret_type_llvm, bool& has_terminator);
    llvm::Function* get_or_resolve_function(AST::CallExpr* call, const std::vector<llvm::Value*>& args);
    llvm::Type* get_llvm_type(const std::string& type_str);
    void optimize_module(llvm::Module& module);
    llvm::Value* codegen_array_push(llvm::Value* arr_ptr, llvm::Value* val);
public:
    CodeGenerator();
    void compile(AST::Program* program);
    void exec(const std::vector<std::string>& raw_args);
    ffi_type* get_ffi_type(const std::string& type_str);
    
};

} // namespace Yuri