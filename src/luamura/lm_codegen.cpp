#include "lm_codegen.h"

#include <llvm/ADT/STLExtras.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/GlobalVariable.h>

#include <string>

extern "C" {
#include "lopnames.h"
}

namespace luamura {

Compiler::Compiler(llvm::Module& module, llvm::IRBuilder<>& builder)
    : module(module), context(module.getContext()), builder(builder),
      ptr_ty(builder.getPtrTy()), i32_ty(builder.getInt32Ty()),
      i64_ty(builder.getInt64Ty()), aot_function_ty(llvm::FunctionType::get(
                                        ptr_ty, {ptr_ty, ptr_ty}, false)),
      op_handler_ty(llvm::FunctionType::get(
          ptr_ty, {ptr_ty, ptr_ty, ptr_ty, i32_ty}, false)) {}

BlockCompiler::BlockCompiler(const Compiler& compiler, const Proto& proto,
                             const Frame& frame)
    : Compiler(compiler), proto(proto), frame(frame) {}

llvm::Value* BlockCompiler::compile(const CFG::Block& block) {
    llvm::Value* next = nullptr;
    for (PC pc = block.start; pc < block.end; pc++)
        next = project_op_handler(pc);
    return next;
}

llvm::Value* BlockCompiler::project_op_handler(PC pc) {
    Instruction i = proto.code[pc];
    // the interpreter's pc after fetching i
    builder.CreateStore(
        builder.CreateConstInBoundsGEP1_32(i32_ty, frame.code, pc + 1),
        frame.pc);
    auto* call = builder.CreateCall(
        module.getOrInsertFunction(std::string("luaV_OP_") +
                                       opnames[GET_OPCODE(i)],
                                   op_handler_ty),
        {frame.L, frame.ci, frame.pc, builder.getInt32(i)});
    call->addFnAttr(llvm::Attribute::AlwaysInline);
    return call;
}

FunctionCompiler::FunctionCompiler(const Compiler& compiler,
                                   const Proto& proto)
    : Compiler(compiler), proto(proto), cfg(&proto) {}

llvm::BasicBlock* FunctionCompiler::create_block(const llvm::Twine& name) {
    return llvm::BasicBlock::Create(context, name, function);
}

llvm::Function* FunctionCompiler::compile(const llvm::Twine& name) {
    function = llvm::Function::Create(
        aot_function_ty, llvm::GlobalValue::InternalLinkage, name, module);
    frame.L = function->getArg(0);
    frame.ci = function->getArg(1);

    auto* entry = create_block();
    interpret = create_block();
    // named for reading the IR against the bytecode
    for (const CFG::Block& block : cfg.blocks)
        llvm_blocks.push_back(
            create_block("pc" + llvm::Twine(block.start)));

    builder.SetInsertPoint(interpret);
    builder.CreateRet(frame.ci);
    builder.SetInsertPoint(entry);
    enter();

    BlockCompiler block_compiler(*this, proto, frame);
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
    frame.code = builder.CreateCall(
        module.getOrInsertFunction("lm_code", ptr_ty, ptr_ty), {frame.ci});
    auto* saved = builder.CreateCall(
        module.getOrInsertFunction("lm_resume", i32_ty, ptr_ty),
        {frame.ci});
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
        return after_call(next, llvm_blocks[block.successors.front()]);
    case OP_TAILCALL:
        return tail_call(next);
    case OP_RETURN:
    case OP_RETURN0:
    case OP_RETURN1:
        builder.CreateRet(next);
        return;
    default:
        return to_successors(block);
    }
}

// The instruction left in pc which successor to continue at.
void FunctionCompiler::to_successors(const CFG::Block& block) {
    const std::vector<CFG::BlockIndex>& targets = block.successors;
    if (targets.size() == 1) {
        builder.CreateBr(llvm_blocks[targets.front()]);
        return;
    }
    auto* moved = builder.CreatePtrDiff(
        i32_ty, builder.CreateLoad(ptr_ty, frame.pc), frame.code);
    auto* to = builder.CreateSwitch(moved, llvm_blocks[targets.front()]);
    for (CFG::BlockIndex t : llvm::drop_begin(targets))
        to->addCase(builder.getInt64(cfg.blocks[t].start), llvm_blocks[t]);
}

// A Lua callee is left for luaV_execute to run. A C function returned
// here, and may have set a hook, such as with debug.sethook, which only
// the interpreter runs.
void FunctionCompiler::after_call(llvm::Value* next,
                                  llvm::BasicBlock* successor) {
    auto* returned = create_block();
    auto* called = create_block();
    builder.CreateCondBr(builder.CreateIsNull(next), returned, called);
    builder.SetInsertPoint(called);
    builder.CreateRet(next);
    builder.SetInsertPoint(returned);

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
void FunctionCompiler::tail_call(llvm::Value* next) {
    auto* callee = create_block();
    auto* compiled = create_block();
    auto* done = create_block();
    builder.CreateCondBr(builder.CreateICmpEQ(next, frame.ci), callee,
                         done);
    builder.SetInsertPoint(done);
    builder.CreateRet(next);

    builder.SetInsertPoint(callee);
    auto* native = builder.CreateCall(
        module.getOrInsertFunction("lm_compiled", ptr_ty, ptr_ty, ptr_ty),
        {frame.L, frame.ci});
    builder.CreateCondBr(builder.CreateIsNull(native), done, compiled);
    builder.SetInsertPoint(compiled);
    auto* call =
        builder.CreateCall(aot_function_ty, native, {frame.L, frame.ci});
    call->setTailCallKind(llvm::CallInst::TCK_MustTail);
    builder.CreateRet(call);
}

void ModuleCompiler::compile(const Proto& main, const std::string& chunk) {
    compile_function(main);
    define("lm_chunk",
           llvm::ConstantDataArray::getString(context, chunk, false));
    define("lm_chunk_size", llvm::ConstantInt::get(i64_ty, chunk.size()));
    define("lm_functions",
           llvm::ConstantArray::get(
               llvm::ArrayType::get(ptr_ty, functions.size()), functions));
    define("lm_nfunctions",
           llvm::ConstantInt::get(i32_ty, functions.size()));
}

void ModuleCompiler::define(const char* name, llvm::Constant* value) {
    new llvm::GlobalVariable(module, value->getType(), true,
                             llvm::GlobalValue::ExternalLinkage, value,
                             name);
}

// The runtime attaches the functions to the chunk's Protos in preorder.
void ModuleCompiler::compile_function(const Proto& proto) {
    functions.push_back(
        FunctionCompiler(*this, proto)
            .compile("lm_function_" + llvm::Twine(functions.size())));
    for (const Proto* nested : llvm::ArrayRef(proto.p, proto.sizep))
        compile_function(*nested);
}

std::unique_ptr<llvm::Module> compile_module(llvm::LLVMContext& context,
                                             const Proto* main,
                                             const std::string& chunk) {
    auto module = std::make_unique<llvm::Module>("luamura", context);
    llvm::IRBuilder<> builder(context);
    ModuleCompiler(*module, builder).compile(*main, chunk);
    return module;
}

} // namespace luamura
