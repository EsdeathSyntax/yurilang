#include "yuri/codegen.hpp"
#include "yuri/logger.hpp"
#include "yuri/registry.hpp"
#include "yuri/ast.hpp"
#include <iostream>
#include <stdexcept>
#include <fstream>
#include <cstring>
#include <csignal>
#include <execinfo.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <cstdio>
#include <cstdlib>
#include <ucontext.h>
#include <elf.h>
#include <cxxabi.h>
#include <dlfcn.h>
#include <ffi.h>
#include <link.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/DynamicLibrary.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/IR/Metadata.h>
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/StandardInstrumentations.h"
#include "llvm/Linker/Linker.h"
#include "llvm/Support/Error.h"


namespace Yuri {

struct NullableValue {
    uint8_t is_null;
    uint64_t bits;
}

llvm::Function* CodeGenerator::get_or_resolve_function(AST::CallExpr* call, const std::vector<llvm::Value*>& args) {
    std::string symbol_name = call->method;
    if (!call->module.empty()) {
        symbol_name = call->module + "_" + call->method;
    }

    if (auto* existing = module->getFunction(symbol_name)) {
        return existing;
    }

    std::vector<llvm::Type*> param_types;
    llvm::Type* ret_type = builder->getInt64Ty();

    auto* sig = Yuri::Registry::get_native_sig(symbol_name);
    if (!sig && call->module.empty()) {
        sig = Yuri::Registry::get_native_sig(call->method);
    }

    if (sig) {
        ret_type = get_llvm_type(sig->ret_type);
        for (const auto& p_str : sig->param_types) {
            param_types.push_back(get_llvm_type(p_str));
        }
    } else {
        throw std::runtime_error("Missing native function signature in Registry for symbol: " + symbol_name);
    }

    auto* fn_type = llvm::FunctionType::get(ret_type, param_types, false);
    return llvm::Function::Create(
        fn_type,
        llvm::Function::ExternalLinkage,
        symbol_name,
        module.get()
    );
}

CodeGenerator::CodeGenerator() {
    context = std::make_unique<llvm::LLVMContext>();
    module = std::make_unique<llvm::Module>("jit", *context);
    builder = std::make_unique<llvm::IRBuilder<>>(*context);
}

llvm::Type* CodeGenerator::get_llvm_type(const std::string& type_str) {
    if (!type_str.empty() && type_str.back() == '?') {
        std::string base_type_str = type_str.substr(0, type_str.length() - 1);
        llvm::Type* base_ty = get_llvm_type(base_type_str);
        
        std::string struct_name = "nullable_" + base_type_str;
        if (auto* existing_struct = llvm::StructType::getTypeByName(*context, struct_name)) {
            return existing_struct;
        }
        
        return llvm::StructType::create(*context, { builder->getInt1Ty(), base_ty }, struct_name);
    }

    if (type_str == "float" || type_str == "double" || type_str == "f64") {
        return builder->getDoubleTy();
    }
    if (type_str == "float32" || type_str == "f32") {
        return builder->getFloatTy();
    }
    if (type_str == "int" || type_str == "i64") {
        return builder->getInt64Ty();
    }
    if (type_str == "i32") {
        return builder->getInt32Ty();
    }
    if (type_str == "i1" || type_str == "bool") {
        return builder->getInt1Ty();
    }
    if (type_str == "void") {
        return builder->getVoidTy();
    }
    if (type_str == "string" || type_str == "str") {
        return llvm::PointerType::get(*context, 0);
    }

    if (auto* existing_struct = llvm::StructType::getTypeByName(*context, type_str)) {
        return existing_struct;
    }

    if (!type_str.empty() && type_str.back() == '*') {
        std::string base_type_name = type_str.substr(0, type_str.length() - 1);
        llvm::Type* base_ty = get_llvm_type(base_type_name);
        if (base_ty) {
            return llvm::PointerType::get(*context, 0);
        }
    }

    Logger::log(Subsystem::Codegen, LogLevel::Warning, "Unknown type '" + type_str + "', defaulting to i64.");
    return builder->getInt64Ty();
}

llvm::Value* CodeGenerator::codegen_expr(AST::Expr* expr) {
    if (!expr) return nullptr;

    if (auto null_expr = dynamic_cast<AST::NullPtrExpr*>(expr)) {
        return llvm::ConstantPointerNull::get(llvm::PointerType::get(*context, 0));
    }

    if (auto bool_lit = dynamic_cast<AST::BoolLiteralExpr*>(expr)) {
        return llvm::ConstantInt::get(builder->getInt1Ty(), bool_lit->value ? 1 : 0);
    }
    
    if (auto arr_lit = dynamic_cast<AST::ArrayLiteralExpr*>(expr)) {
        size_t count = arr_lit->elements.size();
        std::vector<llvm::Value*> evaluated_elements;
        for (size_t i = 0; i < count; ++i) {
            evaluated_elements.push_back(codegen_expr(arr_lit->elements[i].get()));
        }

        size_t alloc_bytes = 24 + (count * 8);
        auto* void_ptr_ty = llvm::PointerType::get(*context, 0);
        llvm::FunctionType* malloc_type = llvm::FunctionType::get(void_ptr_ty, {builder->getInt64Ty()}, false);
        llvm::Function* malloc_fn = llvm::cast<llvm::Function>(module->getOrInsertFunction("malloc", malloc_type).getCallee());
        llvm::Value* raw_mem = builder->CreateCall(malloc_fn, {builder->getInt64(alloc_bytes)}, "arr_mem");

        llvm::Value* tag_ptr = builder->CreatePointerCast(raw_mem, builder->getInt64Ty()->getPointerTo());
        builder->CreateStore(builder->getInt64(1), tag_ptr);
    
        llvm::Value* cap_ptr = builder->CreateConstGEP1_64(builder->getInt64Ty(), tag_ptr, 1);
        builder->CreateStore(builder->getInt64(count), cap_ptr);
        
        llvm::Value* len_ptr = builder->CreateConstGEP1_64(builder->getInt64Ty(), tag_ptr, 2);
        builder->CreateStore(builder->getInt64(count), len_ptr);

        llvm::Value* data_base = builder->CreateConstGEP1_64(builder->getInt64Ty(), tag_ptr, 3);
        for (size_t i = 0; i < count; ++i) {
            llvm::Value* val = evaluated_elements[i];
            llvm::Value* val_bits = val->getType()->isFloatingPointTy() ? builder->CreateBitCast(val, builder->getInt64Ty())
                : (val->getType()->isPointerTy() ? builder->CreatePtrToInt(val, builder->getInt64Ty()) : builder->CreateIntCast(val, builder->getInt64Ty(), false));

            llvm::Value* slot = builder->CreateConstGEP1_64(builder->getInt64Ty(), data_base, i, "arr_slot");
            builder->CreateStore(val_bits, slot);
        }
        return raw_mem;
    }

    if (auto str_lit = dynamic_cast<AST::StringLiteralExpr*>(expr)) {
        return builder->CreateGlobalStringPtr(str_lit->value);
    }

    if (auto tbl_lit = dynamic_cast<AST::TableLiteralExpr*>(expr)) {
        size_t count = tbl_lit->entries.size();
        size_t alloc_size = 16 + (count * 16); 
        
        auto* void_ptr_ty = llvm::PointerType::get(*context, 0);
        llvm::FunctionType* malloc_type = llvm::FunctionType::get(void_ptr_ty, {builder->getInt64Ty()}, false);
        llvm::Function* malloc_fn = llvm::cast<llvm::Function>(module->getOrInsertFunction("malloc", malloc_type).getCallee());
        llvm::Value* raw_mem = builder->CreateCall(malloc_fn, {builder->getInt64(alloc_size)}, "tbl_mem");

        llvm::Value* tag_ptr = builder->CreatePointerCast(raw_mem, builder->getInt64Ty()->getPointerTo());
        builder->CreateStore(builder->getInt64(2), tag_ptr);

        llvm::Value* count_ptr = builder->CreateConstGEP1_64(builder->getInt64Ty(), tag_ptr, 1);
        builder->CreateStore(builder->getInt64(count), count_ptr);

        llvm::Value* entry_base = builder->CreateConstGEP1_64(builder->getInt64Ty(), tag_ptr, 2);
        for (size_t i = 0; i < count; ++i) {
            llvm::Value* key_str_ptr = builder->CreateGlobalStringPtr(tbl_lit->entries[i].key);
            llvm::Value* val = codegen_expr(tbl_lit->entries[i].value.get());

            llvm::Value* val_bits = val->getType()->isFloatingPointTy() 
                ? builder->CreateBitCast(val, builder->getInt64Ty())
                : (val->getType()->isPointerTy() ? builder->CreatePtrToInt(val, builder->getInt64Ty()) : builder->CreateIntCast(val, builder->getInt64Ty(), false));

            llvm::Value* key_slot = builder->CreateConstGEP1_64(builder->getInt64Ty(), entry_base, i * 2, "tbl_key_slot");
            builder->CreateStore(builder->CreatePtrToInt(key_str_ptr, builder->getInt64Ty()), key_slot);

            llvm::Value* val_slot = builder->CreateConstGEP1_64(builder->getInt64Ty(), entry_base, (i * 2) + 1, "tbl_val_slot");
            builder->CreateStore(val_bits, val_slot);
        }
        return raw_mem;
    }

    if (auto idx_expr = dynamic_cast<AST::IndexExpr*>(expr)) {
        llvm::Value* target = codegen_expr(idx_expr->target.get());
        llvm::Value* index = codegen_expr(idx_expr->index.get());

        llvm::Value* cap_ptr = builder->CreatePointerCast(target, builder->getInt64Ty()->getPointerTo());
        llvm::Value* data_base = builder->CreateConstGEP1_64(builder->getInt64Ty(), cap_ptr, 3);
        
        llvm::Value* target_slot = builder->CreateGEP(builder->getInt64Ty(), data_base, index, "index_slot");
        return builder->CreateLoad(builder->getInt64Ty(), target_slot, "index_load");
    }

    if (auto lit = dynamic_cast<AST::LiteralExpr*>(expr)) {
        return llvm::ConstantInt::get(*context, llvm::APInt(64, static_cast<uint64_t>(lit->value), false));
    }

    if (auto float_lit = dynamic_cast<AST::FloatLiteralExpr*>(expr)) {
        return llvm::ConstantFP::get(*context, llvm::APFloat(float_lit->value));
    }

    if (auto var = dynamic_cast<AST::VariableExpr*>(expr)) {
        auto it = named_values.find(var->name);
        if (it == named_values.end()) {
            throw std::runtime_error("Unknown variable: " + var->name);
        }
        llvm::AllocaInst* alloca = dyn_cast<llvm::AllocaInst>(it->second);
        return builder->CreateLoad(alloca ? alloca->getAllocatedType() : builder->getInt64Ty(), it->second, var->name.c_str());
    }

    if (auto assign = dynamic_cast<AST::AssignExpr*>(expr)) {
        llvm::Value* val = codegen_expr(assign->value.get());
        auto it = named_values.find(assign->name);
        if (it == named_values.end()) {
            throw std::runtime_error("Assignment to undeclared variable: " + assign->name);
        }
        builder->CreateStore(val, it->second);
        return val;
    }

   if (auto bin = dynamic_cast<AST::BinaryExpr*>(expr)) {
        llvm::Value* l = codegen_expr(bin->left.get());
        llvm::Value* r = codegen_expr(bin->right.get());
        if (!l || !r) return nullptr;

        bool l_is_nullable = l->getType()->isStructTy();
        bool r_is_nullable = r->getType()->isStructTy();
        bool l_is_nullptr = dynamic_cast<AST::NullPtrExpr*>(bin->left.get()) != nullptr;
        bool r_is_nullptr = dynamic_cast<AST::NullPtrExpr*>(bin->right.get()) != nullptr;

        if ((l_is_nullable && r_is_nullptr) || (r_is_nullable && l_is_nullptr)) {
            llvm::Value* nullable_val = l_is_nullable ? l : r;
            llvm::Value* is_null = builder->CreateExtractValue(nullable_val, {0}, "get_is_null");
            llvm::Value* cmp = builder->CreateICmpEQ(is_null, builder->getInt1(true), "is_null_cmp");
            if (bin->op == "!=") {
                cmp = builder->CreateNot(cmp, "is_not_null_cmp");
            }
            return cmp;
        }

        if (l_is_nullable && !r_is_nullable && !r_is_nullptr) {
            l = builder->CreateExtractValue(l, {1}, "unwrap_l_nullable");
        }
        if (r_is_nullable && !l_is_nullable && !l_is_nullptr) {
            r = builder->CreateExtractValue(r, {1}, "unwrap_r_nullable");
        }

        const std::string& op = bin->op;

        if (l->getType()->isPointerTy() || r->getType()->isPointerTy()) {
            if (l->getType() != r->getType()) {
                if (l->getType()->isPointerTy() && r->getType()->isIntegerTy()) {
                    r = builder->CreateIntToPtr(r, l->getType(), "r_to_ptr");
                } else if (r->getType()->isPointerTy() && l->getType()->isIntegerTy()) {
                    l = builder->CreateIntToPtr(l, r->getType(), "l_to_ptr");
                }
            }
            if (op == "==") return builder->CreateICmpEQ(l, r, "ptr_eq");
            if (op == "!=") return builder->CreateICmpNE(l, r, "ptr_ne");
            throw std::runtime_error("Unsupported operator for pointer types: " + op);
        }

        bool is_float = l->getType()->isFloatingPointTy() || r->getType()->isFloatingPointTy();
        if (is_float) {
            if (l->getType()->isIntegerTy()) l = builder->CreateSIToFP(l, builder->getDoubleTy(), "l_cast_fp");
            if (r->getType()->isIntegerTy()) r = builder->CreateSIToFP(r, builder->getDoubleTy(), "r_cast_fp");
        } else if (l->getType()->isIntegerTy() && r->getType()->isIntegerTy()) {
            unsigned l_bits = l->getType()->getIntegerBitWidth();
            unsigned r_bits = r->getType()->getIntegerBitWidth();
            if (l_bits < r_bits) {
                l = builder->CreateIntCast(l, r->getType(), true, "ext_l");
            } else if (r_bits < l_bits) {
                r = builder->CreateIntCast(r, l->getType(), true, "ext_r");
            }
        }

        if (op == "+") return is_float ? builder->CreateFAdd(l, r, "faddtmp") : builder->CreateAdd(l, r, "addtmp");
        if (op == "-") return is_float ? builder->CreateFSub(l, r, "fsubtmp") : builder->CreateSub(l, r, "subtmp");
        if (op == "*") return is_float ? builder->CreateFMul(l, r, "fmultmp") : builder->CreateMul(l, r, "multmp");
        if (op == "/") return is_float ? builder->CreateFDiv(l, r, "fdivtmp") : builder->CreateSDiv(l, r, "idivtmp");
        
        if (op == "==") return is_float ? builder->CreateFCmpOEQ(l, r, "feqtmp") : builder->CreateICmpEQ(l, r, "ieqtmp");
        if (op == "!=") return is_float ? builder->CreateFCmpONE(l, r, "fnetmp") : builder->CreateICmpNE(l, r, "inetmp");
        if (op == "<")  return is_float ? builder->CreateFCmpOLT(l, r, "flttmp") : builder->CreateICmpSLT(l, r, "ilttmp");
        if (op == ">")  return is_float ? builder->CreateFCmpOGT(l, r, "fgttmp") : builder->CreateICmpSGT(l, r, "igttmp");
        if (op == "<=") return is_float ? builder->CreateFCmpOLE(l, r, "fletmp") : builder->CreateICmpSLE(l, r, "iletmp");
        if (op == ">=") return is_float ? builder->CreateFCmpOGE(l, r, "fgetmp") : builder->CreateICmpSGE(l, r, "igetmp");

        throw std::runtime_error("Unsupported binary operator: " + op);
    }
    if (auto call = dynamic_cast<AST::CallExpr*>(expr)) {
        if ((call->module.empty() || call->module == "io") && (call->method == "__print__" || call->method == "io_write" || call->method == "write")) {
            if (call->arguments.empty()) return nullptr;
            llvm::Value* val = codegen_expr(call->arguments[0].get());
            
            llvm::Value* bits_val = val->getType()->isFloatingPointTy() ? builder->CreateBitCast(val, builder->getInt64Ty())
                : (val->getType()->isPointerTy() ? builder->CreatePtrToInt(val, builder->getInt64Ty()) : builder->CreateIntCast(val, builder->getInt64Ty(), true));

            llvm::Module* mod_ptr = builder->GetInsertBlock()->getModule();
            llvm::FunctionType* write_fn_type = llvm::FunctionType::get(builder->getInt64Ty(), {builder->getInt64Ty()}, false);
            llvm::Function* write_fn = mod_ptr->getFunction("io_write");
            if (!write_fn) {
                write_fn = llvm::Function::Create(write_fn_type, llvm::Function::ExternalLinkage, "io_write", mod_ptr);
            }
            return builder->CreateCall(write_fn, {bits_val});
        }
        else {
            std::vector<llvm::Value*> args;
            for (const auto& arg : call->arguments) {
                args.push_back(codegen_expr(arg.get()));
            }
            llvm::Function* callee = get_or_resolve_function(call, args);
            if (!callee) throw std::runtime_error("Unknown function call: " + call->method);

            const auto& param_tys = callee->getFunctionType()->params();
            for (size_t i = 0; i < args.size() && i < param_tys.size(); ++i) {
                if (args[i]->getType() != param_tys[i]) {
                    if (args[i]->getType()->isIntegerTy() && param_tys[i]->isFloatingPointTy()) {
                        args[i] = builder->CreateSIToFP(args[i], param_tys[i], "arg_sitofp");
                    } else if (args[i]->getType()->isFloatingPointTy() && param_tys[i]->isIntegerTy()) {
                        args[i] = builder->CreateFPToSI(args[i], param_tys[i], "arg_fptosi");
                    } else if (args[i]->getType()->isStructTy() && param_tys[i]->isFloatingPointTy()) {
                        args[i] = builder->CreateExtractValue(args[i], {1}, "unwrap_nullable_val");
                    }
                }
            }

            return builder->CreateCall(callee, args, "calltmp");
        }
    }

    throw std::runtime_error("Unknown expression type in code generator");
}

void CodeGenerator::codegen_stmt(AST::Node* stmt_node, llvm::Type* ret_type_llvm, bool& has_terminator) {
    if (!stmt_node || has_terminator) return;

    if (auto ret = dynamic_cast<AST::ReturnStmt*>(stmt_node)) {
        if (ret->value) {
            builder->CreateRet(codegen_expr(dynamic_cast<Yuri::AST::Expr*>(ret->value.get())));
        } else {
            builder->CreateRetVoid();
        }
        has_terminator = true;
    } else if (auto break_stmt = dynamic_cast<AST::BreakStmt*>(stmt_node)) {
        if (loop_exit_stack.empty()) {
            throw std::runtime_error("Break statement used outside of a loop.");
        }
        builder->CreateBr(loop_exit_stack.back());
        has_terminator = true;
    } else if (auto expr = dynamic_cast<AST::Expr*>(stmt_node)) {
        codegen_expr(expr);
    } else if (auto decl = dynamic_cast<AST::VarDecl*>(stmt_node)) {
        llvm::Value* init_val = decl->initializer ? codegen_expr(decl->initializer.get()) : nullptr;
        
        bool is_container_lit = decl->initializer && (
            dynamic_cast<AST::TableLiteralExpr*>(decl->initializer.get()) || 
            dynamic_cast<AST::ArrayLiteralExpr*>(decl->initializer.get())
        );

        llvm::Type* llvm_type = nullptr;
        if (!decl->type.empty() && decl->type != "auto" && !is_container_lit) {
            llvm_type = get_llvm_type(decl->type);
        } else if (init_val) {
            llvm_type = is_container_lit ? llvm::PointerType::get(*context, 0) : init_val->getType();
        } else {
            llvm_type = builder->getInt64Ty();
        }

        llvm::AllocaInst* alloca = builder->CreateAlloca(llvm_type, nullptr, decl->name.c_str());
        if (init_val) {
            builder->CreateStore(init_val, alloca);
        }
        named_values[decl->name] = alloca;
    } else if (auto while_stmt = dynamic_cast<AST::WhileStmt*>(stmt_node)) {
        llvm::Function* parent_fn = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* cond_bb = llvm::BasicBlock::Create(*context, "while.cond", parent_fn);
        llvm::BasicBlock* body_bb = llvm::BasicBlock::Create(*context, "while.body", parent_fn);
        llvm::BasicBlock* merge_bb = llvm::BasicBlock::Create(*context, "while.end", parent_fn);

        builder->CreateBr(cond_bb);
        builder->SetInsertPoint(cond_bb);

        llvm::Value* cond_val = codegen_expr(while_stmt->condition.get());
        if (!cond_val->getType()->isIntegerTy(1)) {
            cond_val = builder->CreateICmpNE(cond_val, builder->getInt64(0), "to_bool");
        }
        builder->CreateCondBr(cond_val, body_bb, merge_bb);

        builder->SetInsertPoint(body_bb);
        loop_exit_stack.push_back(merge_bb);
        bool body_terminator = false;
        for (const auto& body_node : while_stmt->body) {
            codegen_stmt(body_node.get(), ret_type_llvm, body_terminator);
            if (body_terminator) break;
        }
        loop_exit_stack.pop_back();
        
        if (!body_terminator) {
            builder->CreateBr(cond_bb);
        }

        builder->SetInsertPoint(merge_bb);
    } else if (auto num_for = dynamic_cast<AST::ForNumericStmt*>(stmt_node)) {
        llvm::Value* start_val = codegen_expr(num_for->start.get());
        llvm::Type* var_ty = start_val->getType();
        llvm::AllocaInst* alloca = builder->CreateAlloca(var_ty, nullptr, num_for->var_name);
        builder->CreateStore(start_val, alloca);
        named_values[num_for->var_name] = alloca;

        llvm::Function* parent_fn = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* cond_bb = llvm::BasicBlock::Create(*context, "for.cond", parent_fn);
        llvm::BasicBlock* body_bb = llvm::BasicBlock::Create(*context, "for.body", parent_fn);
        llvm::BasicBlock* update_bb = llvm::BasicBlock::Create(*context, "for.update", parent_fn);
        llvm::BasicBlock* merge_bb = llvm::BasicBlock::Create(*context, "for.end", parent_fn);

        builder->CreateBr(cond_bb);
        builder->SetInsertPoint(cond_bb);

        llvm::Value* current_val = builder->CreateLoad(var_ty, alloca, num_for->var_name);
        llvm::Value* end_val = codegen_expr(num_for->end.get());
        llvm::Value* cond = builder->CreateICmpSLE(current_val, end_val, "loop_cond");
        builder->CreateCondBr(cond, body_bb, merge_bb);

        builder->SetInsertPoint(body_bb);
        loop_exit_stack.push_back(merge_bb);
        bool body_term = false;
        for (const auto& body_node : num_for->body) {
            codegen_stmt(body_node.get(), ret_type_llvm, body_term);
            if (body_term) break;
        }
        loop_exit_stack.pop_back();

        if (!body_term) builder->CreateBr(update_bb);

        builder->SetInsertPoint(update_bb);
        llvm::Value* step_val = num_for->step ? codegen_expr(num_for->step.get()) : llvm::ConstantInt::get(var_ty, 1);
        llvm::Value* next_val = builder->CreateAdd(current_val, step_val, "step_add");
        builder->CreateStore(next_val, alloca);
        builder->CreateBr(cond_bb);

        builder->SetInsertPoint(merge_bb);
    } 
    else if (auto in_for = dynamic_cast<AST::ForInStmt*>(stmt_node)) {
        llvm::Value* target = codegen_expr(in_for->iterable.get());
        llvm::Value* cap_ptr = builder->CreatePointerCast(target, builder->getInt64Ty()->getPointerTo());
        llvm::Value* len_ptr = builder->CreateConstGEP1_64(builder->getInt64Ty(), cap_ptr, 2);
        llvm::Value* length = builder->CreateLoad(builder->getInt64Ty(), len_ptr, "arr_len");
        llvm::Value* data_base = builder->CreateConstGEP1_64(builder->getInt64Ty(), cap_ptr, 3);

        llvm::AllocaInst* idx_alloca = builder->CreateAlloca(builder->getInt64Ty(), nullptr, "iter_idx");
        builder->CreateStore(builder->getInt64(0), idx_alloca);

        llvm::AllocaInst* val_alloca = builder->CreateAlloca(builder->getInt64Ty(), nullptr, in_for->var_name);
        named_values[in_for->var_name] = val_alloca;

        llvm::Function* parent_fn = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* cond_bb = llvm::BasicBlock::Create(*context, "in.cond", parent_fn);
        llvm::BasicBlock* body_bb = llvm::BasicBlock::Create(*context, "in.body", parent_fn);
        llvm::BasicBlock* update_bb = llvm::BasicBlock::Create(*context, "in.update", parent_fn);
        llvm::BasicBlock* merge_bb = llvm::BasicBlock::Create(*context, "in.end", parent_fn);

        builder->CreateBr(cond_bb);
        builder->SetInsertPoint(cond_bb);

        llvm::Value* idx_val = builder->CreateLoad(builder->getInt64Ty(), idx_alloca, "curr_idx");
        llvm::Value* cond = builder->CreateICmpSLT(idx_val, length, "iter_cond");
        builder->CreateCondBr(cond, body_bb, merge_bb);

        builder->SetInsertPoint(body_bb);
        llvm::Value* slot = builder->CreateGEP(builder->getInt64Ty(), data_base, idx_val, "iter_slot");
        llvm::Value* elem_val = builder->CreateLoad(builder->getInt64Ty(), slot, "iter_elem");
        builder->CreateStore(elem_val, val_alloca);

        loop_exit_stack.push_back(merge_bb);
        bool body_term = false;
        for (const auto& body_node : in_for->body) {
            codegen_stmt(body_node.get(), ret_type_llvm, body_term);
            if (body_term) break;
        }
        loop_exit_stack.pop_back();

        if (!body_term) builder->CreateBr(update_bb);

        builder->SetInsertPoint(update_bb);
        llvm::Value* next_idx = builder->CreateAdd(idx_val, builder->getInt64(1), "next_idx");
        builder->CreateStore(next_idx, idx_alloca);
        builder->CreateBr(cond_bb);

        builder->SetInsertPoint(merge_bb);
    }
}

void CodeGenerator::compile(AST::Program* program) {
    target_entry_function = program->entry_function.empty() ? "main" : program->entry_function;

    for (const auto& stmt : program->statements) {
        if (auto fn = dynamic_cast<AST::Function*>(stmt.get())) {
            std::vector<llvm::Type*> arg_types;
            for (const auto& param : fn->params) {
                arg_types.push_back(get_llvm_type(param.second));
            }

            llvm::Type* ret_type_llvm = (fn->ret_type == "void") ? builder->getVoidTy() : get_llvm_type(fn->ret_type);
            llvm::FunctionType* fn_type = llvm::FunctionType::get(ret_type_llvm, arg_types, false);
            llvm::Function* function = llvm::Function::Create(fn_type, llvm::Function::ExternalLinkage, fn->name, module.get());

            size_t idx = 0;
            for (auto& arg : function->args()) {
                arg.setName(fn->params[idx++].first);
            }

            builder->SetInsertPoint(llvm::BasicBlock::Create(*context, "entry", function));
            named_values.clear();
            loop_exit_stack.clear();
            for (auto& arg : function->args()) {
                llvm::AllocaInst* alloca = builder->CreateAlloca(arg.getType(), nullptr, arg.getName());
                builder->CreateStore(&arg, alloca);
                named_values[std::string(arg.getName())] = alloca;
            }

            if (fn->name == target_entry_function) {
                entry_param_count = fn->params.size();
                entry_returns_void = (fn->ret_type == "void");
                entry_param_types.clear();
                for (const auto& param : fn->params) {
                    entry_param_types.push_back(param.second);
                }
            }

            bool has_terminator = false;
            for (const auto& body_stmt : fn->body) {
                codegen_stmt(body_stmt.get(), ret_type_llvm, has_terminator);
                if (has_terminator) break;
            }

            if (!has_terminator) {
                if (ret_type_llvm->isVoidTy()) builder->CreateRetVoid();
                else builder->CreateRet(builder->getInt64(0));
            }
            llvm::verifyFunction(*function);
        }
    }
}

ffi_type* CodeGenerator::get_ffi_type(const std::string& type_str) {
    return &ffi_type_pointer;
}

void CodeGenerator::exec(const std::vector<std::string>& raw_args) {

    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    llvm::InitializeNativeTargetAsmParser();
    llvm::sys::DynamicLibrary::LoadLibraryPermanently(nullptr);

    std::error_code ec;
    llvm::raw_fd_ostream dest("output.ll", ec, llvm::sys::fs::OF_None);
    if (!ec) {
        module->print(dest, nullptr);
        dest.flush();
    }

    auto JITBuilder = llvm::orc::LLJITBuilder();
    auto JIT = cantFail(JITBuilder.create());

    auto &MainDylib = JIT->getMainJITDylib();
    auto &ES = JIT->getExecutionSession();

    MainDylib.addGenerator(
        cantFail(llvm::orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
            JIT->getDataLayout().getGlobalPrefix()))
    );

