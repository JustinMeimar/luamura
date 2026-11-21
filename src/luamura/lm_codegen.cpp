#include "lm_codegen.h"
#include <llvm/ADT/STLExtras.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/GlobalVariable.h>
#include <string>

extern "C" {
#include "lopnames.h"
}

namespace luamura {

BlockCompiler::BlockCompiler(llvm::Module& module,
                             llvm::IRBuilder<>& builder, const Proto& proto,
                             const Frame& frame)
    : module(module), builder(builder),
      op_handler_ty(llvm::FunctionType::get(
          builder.getPtrTy(),
          {builder.getPtrTy(), builder.getPtrTy(), builder.getPtrTy(),
           builder.getInt32Ty(), builder.getInt32Ty()},
          false)),
      proto(proto), frame(frame) {}

llvm::Value* BlockCompiler::compile(const CFG::Block& block) {
    llvm::Value* next = nullptr;
    for (PC pc = block.start; pc < block.end; pc++) {
        Instruction i = proto.code[pc];
        Instruction ni = pc + 1 < proto.sizecode ? proto.code[pc + 1] : 0;
        // The interpreter advances its pc before executing an instruction.
        builder.CreateStore(builder.CreateConstInBoundsGEP1_32(
                                builder.getInt32Ty(), frame.code, pc + 1),
                            frame.pc);
        auto* call = builder.CreateCall(
            module.getOrInsertFunction(std::string("luaV_OP_") +
                                           opnames[GET_OPCODE(i)],
                                       op_handler_ty),
            {frame.L, frame.ci, frame.pc, builder.getInt32(i),
             builder.getInt32(ni)});
        call->addFnAttr(llvm::Attribute::AlwaysInline);
        next = call;
    }
    return next;
}

FunctionCompiler::FunctionCompiler(llvm::Module& module, const Proto& proto)
    : module(module), context(module.getContext()), builder(context),
      ptr_ty(builder.getPtrTy()), i32_ty(builder.getInt32Ty()),
      instruction_ty(builder.getInt32Ty()),
      aot_function_ty(
          llvm::FunctionType::get(ptr_ty, {ptr_ty, ptr_ty}, false)),
      lm_code(module.getOrInsertFunction("lm_code", ptr_ty, ptr_ty)),
      lm_resume(module.getOrInsertFunction("lm_resume", i32_ty, ptr_ty)),
      lm_compiled(module.getOrInsertFunction("lm_compiled", ptr_ty, ptr_ty,
                                             ptr_ty)),
      proto(proto), cfg(proto) {}

llvm::BasicBlock* FunctionCompiler::createBlock(const llvm::Twine& name) {
    return llvm::BasicBlock::Create(context, name, function);
}

llvm::Function* FunctionCompiler::compile(const llvm::Twine& name) {
    function = llvm::Function::Create(
        aot_function_ty, llvm::GlobalValue::InternalLinkage, name, module);
    frame.L = function->getArg(0);
    frame.ci = function->getArg(1);

    auto* entry = createBlock();
    interpret = createBlock();
    // named for reading the IR against the bytecode
    for (const CFG::Block& block : cfg.blocks)
        llvm_blocks.push_back(createBlock("pc" + llvm::Twine(block.start)));

    builder.SetInsertPoint(interpret);
    builder.CreateRet(frame.ci);
    builder.SetInsertPoint(entry);
    enter();

    BlockCompiler block_compiler(module, builder, proto, frame);
    for (auto [block, llvm_block] :
         llvm::zip_equal(cfg.blocks, llvm_blocks)) {
        builder.SetInsertPoint(llvm_block);
        terminate(block, block_compiler.compile(block));
    }
    return function;
}

// Resumes at the saved pc: the function's start, or after a call.
void FunctionCompiler::enter() {
    frame.pc = builder.CreateAlloca(ptr_ty);
    frame.code = builder.CreateCall(lm_code, {frame.ci});
    auto* saved = builder.CreateCall(lm_resume, {frame.ci});
    auto* resume = builder.CreateSwitch(saved, interpret);
    for (CFG::BlockIndex e : cfg.entries)
        resume->addCase(builder.getInt32(cfg.blocks[e].start),
                        llvm_blocks[e]);
}

// Continues where the interpreter would after the block's last
// instruction, which returned next.
void FunctionCompiler::terminate(const CFG::Block& block,
                                 llvm::Value* next) {
    switch (GET_OPCODE(proto.code[block.last()])) {
    case OP_CALL:
        return afterCall(next, llvm_blocks[block.successors.front()]);
    case OP_TAILCALL:
        return tailCall(next);
    case OP_RETURN:
    case OP_RETURN0:
    case OP_RETURN1:
        builder.CreateRet(next);
        return;
    default:
        return toSuccessors(block);
    }
}

// The instruction left in pc which successor to continue at.
void FunctionCompiler::toSuccessors(const CFG::Block& block) {
    const std::vector<CFG::BlockIndex>& targets = block.successors;
    if (targets.size() == 1) {
        builder.CreateBr(llvm_blocks[targets.front()]);
        return;
    }
    auto* moved = builder.CreatePtrDiff(
        instruction_ty, builder.CreateLoad(ptr_ty, frame.pc), frame.code);
    auto* to = builder.CreateSwitch(moved, llvm_blocks[targets.front()]);
    for (CFG::BlockIndex t : llvm::drop_begin(targets))
        to->addCase(builder.getInt64(cfg.blocks[t].start), llvm_blocks[t]);
}

// A Lua callee is left for luaV_execute to run. A C function returned
// here, and may have set a hook, such as with debug.sethook, which only
// the interpreter runs.
void FunctionCompiler::afterCall(llvm::Value* next,
                                 llvm::BasicBlock* successor) {
    returnIf(builder.CreateIsNotNull(next), next);
    // volatile, as signal handlers set it
    auto* hookmask = builder.CreateLoad(
        i32_ty,
        builder.CreateInBoundsPtrAdd(
            frame.L, builder.getInt64(offsetof(lua_State, hookmask))),
        true);
    builder.CreateCondBr(builder.CreateIsNotNull(hookmask), interpret,
                         successor);
}

// A Lua callee replaces this function in ci. Entering its native code
// as a tail call keeps the C stack from growing.
void FunctionCompiler::tailCall(llvm::Value* next) {
    returnIf(builder.CreateICmpNE(next, frame.ci), next);
    auto* native = builder.CreateCall(lm_compiled, {frame.L, frame.ci});
    returnIf(builder.CreateIsNull(native), next);
    auto* call =
        builder.CreateCall(aot_function_ty, native, {frame.L, frame.ci});
    call->setTailCallKind(llvm::CallInst::TCK_MustTail);
    builder.CreateRet(call);
}

void FunctionCompiler::returnIf(llvm::Value* condition,
                                llvm::Value* value) {
    auto* returns = createBlock();
    auto* continues = createBlock();
    builder.CreateCondBr(condition, returns, continues);
    builder.SetInsertPoint(returns);
    builder.CreateRet(value);
    builder.SetInsertPoint(continues);
}

// The runtime attaches the functions to the chunk's Protos in preorder.
static void compileFunctions(llvm::Module& module, const Proto& proto,
                             std::vector<llvm::Constant*>& functions) {
    functions.push_back(
        FunctionCompiler(module, proto)
            .compile("lm_function_" + llvm::Twine(functions.size())));
    for (const Proto* nested : llvm::ArrayRef(proto.p, proto.sizep))
        compileFunctions(module, *nested, functions);
}

std::unique_ptr<llvm::Module> compileModule(llvm::LLVMContext& context,
                                            const Proto& main,
                                            const std::string& chunk) {
    auto module = std::make_unique<llvm::Module>("luamura", context);
    llvm::IRBuilder<> builder(context);
    std::vector<llvm::Constant*> functions;
    compileFunctions(*module, main, functions);

    auto define = [&](const char* name, llvm::Constant* value) {
        new llvm::GlobalVariable(*module, value->getType(), true,
                                 llvm::GlobalValue::ExternalLinkage, value,
                                 name);
    };
    define("lm_chunk",
           llvm::ConstantDataArray::getString(context, chunk, false));
    define("lm_chunk_size", builder.getInt64(chunk.size()));
    define("lm_functions",
           llvm::ConstantArray::get(
               llvm::ArrayType::get(builder.getPtrTy(), functions.size()),
               functions));
    define("lm_nfunctions", builder.getInt32(functions.size()));
    return module;
}

} // namespace luamura
