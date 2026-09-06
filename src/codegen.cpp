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

// Robust signal handler with traceback integration
void yuri_sigsegv_handler(int sig, siginfo_t* info, void* context) {
    std::cerr << "\n[CRITICAL ERROR] Segmentation fault (SIGSEGV) intercepted by runtime.\n";
    std::cerr << "Faulting memory address: " << info->si_addr << "\n";

    void* faulting_ip = nullptr;
    auto* uc = static_cast<ucontext_t*>(context);
    #if defined(__x86_64__)
    faulting_ip = reinterpret_cast<void*>(uc->uc_mcontext.gregs[REG_RIP]);
    #elif defined(__aarch64__)
    faulting_ip = reinterpret_cast<void*>(uc->uc_mcontext.pc);
    #endif

    std::cerr << "Faulting Instruction Pointer (RIP): " << faulting_ip << "\n\n";

    void* trace_stack[32];
    int trace_size = backtrace(trace_stack, 32);

    std::cerr << "Execution Backtrace:\n";
    char exe_path[1024];
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (len != -1) {
        exe_path[len] = '\0';
        for (int i = 0; i < trace_size; ++i) {
            char cmd[256];
            std::snprintf(cmd, sizeof(cmd), "addr2line -e %s -f -p %p", exe_path, trace_stack[i]);
            FILE* pipe = popen(cmd, "r");
            if (pipe) {
                char buffer[256];
                if (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
                    std::cerr << "  #" << i << " " << buffer;
                }
                pclose(pipe);
            }
        }
    } else {
        backtrace_symbols_fd(trace_stack, trace_size, STDERR_FILENO);
    }

    std::_Exit(1);
}

namespace Yuri {

struct NullableFloatABI {
    bool is_null;
    double val;
};

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

    if (auto* sig = Yuri::Registry::get_native_sig(symbol_name)) {
        ret_type = get_llvm_type(sig->ret_type);
        for (const auto& p_str : sig->param_types) {
            param_types.push_back(get_llvm_type(p_str));
        }
    } else {
        for (auto* arg : args) {
            param_types.push_back(arg->getType());
        }
        if (!args.empty()) {
            ret_type = args[0]->getType();
        }
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

        const std::string& op = bin->op;
        bool is_float = l->getType()->isFloatingPointTy() || r->getType()->isFloatingPointTy();

        if (is_float) {
            if (l->getType()->isIntegerTy()) l = builder->CreateSIToFP(l, builder->getDoubleTy(), "l_cast_fp");
            if (r->getType()->isIntegerTy()) r = builder->CreateSIToFP(r, builder->getDoubleTy(), "r_cast_fp");
        }

        if (op == "+") return is_float ? builder->CreateFAdd(l, r, "faddtmp") : builder->CreateAdd(l, r, "addtmp");
        if (op == "-") return is_float ? builder->CreateFSub(l, r, "fsubtmp") : builder->CreateSub(l, r, "subtmp");
        if (op == "*") return is_float ? builder->CreateFMul(l, r, "fmultmp") : builder->CreateMul(l, r, "multmp");
        if (op == "/") return is_float ? builder->CreateFDiv(l, r, "fdivtmp") : builder->CreateSDiv(l, r, "idivtmp");
        if (op == "==") return is_float ? builder->CreateFCmpOEQ(l, r, "feqtmp") : builder->CreateICmpEQ(l, r, "ieqtmp");
        if (op == "!=") return is_float ? builder->CreateFCmpONE(l, r, "fnetmp") : builder->CreateICmpNE(l, r, "inetmp");
        if (op == "<") return is_float ? builder->CreateFCmpOLT(l, r, "flttmp") : builder->CreateICmpSLT(l, r, "ilttmp");
        if (op == ">") return is_float ? builder->CreateFCmpOGT(l, r, "fgttmp") : builder->CreateICmpSGT(l, r, "igttmp");

        throw std::runtime_error("Unsupported binary operator: " + op);
    }
    if (auto call = dynamic_cast<AST::CallExpr*>(expr)) {
        if (call->module.empty() && (call->method == "__print__" || call->method == "io_write" || call->method == "write")) {
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
                if (args[i]->getType()->isStructTy() && param_tys[i]->isFloatingPointTy()) {
                    args[i] = builder->CreateExtractValue(args[i], {1}, "unwrap_nullable_val");
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
    } else if (auto expr = dynamic_cast<AST::Expr*>(stmt_node)) {
        codegen_expr(expr);
    } else if (auto decl = dynamic_cast<AST::VarDecl*>(stmt_node)) {
        llvm::Value* init_val = decl->initializer ? codegen_expr(decl->initializer.get()) : nullptr;
        
        llvm::Type* llvm_type = nullptr;
        if (!decl->type.empty() && decl->type != "auto") {
            llvm_type = get_llvm_type(decl->type);
        } else if (init_val) {
            llvm_type = init_val->getType();
        } else {
            llvm_type = builder->getInt64Ty();
        }

        llvm::AllocaInst* alloca = builder->CreateAlloca(llvm_type, nullptr, decl->name.c_str());
        if (init_val) {
            builder->CreateStore(init_val, alloca);
        }
        named_values[decl->name] = alloca;
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
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(struct sigaction));
    sa.sa_flags = SA_SIGINFO;
    sa.sa_sigaction = yuri_sigsegv_handler;
    sigaction(SIGSEGV, &sa, NULL);

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

    if (entry_returns_void) {
        if (entry_param_count == 1 && entry_param_types[0] == "float?") {
            using EntryFunc1 = void(*)(NullableFloatABI);
            auto entry_fn = reinterpret_cast<EntryFunc1>(fn_ptr);
            NullableFloatABI default_x{true, 0.0};
            entry_fn(default_x);
        } else {
            using EntryFuncVoid = void(*)();
            auto entry_fn = reinterpret_cast<EntryFuncVoid>(fn_ptr);
            entry_fn();
        }
    } else {
        if (entry_param_count == 1 && entry_param_types[0] == "float?") {
            using EntryFunc1 = int64_t(*)(NullableFloatABI);
            auto entry_fn = reinterpret_cast<EntryFunc1>(fn_ptr);
            NullableFloatABI default_x{true, 0.0};
            int64_t result = entry_fn(default_x);
            std::cout << "Program returned: " << result << "\n";
        } else {
            using EntryFuncVal = int64_t(*)();
            auto entry_fn = reinterpret_cast<EntryFuncVal>(fn_ptr);
            int64_t result = entry_fn();
            std::cout << "Program returned: " << result << "\n";
        }
    }
}

} // namespace Yuri