    llvm::orc::SymbolMap native_symbols;
    for (const auto& [name, ptr] : Yuri::Registry::get_all_native_fns()) {
        if (ptr) {
            native_symbols[ES.intern(name)] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(ptr), 
                llvm::JITSymbolFlags::Exported
            );
        }
    }
    cantFail(MainDylib.define(llvm::orc::absoluteSymbols(native_symbols)));
    cantFail(JIT->addIRModule(llvm::orc::ThreadSafeModule(std::move(module), std::move(context))));

    auto sym_result = JIT->lookup(target_entry_function);
    if (!sym_result) {
        std::string err_str = llvm::toString(sym_result.takeError());
        throw std::runtime_error("Failed to resolve entry symbol '" + target_entry_function + "': " + err_str);
    }

    auto sym = *sym_result;
    void* fn_ptr = sym.toPtr<void*>();
    if (!fn_ptr) {
        throw std::runtime_error("Resolved entry symbol '" + target_entry_function + "' evaluated to a null address.");
    }

    auto result;

    if (entry_param_count == 1 && entry_param_types[0].back() == '?') {
        using EntryFunc = void(*)(NullableValue);
        auto entry_fn = reinterpret_cast<EntryFunc>(fn_ptr);
            
        NullableValue default_arg{1, 0}; 
        result = entry_returns_void ? nullptr : entry_fn(default_arg);
    } else {
        using EntryFuncVoid = void(*)();
        auto entry_fn = reinterpret_cast<EntryFuncVoid>(fn_ptr);
        result = entry_returns_void ? nullptr : entry_fn();
    }

    if (result != nullptr) {
        std::cout << "Programmed exited with code " << result << "\n";
    }
}

} // namespace Yuri