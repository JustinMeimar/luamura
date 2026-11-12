#include "lm_codegen.h"
#include "lm_pass_base_updates.h"
#include "lm_pass_type_specialization.h"
#include "lm_pass.h"
#include <llvm/ADT/STLExtras.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/GlobalVariable.h>
#include <string>
#include <cstdlib>

extern "C" {
#include "lopnames.h"
}

namespace luamura {

CodeGenerator::CodeGenerator(llvm::Module& module,
                             const GuestBytecode& bytecode)
    : module(module), context(module.getContext()), builder(context),
      bytecode(bytecode), ptr_ty(builder.getPtrTy()),
      i8_ty(builder.getInt8Ty()), i32_ty(builder.getInt32Ty()),
      instruction_ty(builder.getInt32Ty()),
      aot_function_ty(
          llvm::FunctionType::get(ptr_ty, {ptr_ty, ptr_ty}, false)),
      op_handler_ty(llvm::FunctionType::get(
          ptr_ty,
          {ptr_ty, ptr_ty, ptr_ty, instruction_ty, instruction_ty, ptr_ty},
          false)),
      lm_code(module.getOrInsertFunction("lm_code", ptr_ty, ptr_ty)),
      lm_resume(module.getOrInsertFunction("lm_resume", i32_ty, ptr_ty)),
      lm_compiled(module.getOrInsertFunction("lm_compiled", ptr_ty, ptr_ty,
                                             ptr_ty)) {}

void CodeGenerator::lowerBytecode(const Node& node) {
    next = projectOpHandler(node);
}

void CodeGenerator::updateBase() {
    auto* func = builder.CreateLoad(
        ptr_ty, builder.CreateInBoundsPtrAdd(
                    frame.ci, builder.getInt64(offsetof(CallInfo, func))));
    builder.CreateStore(builder.CreateInBoundsPtrAdd(
                            func, builder.getInt64(sizeof(StackValue))),
                        frame.base);
}

llvm::Value* CodeGenerator::slotAddress(Slot slot, size_t field) {
    return builder.CreateInBoundsPtrAdd(
        builder.CreateLoad(ptr_ty, frame.base),
        builder.getInt64(slot * sizeof(StackValue) + field));
}

// Holds when each input has the type the hot path assumes.
void CodeGenerator::lowerGuard(const Node& node) {
    const Block& hot = *block->successors.front();
    llvm::Value* holds = builder.getTrue();
    for (const Operand& op : node.inputs) {
        auto* tag = builder.CreateLoad(
            i8_ty, slotAddress(op.slot, offsetof(TValue, tt_)));
        holds = builder.CreateAnd(
            holds, builder.CreateICmpEQ(
                       tag, builder.getInt8(hot.types.lookup(op.value))));
    }
    next = holds;
}

llvm::Value* CodeGenerator::projectOpHandler(const Node& node) {
    PC pc = node.pc;
    Instruction i = node.bytecode;
    // the jump after i, if i is a test
    Instruction ni = pc + 1 < proto->sizecode ? proto->code[pc + 1] : 0;
    // the interpreter's pc after fetching i
    builder.CreateStore(builder.CreateConstInBoundsGEP1_32(
                            instruction_ty, frame.code, pc + 1),
                        frame.pc);
    auto* call = builder.CreateCall(
        module.getOrInsertFunction(std::string("luaV_OP_") +
                                       opnames[GET_OPCODE(i)] + "_base",
                                   op_handler_ty),
        {frame.L, frame.ci, frame.pc, builder.getInt32(i),
         builder.getInt32(ni), builder.CreateLoad(ptr_ty, frame.base)});
    call->addFnAttr(llvm::Attribute::AlwaysInline);
    return call;
}

llvm::BasicBlock* CodeGenerator::createBlock(const llvm::Twine& name) {
    return llvm::BasicBlock::Create(context, name, function);
}

// The runtime attaches the functions to the chunk's Protos in preorder.
static std::string lmFunctionName(size_t index) {
    return "lm_function_" + std::to_string(index);
}

void CodeGenerator::lower(const LmModule& module) {
    for (const auto& ir : module.functions)
        lower(*ir);
    define("lm_chunk",
           llvm::ConstantDataArray::get(context, llvm::ArrayRef(bytecode)));
    define("lm_chunk_size", builder.getInt64(bytecode.size()));
    define("lm_functions",
           llvm::ConstantArray::get(
               llvm::ArrayType::get(ptr_ty, functions.size()), functions));
    define("lm_nfunctions", builder.getInt32(functions.size()));
}

void CodeGenerator::lower(const LmIR& ir) {
    proto = &ir.proto;
    function = llvm::Function::Create(
        aot_function_ty, llvm::GlobalValue::InternalLinkage,
        lmFunctionName(functions.size()), module);
    frame = {function->getArg(0), function->getArg(1)};

    // named for reading the IR against the bytecode
    llvm_blocks.clear();
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

    for (const auto& block : ir.blocks)
        lower(*block);
    functions.push_back(function);
}

void CodeGenerator::lower(const Block& block) {
    this->block = &block;
    next = nullptr;
    builder.SetInsertPoint(llvm_blocks[block.id]);
    for (uint32_t id : block.instructions)
        lower(block.parent->nodes[id]);
    lowerTerminator(block);
}

void CodeGenerator::lower(const Node& node) {
    switch (node.kind) {
    // values live in the Lua stack, which needs no reading
    case Node::Kind::FrameRead:
    case Node::Kind::Phi:
        return;
    case Node::Kind::Bytecode:
        return lowerBytecode(node);
    case Node::Kind::Guard:
        return lowerGuard(node);
    case Node::Kind::UpdateBase:
        return updateBase();
    }
}

void CodeGenerator::enter() {
    frame.pc = builder.CreateAlloca(ptr_ty);
    frame.base = builder.CreateAlloca(ptr_ty);
    frame.code = builder.CreateCall(lm_code, {frame.ci});
}

// Continues at the saved pc: the function's start, or after a call.
void CodeGenerator::resume(const Block& block) {
    auto* saved = builder.CreateCall(lm_resume, {frame.ci});
    auto* to = builder.CreateSwitch(saved, interpret);
    for (const Block* target : block.successors)
        to->addCase(builder.getInt32(target->start),
                    llvm_blocks[target->id]);
}

// Continues where the interpreter would after the block's last
// instruction, which returned next.
void CodeGenerator::lowerTerminator(const Block& block) {
    switch (block.terminator) {
    case Block::Terminator::Resume:
        return resume(block);
    case Block::Terminator::Branch:
        return toSuccessors(block);
    case Block::Terminator::Call:
        return afterCall(llvm_blocks[block.successors.front()->id]);
    case Block::Terminator::TailCall:
        return tailCall();
    case Block::Terminator::Return:
        builder.CreateRet(next);
        return;
    case Block::Terminator::Guard:
        builder.CreateCondBr(next, llvm_blocks[block.successors[0]->id],
                             llvm_blocks[block.successors[1]->id]);
        return;
    }
}

// The instruction left in pc which successor to continue at.
void CodeGenerator::toSuccessors(const Block& block) {
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
void CodeGenerator::afterCall(llvm::BasicBlock* successor) {
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
void CodeGenerator::tailCall() {
    returnIf(builder.CreateICmpNE(next, frame.ci), next);
    auto* native = builder.CreateCall(lm_compiled, {frame.L, frame.ci});
    returnIf(builder.CreateIsNull(native), next);
    auto* call =
        builder.CreateCall(aot_function_ty, native, {frame.L, frame.ci});
    call->setTailCallKind(llvm::CallInst::TCK_MustTail);
    builder.CreateRet(call);
}

void CodeGenerator::returnIf(llvm::Value* condition, llvm::Value* value) {
    auto* returns = createBlock();
    auto* continues = createBlock();
    builder.CreateCondBr(condition, returns, continues);
    builder.SetInsertPoint(returns);
    builder.CreateRet(value);
    builder.SetInsertPoint(continues);
}

void CodeGenerator::define(const char* name, llvm::Constant* value) {
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

std::unique_ptr<llvm::Module> compileModule(llvm::LLVMContext& context,
                                            const Proto& main,
                                            const GuestBytecode& bytecode) {
    LmModule ir(main);
    PassManager passes;
    passes.add(std::make_unique<TypeSpecialization>());
    passes.add(std::make_unique<BaseUpdates>());
    passes.run(ir);
    for (size_t index = 0; index < ir.functions.size(); ++index) {
        if (std::getenv("LUAMURA_DUMP_IR"))
            ir.functions[index]->print(llvm::errs());
        if (std::getenv("LUAMURA_DUMP_DOT"))
            writeGraph(*ir.functions[index], lmFunctionName(index));
    }
    auto module = std::make_unique<llvm::Module>("luamura", context);
    CodeGenerator(*module, bytecode).lower(ir);
    return module;
}

} // namespace luamura
