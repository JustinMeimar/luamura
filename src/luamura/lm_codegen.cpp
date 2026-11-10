#include "lm_codegen.h"

#include <llvm/ADT/STLExtras.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/GlobalVariable.h>

#include <string>
#include <cstdlib>

extern "C" {
#include "lopnames.h"
}

namespace luamura {

BaselineCompiler::BaselineCompiler(llvm::Module& module,
                                   llvm::IRBuilder<>& builder)
    : module(module), context(module.getContext()), builder(builder),
      ptr_ty(builder.getPtrTy()), i32_ty(builder.getInt32Ty()),
      i64_ty(builder.getInt64Ty()), instruction_ty(builder.getInt32Ty()),
      aot_function_ty(
          llvm::FunctionType::get(ptr_ty, {ptr_ty, ptr_ty}, false)),
      op_handler_ty(llvm::FunctionType::get(
          ptr_ty, {ptr_ty, ptr_ty, ptr_ty, instruction_ty, instruction_ty},
          false)),
      lm_code(module.getOrInsertFunction("lm_code", ptr_ty, ptr_ty)),
      lm_resume(module.getOrInsertFunction("lm_resume", i32_ty, ptr_ty)),
      lm_compiled(module.getOrInsertFunction("lm_compiled", ptr_ty, ptr_ty,
                                             ptr_ty)) {}

// Returns what the block's last instruction returned.
llvm::Value* FunctionCompiler::lowerBlock(const Block& block) {
    llvm::Value* next = nullptr;
    for (uint32_t id : block.instructions) {
        const Node& node = ir.nodes[id];
        // values live in the Lua stack, which needs no reading
        if (node.kind == Node::Kind::Bytecode)
            next = projectOpHandler(node);
    }
    return next;
}

llvm::Value* FunctionCompiler::projectOpHandler(const Node& node) {
    PC pc = node.pc;
    Instruction i = node.bytecode;
    // the jump after i, if i is a test
    Instruction ni = pc + 1 < proto.sizecode ? proto.code[pc + 1] : 0;
    // the interpreter's pc after fetching i
    builder.CreateStore(builder.CreateConstInBoundsGEP1_32(
                            instruction_ty, frame.code, pc + 1),
                        frame.pc);
    auto* call = builder.CreateCall(
        module.getOrInsertFunction(std::string("luaV_OP_") +
                                       opnames[GET_OPCODE(i)],
                                   op_handler_ty),
        {frame.L, frame.ci, frame.pc, builder.getInt32(i),
         builder.getInt32(ni)});
    call->addFnAttr(llvm::Attribute::AlwaysInline);
    return call;
}

FunctionCompiler::FunctionCompiler(const BaselineCompiler& compiler,
                                   const LmIR& ir)
    : BaselineCompiler(compiler), proto(ir.proto), ir(ir) {}

llvm::BasicBlock* FunctionCompiler::createBlock(const llvm::Twine& name) {
    return llvm::BasicBlock::Create(context, name, function);
}

llvm::Function* FunctionCompiler::compile(const llvm::Twine& name) {
    function = llvm::Function::Create(
        aot_function_ty, llvm::GlobalValue::InternalLinkage, name, module);
    frame.L = function->getArg(0);
    frame.ci = function->getArg(1);

    // named for reading the IR against the bytecode
    for (const auto& block : ir.blocks)
        llvm_blocks.push_back(
            block->start < 0
                ? createBlock()
                : createBlock("pc" + llvm::Twine(block->start)));
    interpret = createBlock();

    builder.SetInsertPoint(interpret);
    builder.CreateRet(frame.ci);
    builder.SetInsertPoint(llvm_blocks.front());
    enter();

    for (const auto& block : ir.blocks) {
        builder.SetInsertPoint(llvm_blocks[block->id]);
        terminate(*block, lowerBlock(*block));
    }
    return function;
}

void FunctionCompiler::enter() {
    frame.pc = builder.CreateAlloca(ptr_ty);
    frame.code = builder.CreateCall(lm_code, {frame.ci});
}

// Continues at the saved pc: the function's start, or after a call.
void FunctionCompiler::resume(const Block& block) {
    auto* saved = builder.CreateCall(lm_resume, {frame.ci});
    auto* to = builder.CreateSwitch(saved, interpret);
    for (const Block* target : block.successors)
        to->addCase(builder.getInt32(target->start),
                    llvm_blocks[target->id]);
}

// Continues where the interpreter would after the block's last
// instruction, which returned next.
void FunctionCompiler::terminate(const Block& block, llvm::Value* next) {
    switch (block.terminator) {
    case Block::Terminator::Resume:
        return resume(block);
    case Block::Terminator::Branch:
        return toSuccessors(block);
    case Block::Terminator::Call:
        return afterCall(next, llvm_blocks[block.successors.front()->id]);
    case Block::Terminator::TailCall:
        return tailCall(next);
    case Block::Terminator::Return:
        builder.CreateRet(next);
        return;
    }
}

// The instruction left in pc which successor to continue at.
void FunctionCompiler::toSuccessors(const Block& block) {
    const std::vector<Block*>& targets = block.successors;
    if (targets.size() == 1) {
        builder.CreateBr(llvm_blocks[targets.front()->id]);
        return;
    }
    auto* moved = builder.CreatePtrDiff(
        instruction_ty, builder.CreateLoad(ptr_ty, frame.pc), frame.code);
    auto* to =
        builder.CreateSwitch(moved, llvm_blocks[targets.front()->id]);
    for (const Block* target : llvm::drop_begin(targets))
        to->addCase(builder.getInt64(target->start),
                    llvm_blocks[target->id]);
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

void ModuleCompiler::compile(const Proto& main, const std::string& chunk) {
    compileFunction(main);
    define("lm_chunk",
           llvm::ConstantDataArray::getString(context, chunk, false));
    define("lm_chunk_size", builder.getInt64(chunk.size()));
    define("lm_functions",
           llvm::ConstantArray::get(
               llvm::ArrayType::get(ptr_ty, functions.size()), functions));
    define("lm_nfunctions", builder.getInt32(functions.size()));
}

void ModuleCompiler::define(const char* name, llvm::Constant* value) {
    new llvm::GlobalVariable(module, value->getType(), true,
                             llvm::GlobalValue::ExternalLinkage, value,
                             name);
}

// Named as the LLVM function, to read the graph against the .ll.
static void writeGraph(LmIR& ir, const std::string& name) {
    std::error_code ec;
    llvm::raw_fd_ostream out(name + ".dot", ec);
    if (ec) {
        llvm::errs() << "luamura: cannot write " << name
                     << ".dot: " << ec.message() << "\n";
        return;
    }
    ir.writeGraph(out, name);
}

// The runtime attaches the functions to the chunk's Protos in preorder.
void ModuleCompiler::compileFunction(const Proto& proto) {
    LmIR ir(proto);
    passes.run(ir);
    std::string name = "lm_function_" + std::to_string(functions.size());
    if (std::getenv("LUAMURA_DUMP_IR"))
        ir.print(llvm::errs());
    if (std::getenv("LUAMURA_DUMP_DOT"))
        writeGraph(ir, name);
    functions.push_back(FunctionCompiler(*this, ir).compile(name));
    for (const Proto* nested : llvm::ArrayRef(proto.p, proto.sizep))
        compileFunction(*nested);
}

std::unique_ptr<llvm::Module> compileModule(llvm::LLVMContext& context,
                                            const Proto& main,
                                            const std::string& chunk) {
    auto module = std::make_unique<llvm::Module>("luamura", context);
    llvm::IRBuilder<> builder(context);
    ModuleCompiler(*module, builder).compile(main, chunk);
    return module;
}

} // namespace luamura
