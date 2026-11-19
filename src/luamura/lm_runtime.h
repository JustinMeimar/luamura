#ifndef LM_RUNTIME_H
#define LM_RUNTIME_H

#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Module.h>

namespace luamura {

// The LLVM types of the runtime's C types that compiled code uses.
struct RuntimeTypes {
    explicit RuntimeTypes(llvm::LLVMContext& context);
    
    ///NOTE(Justin): There is a proliferation of *ptr a = nullptr
    ///initializations in class bodies. I'm confused why. I see
    ///how maybe it's a type that if truly undefined (malloc'd
    ///memory) it could be UB. But. still kinda ugly
    llvm::PointerType* ptr = nullptr;
    llvm::IntegerType* i8 = nullptr;
    llvm::IntegerType* i32 = nullptr;
    llvm::IntegerType* instruction = nullptr;
    // lua_AotFunction
    llvm::FunctionType* aot_function = nullptr;
    // lvm.c's function for an instruction
    llvm::FunctionType* op_handler = nullptr;
};

// lm_rt.c's functions, which compiled code calls.
struct RuntimeFunctions {
    RuntimeFunctions(llvm::Module& module, const RuntimeTypes& types);

    llvm::FunctionCallee lm_code;
    llvm::FunctionCallee lm_resume;
    llvm::FunctionCallee lm_compiled;
};

} // namespace luamura

#endif // LM_RUNTIME_H
