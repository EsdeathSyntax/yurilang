#include "yuri/codegen.hpp"
#include "yuri/logger.hpp"
#include "yuri/registry.hpp"
#include "yuri/ast.hpp"
#include <iostream>
#include <stdexcept>
#include <fstream>
#include <cstring>
#include <elf.h>
#include <cxxabi.h>
#include <dlfcn.h>
#include <ffi.h>
#include <link.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/IR/Metadata.h>
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/StandardInstrumentations.h"
#include "llvm/Linker/Linker.h"

llvm::Function* declare_printf(llvm::Module* mod, llvm::LLVMContext& context) {
    if (auto* existing = mod->getFunction("printf")) {
        return existing;
    }

    auto* printf_type = llvm::FunctionType::get(
        llvm::Type::getInt32Ty(context),
        { llvm::PointerType::get(llvm::Type::getInt8Ty(context), 0) },
        true
    );

    return llvm::Function::Create(
        printf_type,
        llvm::Function::ExternalLinkage,
        "printf",
        mod
    );
}

namespace Yuri {

llvm::Function* CodeGenerator::get_or_resolve_function(AST::CallExpr* call, const std::vector<llvm::Value*>& args) {
    std::string symbol_name = call->method;
    if (!call->module.empty()) {
        symbol_name = call->module + "_" + call->method;
    }

    if (auto* existing = module->getFunction(symbol_name)) {
        return existing;
    }

    std::vector<llvm::Type*> param_types;
    for (auto* arg : args) {
        param_types.push_back(arg->getType());
    }

    llvm::Type* ret_type = builder->getInt64Ty();
    if (auto* sig = Yuri::Registry::get_native_sig(symbol_name)) {
        ret_type = get_llvm_type(sig->ret_type);
    } else if (!args.empty()) {
        ret_type = args[0]->getType();
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

    if (auto arr_lit = dynamic_cast<AST::ArrayLiteralExpr*>(expr)) {
        size_t count = arr_lit->elements.size();
        
        std::vector<llvm::Constant*> const_elements;
        bool can_be_global = true;

        for (size_t i = 0; i < count; ++i) {
            auto* el = arr_lit->elements[i].get();
            if (auto lit = dynamic_cast<AST::LiteralExpr*>(el)) {
                const_elements.push_back(llvm::ConstantInt::get(builder->getInt64Ty(), lit->value));
            } else if (auto float_lit = dynamic_cast<AST::FloatLiteralExpr*>(el)) {
                double dval = float_lit->value;
                uint64_t uval;
                std::memcpy(&uval, &dval, 8);
                const_elements.push_back(llvm::ConstantInt::get(builder->getInt64Ty(), uval));
            } else if (auto str_lit = dynamic_cast<AST::StringLiteralExpr*>(el)) {
                llvm::GlobalVariable* str_gv = builder->CreateGlobalString(str_lit->value, "str_lit", 0, module.get());
                llvm::Constant* zero = builder->getInt64(0);
                std::vector<llvm::Constant*> indices = {zero, zero};
                llvm::Constant* str_ptr = llvm::ConstantExpr::getGetElementPtr(str_gv->getValueType(), str_gv, indices);
                const_elements.push_back(llvm::ConstantExpr::getPtrToInt(str_ptr, builder->getInt64Ty()));
            } else {
                can_be_global = false;
                break;
            }
        }

        if (can_be_global && count > 0) {
            llvm::ArrayType* elements_array_type = llvm::ArrayType::get(builder->getInt64Ty(), count);
            llvm::Constant* elements_array_const = llvm::ConstantArray::get(elements_array_type, const_elements);

            llvm::StructType* global_arr_type = llvm::StructType::get(
                *context, 
                { builder->getInt64Ty(), builder->getInt64Ty(), elements_array_type }
            );

            llvm::Constant* struct_const = llvm::ConstantStruct::get(global_arr_type, {
                builder->getInt64(count),
                builder->getInt64(count),
                elements_array_const
            });

            llvm::GlobalVariable* global_arr = new llvm::GlobalVariable(
                *module,
                global_arr_type,
                true,
                llvm::GlobalValue::InternalLinkage,
                struct_const,
                "const_long_arr"
            );

            return builder->CreatePointerCast(global_arr, llvm::PointerType::get(*context, 0));
        }

        std::vector<llvm::Value*> evaluated_elements;
        for (size_t i = 0; i < count; ++i) {
            evaluated_elements.push_back(codegen_expr(arr_lit->elements[i].get()));
        }

        size_t alloc_bytes = 16 + (count * 8);
        auto* void_ptr_ty = llvm::PointerType::get(*context, 0);
        llvm::FunctionType* malloc_type = llvm::FunctionType::get(void_ptr_ty, {builder->getInt64Ty()}, false);
        llvm::Function* malloc_fn = llvm::cast<llvm::Function>(module->getOrInsertFunction("malloc", malloc_type).getCallee());
        llvm::Value* raw_mem = builder->CreateCall(malloc_fn, {builder->getInt64(alloc_bytes)}, "arr_mem");

        llvm::Value* cap_ptr = builder->CreatePointerCast(raw_mem, builder->getInt64Ty()->getPointerTo());
        builder->CreateStore(builder->getInt64(count), cap_ptr);
        
        llvm::Value* len_ptr = builder->CreateConstGEP1_64(builder->getInt64Ty(), cap_ptr, 1);
        builder->CreateStore(builder->getInt64(count), len_ptr);

        llvm::Value* data_base = builder->CreateConstGEP1_64(builder->getInt64Ty(), cap_ptr, 2);
        for (size_t i = 0; i < count; ++i) {
            llvm::Value* val = evaluated_elements[i];
            
            llvm::Value* val_bits = nullptr;
            if (val->getType()->isFloatingPointTy()) {
                val_bits = builder->CreateBitCast(val, builder->getInt64Ty());
            } else if (val->getType()->isPointerTy()) {
                val_bits = builder->CreatePtrToInt(val, builder->getInt64Ty());
            } else {
                val_bits = builder->CreateIntCast(val, builder->getInt64Ty(), false);
            }

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
        size_t alloc_size = 8 + (count * 16);
        
        auto* void_ptr_ty = llvm::PointerType::get(*context, 0);
        llvm::FunctionType* malloc_type = llvm::FunctionType::get(void_ptr_ty, {builder->getInt64Ty()}, false);
        llvm::Function* malloc_fn = llvm::cast<llvm::Function>(module->getOrInsertFunction("malloc", malloc_type).getCallee());
        llvm::Value* raw_mem = builder->CreateCall(malloc_fn, {builder->getInt64(alloc_size)}, "tbl_mem");

        llvm::Value* count_ptr = builder->CreatePointerCast(raw_mem, builder->getInt64Ty()->getPointerTo());
        builder->CreateStore(builder->getInt64(count), count_ptr);

        llvm::Value* entry_base = builder->CreateConstGEP1_64(builder->getInt64Ty(), count_ptr, 1);
        for (size_t i = 0; i < count; ++i) {
            uint64_t key_hash = std::hash<std::string>{}(tbl_lit->entries[i].key);
            llvm::Value* val = codegen_expr(tbl_lit->entries[i].value.get());

            llvm::Value* val_bits = val->getType()->isFloatingPointTy() 
                ? builder->CreateBitCast(val, builder->getInt64Ty())
                : (val->getType()->isPointerTy() ? builder->CreatePtrToInt(val, builder->getInt64Ty()) : builder->CreateIntCast(val, builder->getInt64Ty(), false));

            llvm::Value* key_slot = builder->CreateConstGEP1_64(builder->getInt64Ty(), entry_base, i * 2, "tbl_key_slot");
            builder->CreateStore(builder->getInt64(key_hash), key_slot);

            llvm::Value* val_slot = builder->CreateConstGEP1_64(builder->getInt64Ty(), entry_base, (i * 2) + 1, "tbl_val_slot");
            builder->CreateStore(val_bits, val_slot);
        }
        return raw_mem;
    }

    if (auto idx_expr = dynamic_cast<AST::IndexExpr*>(expr)) {
        llvm::Value* target = codegen_expr(idx_expr->target.get());
        llvm::Value* index = codegen_expr(idx_expr->index.get());

        llvm::Value* cap_ptr = builder->CreatePointerCast(target, builder->getInt64Ty()->getPointerTo());
        llvm::Value* data_base = builder->CreateConstGEP1_64(builder->getInt64Ty(), cap_ptr, 2);
        
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
            Logger::log(Subsystem::Codegen, LogLevel::Error, "Unknown variable reference: " + var->name);
            throw std::runtime_error("Unknown variable: " + var->name);
        }

        llvm::AllocaInst* alloca = dyn_cast<llvm::AllocaInst>(it->second);
        llvm::Type* load_ty = alloca ? alloca->getAllocatedType() : builder->getInt64Ty();

        llvm::Value* val = builder->CreateLoad(load_ty, it->second, var->name.c_str());
        return val;
    }

    if (auto assign = dynamic_cast<AST::AssignExpr*>(expr)) {
        llvm::Value* val = codegen_expr(assign->value.get());
        auto it = named_values.find(assign->name);
        if (it == named_values.end()) {
            throw std::runtime_error("Assignment to undeclared variable: " + assign->name);
        }
        
        llvm::AllocaInst* alloca = dyn_cast<llvm::AllocaInst>(it->second);
        if (alloca) {
            llvm::Type* target_ty = alloca->getAllocatedType();
            if (target_ty->isStructTy()) {
                // Nullable type assignment: wrap the scalar into { i1 true, base_ty val }
                llvm::Type* base_ty = target_ty->getStructElementType(1);
                if (base_ty->isFloatingPointTy() && val->getType()->isIntegerTy()) {
                    val = builder->CreateSIToFP(val, base_ty, "cast_to_fp");
                } else if (base_ty->isIntegerTy() && val->getType()->isFloatingPointTy()) {
                    val = builder->CreateFPToSI(val, base_ty, "cast_to_si");
                } else if (base_ty->isFloatingPointTy() && val->getType()->isDoubleTy() && base_ty->isFloatTy()) {
                    val = builder->CreateFPCast(val, base_ty, "cast_to_f32");
                }

                llvm::Value* nullable_val = llvm::Constant::getNullValue(target_ty);
                nullable_val = builder->CreateInsertValue(nullable_val, builder->getInt1(true), 0, "assign_flag");
                nullable_val = builder->CreateInsertValue(nullable_val, val, 1, "assign_val");
                val = nullable_val;
            } else if (target_ty->isFloatingPointTy() && val->getType()->isIntegerTy()) {
                val = builder->CreateSIToFP(val, target_ty, "cast_to_fp");
            } else if (target_ty->isIntegerTy() && val->getType()->isFloatingPointTy()) {
                val = builder->CreateFPToSI(val, target_ty, "cast_to_si");
            }
        }
        
        builder->CreateStore(val, it->second);
        return val;
    }

    if (auto bin = dynamic_cast<AST::BinaryExpr*>(expr)) {
        llvm::Value* l = codegen_expr(bin->left.get());
        llvm::Value* r = codegen_expr(bin->right.get());

        if (!l || !r) return nullptr;

        const std::string& op = bin->op;

        if ((op == "==" || op == "!=") && (l->getType()->isStructTy() || r->getType()->isStructTy())) {
            // Extract the i1 presence flag (index 0) from the nullable struct
            if (l->getType()->isStructTy() && isa<llvm::ConstantPointerNull>(r)) {
                l = builder->CreateExtractValue(l, 0, "null_check_flag");
                r = builder->getInt1(false);
            } else if (r->getType()->isStructTy() && isa<llvm::ConstantPointerNull>(l)) {
                r = builder->CreateExtractValue(r, 0, "null_check_flag");
                l = builder->getInt1(false);
            }
        }

        if (op == "..") {
            llvm::Function* concat_fn = module->getFunction("io_yuri_str_concat");
            if (!concat_fn) {
                auto* char_ptr_ty = llvm::PointerType::get(*context, 0);
                llvm::FunctionType* fn_type = llvm::FunctionType::get(char_ptr_ty, {char_ptr_ty, char_ptr_ty}, false);
                concat_fn = llvm::Function::Create(fn_type, llvm::Function::ExternalLinkage, "io_yuri_str_concat", module.get());
            }
            return builder->CreateCall(concat_fn, {l, r}, "concat_tmp");
        }

        bool is_float = l->getType()->isFloatingPointTy() || r->getType()->isFloatingPointTy();

        if (is_float) {
            if (l->getType()->isIntegerTy()) {
                l = builder->CreateSIToFP(l, builder->getDoubleTy(), "l_cast_fp");
            }
            if (r->getType()->isIntegerTy()) {
                r = builder->CreateSIToFP(r, builder->getDoubleTy(), "r_cast_fp");
            }
        }

        if (op == "+") return is_float ? builder->CreateFAdd(l, r, "faddtmp") : builder->CreateAdd(l, r, "addtmp");
        if (op == "-") return is_float ? builder->CreateFSub(l, r, "fsubtmp") : builder->CreateSub(l, r, "subtmp");
        if (op == "*") return is_float ? builder->CreateFMul(l, r, "fmultmp") : builder->CreateMul(l, r, "multmp");
        if (op == "/") return is_float ? builder->CreateFDiv(l, r, "fdivtmp") : builder->CreateSDiv(l, r, "idivtmp");
        if (op == "%") return builder->CreateSRem(l, r, "modtmp");
        if (op == "&") return builder->CreateAnd(l, r, "bandtmp");
        if (op == "|") return builder->CreateOr(l, r, "bortmp");
        if (op == "^") return builder->CreateXor(l, r, "bxortmp");
        if (op == "<<") return builder->CreateShl(l, r, "bshltmp");
        if (op == ">>") return builder->CreateLShr(l, r, "bshrtmp");
        if (op == "==") return is_float ? builder->CreateFCmpOEQ(l, r, "feqtmp") : builder->CreateICmpEQ(l, r, "ieqtmp");
        if (op == "!=") return is_float ? builder->CreateFCmpONE(l, r, "fnetmp") : builder->CreateICmpNE(l, r, "inetmp");
        if (op == "<") return is_float ? builder->CreateFCmpOLT(l, r, "flttmp") : builder->CreateICmpSLT(l, r, "ilttmp");
        if (op == "<=") return is_float ? builder->CreateFCmpOLE(l, r, "fletmp") : builder->CreateICmpSLE(l, r, "iletmp");
        if (op == ">") return is_float ? builder->CreateFCmpOGT(l, r, "fgttmp") : builder->CreateICmpSGT(l, r, "igttmp");
        if (op == ">=") return is_float ? builder->CreateFCmpOGE(l, r, "fgetmp") : builder->CreateICmpSGE(l, r, "igetmp");

        throw std::runtime_error("Unsupported binary operator: " + op);
    }

    if (auto call = dynamic_cast<AST::CallExpr*>(expr)) {
        if (call->module.empty() && call->method == "__print__" || call->method == "write") {
            if (call->arguments.empty()) return nullptr;

            llvm::Value* val = codegen_expr(call->arguments[0].get());
            
            if (call->method == "write" || call->method == "__print__") {
                if (val->getType()->isFloatingPointTy() || val->getType()->isIntegerTy()) {
                    llvm::AllocaInst* temp_alloc = builder->CreateAlloca(val->getType(), nullptr, "io_temp");
                    builder->CreateStore(val, temp_alloc);
                    val = temp_alloc;
                }
            }
            llvm::Module* mod_ptr = builder->GetInsertBlock()->getModule();
            llvm::Function* printf_fn = mod_ptr->getFunction("printf");
            if (!printf_fn) {
                printf_fn = declare_printf(mod_ptr, *context);
            }

            llvm::Constant* format_str = nullptr;
            if (val->getType()->isFloatingPointTy()) {
                format_str = builder->CreateGlobalStringPtr("%f\n");
            } else if (val->getType()->isPointerTy()) {
                format_str = builder->CreateGlobalStringPtr("%p (array/pointer)\n");
            } else {
                format_str = builder->CreateGlobalStringPtr("%lld\n");
            }

            return builder->CreateCall(printf_fn, {format_str, val});
        }
        else {
            std::vector<llvm::Value*> args;
            for (const auto& arg : call->arguments) {
                llvm::Value* arg_val = codegen_expr(arg.get());
                // If a nullable struct is passed to a regular function/native call expecting a scalar, unwrap it
                if (arg_val && arg_val->getType()->isStructTy()) {
                    arg_val = builder->CreateExtractValue(arg_val, 1, "arg_unwrapped");
                }
                args.push_back(arg_val);
            }

            llvm::Function* callee = get_or_resolve_function(call, args);
            if (!callee) {
                throw std::runtime_error("Unknown function call: " + call->method);
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
            auto expr_node = dynamic_cast<Yuri::AST::Expr*>(ret->value.get());
            llvm::Value* ret_val = codegen_expr(expr_node);
            
            if (ret_type_llvm->isFloatingPointTy() && ret_val->getType()->isIntegerTy()) {
                ret_val = builder->CreateSIToFP(ret_val, ret_type_llvm, "ret_cast_fp");
            } else if (ret_type_llvm->isIntegerTy() && ret_val->getType()->isFloatingPointTy()) {
                ret_val = builder->CreateFPToSI(ret_val, ret_type_llvm, "ret_cast_si");
            }

            builder->CreateRet(ret_val);
        } else {
            builder->CreateRetVoid();
        }
        has_terminator = true;
    } else if (auto expr = dynamic_cast<AST::Expr*>(stmt_node)) {
        codegen_expr(expr);
    } else if (auto decl = dynamic_cast<AST::VarDecl*>(stmt_node)) {
        llvm::Type* llvm_type = (decl->type == "auto" || decl->type.empty()) ? 
            builder->getInt64Ty() : get_llvm_type(decl->type);

        llvm::AllocaInst* alloca = builder->CreateAlloca(llvm_type, nullptr, decl->name.c_str());
        
        if (auto null_init = dynamic_cast<AST::NullPtrExpr*>(decl->initializer.get())) {
            if (llvm_type->isStructTy()) {
                llvm::Constant* null_struct_const = llvm::Constant::getNullValue(llvm_type);
                builder->CreateStore(null_struct_const, alloca);
                named_values[decl->name] = alloca;
                return;
            }
        }

        llvm::Value* init_val = codegen_expr(decl->initializer.get());
        if (decl->type.empty() || decl->type == "auto") {
            if (init_val) llvm_type = init_val->getType();
        }

        if (init_val) {
            if (llvm_type->isFloatingPointTy() && init_val->getType()->isIntegerTy()) {
                init_val = builder->CreateSIToFP(init_val, llvm_type, "cast_to_fp");
            } else if (llvm_type->isIntegerTy() && init_val->getType()->isFloatingPointTy()) {
                init_val = builder->CreateFPToSI(init_val, llvm_type, "cast_to_si");
            }
            builder->CreateStore(init_val, alloca);
        }
        named_values[decl->name] = alloca;
    } else if (auto if_stmt = dynamic_cast<AST::IfStmt*>(stmt_node)) {
        llvm::Value* cond_val = codegen_expr(if_stmt->condition.get());
        if (!cond_val->getType()->isIntegerTy(1)) {
            cond_val = builder->CreateICmpNE(cond_val, builder->getInt64(0), "ifcond");
        }

        llvm::Function* parent_fn = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* then_bb = llvm::BasicBlock::Create(*context, "if_then", parent_fn);
        llvm::BasicBlock* else_bb = llvm::BasicBlock::Create(*context, "if_else", parent_fn);
        llvm::BasicBlock* merge_bb = llvm::BasicBlock::Create(*context, "if_merge", parent_fn);

        bool has_else = !if_stmt->else_branch.empty();
        builder->CreateCondBr(cond_val, then_bb, has_else ? else_bb : merge_bb);

        builder->SetInsertPoint(then_bb);
        bool then_terminated = false;
        for (const auto& s : if_stmt->then_branch) {
            codegen_stmt(s.get(), ret_type_llvm, then_terminated);
        }
        if (!then_terminated && !builder->GetInsertBlock()->getTerminator()) {
            builder->CreateBr(merge_bb);
        }

        if (has_else) {
            builder->SetInsertPoint(else_bb);
            bool else_terminated = false;
            for (const auto& s : if_stmt->else_branch) {
                codegen_stmt(s.get(), ret_type_llvm, else_terminated);
            }
            if (!else_terminated && !builder->GetInsertBlock()->getTerminator()) {
                builder->CreateBr(merge_bb);
            }
        }

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

            llvm::Type* ret_type_llvm = builder->getInt64Ty();
            if (fn->ret_type == "void") {
                ret_type_llvm = builder->getVoidTy();
            } else if (!fn->ret_type.empty() && fn->ret_type != "auto") {
                ret_type_llvm = get_llvm_type(fn->ret_type);
            } else {
                bool inferred_float = false;
                for (const auto& param : fn->params) {
                    if (param.second == "float" || param.second == "double") {
                        inferred_float = true;
                        break;
                    }
                }
                ret_type_llvm = inferred_float ? builder->getDoubleTy() : builder->getInt64Ty();
            }

            llvm::FunctionType* fn_type = llvm::FunctionType::get(ret_type_llvm, arg_types, false);
            llvm::Function* function = llvm::Function::Create(fn_type, llvm::Function::ExternalLinkage, fn->name, module.get());

            function->addFnAttr(llvm::Attribute::NoInline);

            size_t idx = 0;
            for (auto& arg : function->args()) {
                arg.setName(fn->params[idx++].first);
            }

            llvm::BasicBlock* bb = llvm::BasicBlock::Create(*context, "entry", function);
            builder->SetInsertPoint(bb);

            named_values.clear();
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
                if (ret_type_llvm->isVoidTy()) {
                    builder->CreateRetVoid();
                } else if (ret_type_llvm->isFloatingPointTy()) {
                    builder->CreateRet(llvm::ConstantFP::get(*context, llvm::APFloat(0.0)));
                } else {
                    builder->CreateRet(builder->getInt64(0));
                }
            }

            llvm::verifyFunction(*function);
        }
    }
}

ffi_type* CodeGenerator::get_ffi_type(const std::string& type_str) {
    if (!type_str.empty() && type_str.back() == '?') {
        std::string base_type_str = type_str.substr(0, type_str.length() - 1);
        ffi_type* base_ffi = get_ffi_type(base_type_str);
        
        auto* struct_ffi = new ffi_type;
        struct_ffi->size = 0;
        struct_ffi->alignment = 0;
        struct_ffi->type = FFI_TYPE_STRUCT;
        
        ffi_type** elements = new ffi_type*[3];
        elements[0] = &ffi_type_sint8;
        elements[1] = base_ffi;
        elements[2] = nullptr;
        
        struct_ffi->elements = elements;
        return struct_ffi;
    }

    if (type_str == "float" || type_str == "double" || type_str == "f64") return &ffi_type_double;
    if (type_str == "float32" || type_str == "f32") return &ffi_type_float;
    if (type_str == "int" || type_str == "i64" || type_str == "i32") return &ffi_type_sint64;
    if (type_str == "bool" || type_str == "i1") return &ffi_type_sint8;
    
    if (!type_str.empty() && (type_str.back() == '*' || type_str == "string" || type_str == "str")) {
        return &ffi_type_pointer;
    }

    return &ffi_type_pointer;
}

void CodeGenerator::exec(const std::vector<std::string>& raw_args) {
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    llvm::InitializeNativeTargetAsmParser();


    std::error_code ec;
    llvm::raw_fd_ostream dest("output.ll", ec, llvm::sys::fs::OF_None);
    if (!ec) {
        module->print(dest, nullptr);
        dest.flush();
    } else {
        Logger::log(Subsystem::Codegen, LogLevel::Warning, "Failed to write LLVM IR to file: " + ec.message());
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
        native_symbols[ES.intern(name)] = llvm::orc::ExecutorSymbolDef(
            llvm::orc::ExecutorAddr::fromPtr(ptr), 
            llvm::JITSymbolFlags::Exported
        );
    }
    cantFail(MainDylib.define(llvm::orc::absoluteSymbols(native_symbols)));

    cantFail(JIT->addIRModule(llvm::orc::ThreadSafeModule(std::move(module), std::move(context))));

    auto sym = JIT->lookup(target_entry_function);
    if (!sym) {
        throw std::runtime_error("Failed to resolve entry symbol '" + target_entry_function + "'.");
    }

    void* fn_ptr = sym->toPtr<void*>();
    if (!fn_ptr) {
        throw std::runtime_error("Resolved entry symbol '" + target_entry_function + "' evaluated to a null address.");
    }

    int num_args = entry_param_count;
    std::vector<ffi_type*> ffi_arg_types;
    std::vector<void*> ffi_arg_values;

    for (int i = 0; i < num_args; ++i) {
        std::string p_type = entry_param_types[i];
        ffi_type* f_type = get_ffi_type(p_type);
        ffi_arg_types.push_back(f_type);

        void* val_storage = std::malloc(f_type->size ? f_type->size : 16);
        std::memset(val_storage, 0, f_type->size ? f_type->size : 16);
        std::string arg_str = (i < raw_args.size()) ? raw_args[i] : "1.0";
        
        if (!p_type.empty() && p_type.back() == '?') {
            bool is_null = (arg_str == "nullptr" || arg_str == "null");
            *static_cast<int8_t*>(val_storage) = is_null ? 0 : 1;
            
            std::string base_p_type = p_type.substr(0, p_type.length() - 1);
            char* val_ptr = static_cast<char*>(val_storage) + 8;
            
            if (!is_null) {
                if (base_p_type == "float" || base_p_type == "double" || base_p_type == "f64") {
                    *reinterpret_cast<double*>(val_ptr) = std::stod(arg_str);
                } else {
                    *reinterpret_cast<int64_t*>(val_ptr) = std::stoll(arg_str, nullptr, 0);
                }
            }
        } else {
            if (f_type == &ffi_type_double) {
                *static_cast<double*>(val_storage) = std::stod(arg_str);
            } else if (f_type == &ffi_type_float) {
                *static_cast<float*>(val_storage) = std::stof(arg_str);
            } else if (f_type == &ffi_type_pointer) {
                *static_cast<uintptr_t*>(val_storage) = std::stoull(arg_str, nullptr, 0);
            } else {
                *static_cast<int64_t*>(val_storage) = std::stoll(arg_str, nullptr, 0);
            }
        }
        
        ffi_arg_values.push_back(val_storage);
    }

    ffi_type* ret_ffi_type = entry_returns_void ? &ffi_type_void : &ffi_type_double;

    ffi_cif cif;
    ffi_status status = ffi_prep_cif(&cif, FFI_DEFAULT_ABI, ffi_arg_types.size(), ret_ffi_type, ffi_arg_types.data());
    if (status != FFI_OK) {
        throw std::runtime_error("Failed to prepare libffi CIF for dynamic execution.");
    }

    double result_buffer = 0.0;
    ffi_call(&cif, FFI_FN(fn_ptr), &result_buffer, ffi_arg_values.data());

    for (void* ptr : ffi_arg_values) {
        std::free(ptr);
    }

    if (!entry_returns_void) {
        std::cout << "Program returned: " << result_buffer << "\n";
    }
}

} // namespace Yuri