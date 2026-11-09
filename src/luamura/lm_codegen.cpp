#include "lm_codegen.h"
#include "lm_cfg.h"

#include <llvm/ADT/STLExtras.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>

#include <string>
#include <vector>

extern "C" {
#include "lopnames.h"
#include "luamura.h"
}

// The name of a declaration, which must exist.
#define NAME(x) (static_cast<void>(sizeof(&(x))), #x)

namespace {

// What luaV_execute keeps while running a function.
struct Frame {
    llvm::Value* L = nullptr;
    llvm::Value* ci = nullptr;
    // the function's instructions, and the address of its pc
    llvm::Value* code = nullptr;
    llvm::Value* pc = nullptr;
};

// The Futamura projection of a block: luaV_execute specialized to the
// block's instructions. Each instruction calls lvm.c's function for it
// with the instruction as a constant, and LLVM inlines the call. What
// remains is the interpreter's work for that instruction alone, without
// fetching, decoding or dispatch.
class BlockCompiler {
public:
    BlockCompiler(llvm::IRBuilder<>& builder, const Proto& proto,
                  const Frame& frame)
        : builder(builder), proto(proto), frame(frame) {}

    // Returns what the block's last instruction returned: the next call
    // for luaV_execute to run, if any.
    llvm::Value* compile(const CFG::Block& block);

private:
    llvm::Value* run(int pc);

    llvm::IRBuilder<>& builder;
    const Proto& proto;
    Frame frame;
};

llvm::Value* BlockCompiler::compile(const CFG::Block& block) {
    llvm::Value* next = nullptr;
    for (int pc = block.start; pc < block.end; pc++)
        next = run(pc);
    return next;
}

llvm::Value* BlockCompiler::run(int pc) {
    Instruction i = proto.code[pc];
    // the interpreter's pc after fetching i
    builder.CreateStore(builder.CreateConstInBoundsGEP1_32(
                            builder.getInt32Ty(), frame.code, pc + 1),
                        frame.pc);
    llvm::Module& module = *builder.GetInsertBlock()->getModule();
    auto* ptr = builder.getPtrTy();
    auto* call = builder.CreateCall(
        module.getOrInsertFunction(
            std::string("luaV_OP_") + opnames[GET_OPCODE(i)], ptr, ptr, ptr,
            ptr, builder.getInt32Ty()),
        {frame.L, frame.ci, frame.pc, builder.getInt32(i)});
    call->addFnAttr(llvm::Attribute::AlwaysInline);
    return call;
}

// Compiles a Lua function to a native function whose control flow
// mirrors its CFG: an LLVM block for each of the CFG's blocks, ending
// where the interpreter would continue.
class FunctionCompiler {
public:
    FunctionCompiler(llvm::Module& module, const Proto& proto)
        : module(module), proto(proto), cfg(&proto),
          builder(module.getContext()) {}

    llvm::Function* compile(const llvm::Twine& name);

private:
    void enter();
    void terminate(const CFG::Block& block, llvm::Value* next);
    void to_successors(const CFG::Block& block);
    void after_call(llvm::Value* next, llvm::BasicBlock* successor);
    void tail_call(llvm::Value* next);
    llvm::BasicBlock* create_block(const llvm::Twine& name = "") {
        return llvm::BasicBlock::Create(module.getContext(), name,
                                        function);
    }

