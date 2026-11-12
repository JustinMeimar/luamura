#include "lm_runtime.h"

namespace luamura {

RuntimeTypes::RuntimeTypes(llvm::LLVMContext& context) {
    ptr = llvm::PointerType::getUnqual(context);
    i8 = llvm::Type::getInt8Ty(context);
    i32 = llvm::Type::getInt32Ty(context);
    instruction = i32;
    aot_function = llvm::FunctionType::get(ptr, {ptr, ptr}, false);
    // L, ci, the address of pc, the instruction, the one after it, base
    op_handler = llvm::FunctionType::get(
        ptr, {ptr, ptr, ptr, instruction, instruction, ptr}, false);
}

RuntimeFunctions::RuntimeFunctions(llvm::Module& module,
                                   const RuntimeTypes& types) {
    lm_code = module.getOrInsertFunction("lm_code", types.ptr, types.ptr);
    lm_resume =
        module.getOrInsertFunction("lm_resume", types.i32, types.ptr);
    lm_compiled = module.getOrInsertFunction("lm_compiled", types.ptr,
                                             types.ptr, types.ptr);
}

} // namespace luamura