    llvm::Module& module;
    const Proto& proto;
    CFG cfg;
    llvm::IRBuilder<> builder;
    llvm::Function* function = nullptr;
    Frame frame;
    // the LLVM block for each of cfg's blocks
    std::vector<llvm::BasicBlock*> llvm_blocks;
    // returns to luaV_execute, to interpret from the saved pc
    llvm::BasicBlock* interpret = nullptr;
};

llvm::Function* FunctionCompiler::compile(const llvm::Twine& name) {
    auto* ptr = builder.getPtrTy();
    function = llvm::Function::Create(
        llvm::FunctionType::get(ptr, {ptr, ptr}, false),
        llvm::GlobalValue::InternalLinkage, name, module);
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

    BlockCompiler block_compiler(builder, proto, frame);
    for (auto [block, llvm_block] :
         llvm::zip_equal(cfg.blocks, llvm_blocks)) {
        builder.SetInsertPoint(llvm_block);
        terminate(block, block_compiler.compile(block));
    }
    return function;
}

// Resumes at the saved pc: the function's start, or after a call.
void FunctionCompiler::enter() {
    auto* ptr = builder.getPtrTy();
    frame.pc = builder.CreateAlloca(ptr);
    frame.code = builder.CreateCall(
        module.getOrInsertFunction(NAME(lm_code), ptr, ptr), {frame.ci});
    auto* saved =
        builder.CreateCall(module.getOrInsertFunction(
                               NAME(lm_resume), builder.getInt32Ty(), ptr),
                           {frame.ci});
    auto* resume = builder.CreateSwitch(saved, interpret);
    for (int e : cfg.entries)
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
    const std::vector<int>& targets = block.successors;
    if (targets.size() == 1) {
        builder.CreateBr(llvm_blocks[targets.front()]);
        return;
    }
    auto* moved = builder.CreatePtrDiff(
        builder.getInt32Ty(),
        builder.CreateLoad(builder.getPtrTy(), frame.pc), frame.code);
    auto* to = builder.CreateSwitch(moved, llvm_blocks[targets.front()]);
    for (int t : llvm::drop_begin(targets))
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
        builder.getInt32Ty(),
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
    auto* ptr = builder.getPtrTy();
    auto* native = builder.CreateCall(
        module.getOrInsertFunction(NAME(lm_compiled), ptr, ptr, ptr),
        {frame.L, frame.ci});
    builder.CreateCondBr(builder.CreateIsNull(native), done, compiled);
    builder.SetInsertPoint(compiled);
    auto* call = builder.CreateCall(function->getFunctionType(), native,
                                    {frame.L, frame.ci});
    call->setTailCallKind(llvm::CallInst::TCK_MustTail);
    builder.CreateRet(call);
}

// Compiles a chunk's functions, and defines what the runtime loads.
class ModuleCompiler {
public:
    explicit ModuleCompiler(llvm::LLVMContext& context)
        : module(std::make_unique<llvm::Module>("luamura", context)) {}

    std::unique_ptr<llvm::Module> compile(const Proto& main,
                                          const std::string& chunk);

private:
    void compile_function(const Proto& proto);
    void define(const char* name, llvm::Constant* value) {
        new llvm::GlobalVariable(*module, value->getType(), true,
                                 llvm::GlobalValue::ExternalLinkage, value,
                                 name);
    }

    std::unique_ptr<llvm::Module> module;
    std::vector<llvm::Constant*> functions;
};

std::unique_ptr<llvm::Module>
ModuleCompiler::compile(const Proto& main, const std::string& chunk) {
    compile_function(main);
    llvm::LLVMContext& context = module->getContext();
    define(NAME(lm_chunk),
           llvm::ConstantDataArray::getString(context, chunk, false));
    define(NAME(lm_chunk_size),
           llvm::ConstantInt::get(llvm::Type::getInt64Ty(context),
                                  chunk.size()));
    define(NAME(lm_functions),
           llvm::ConstantArray::get(
               llvm::ArrayType::get(llvm::PointerType::getUnqual(context),
                                    functions.size()),
               functions));
    define(NAME(lm_nfunctions),
           llvm::ConstantInt::get(llvm::Type::getInt32Ty(context),
                                  functions.size()));
    return std::move(module);
}

// The runtime attaches the functions to the chunk's Protos in preorder.
void ModuleCompiler::compile_function(const Proto& proto) {
    functions.push_back(
        FunctionCompiler(*module, proto)
            .compile("lm_function_" + llvm::Twine(functions.size())));
    for (const Proto* nested : llvm::ArrayRef(proto.p, proto.sizep))
        compile_function(*nested);
}

} // namespace

std::unique_ptr<llvm::Module> compile_module(llvm::LLVMContext& context,
                                             const Proto* main,
                                             const std::string& chunk) {
    return ModuleCompiler(context).compile(*main, chunk);
}
