#include "lmcodegen.h"
#include "lmcfg.h"

#include <llvm/ADT/STLExtras.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>

#include <string>
#include <type_traits>
#include <vector>

extern "C" {
#include "lopnames.h"
#include "luamura.h"
}

// The name of a declaration, which must exist.
#define NAME(x) (static_cast<void>(sizeof(&(x))), #x)

// A function luamura.h declares, declared in the module as C declares it.
#define RUNTIME(f) declare(#f, decltype(&f){})

namespace {

// The LLVM types of C types, so that the module agrees with C's
// declarations.
class Types {
public:
    explicit Types(llvm::LLVMContext& context) : context(context) {}

    template <typename T> llvm::Type* of() const {
        if constexpr (std::is_pointer_v<T>) {
            return llvm::PointerType::getUnqual(context);
        } else {
            static_assert(std::is_integral_v<T>);
            return llvm::IntegerType::get(context, 8 * sizeof(T));
        }
    }

    template <typename R, typename... A>
    llvm::FunctionType* of(R (*)(A...)) const {
        return llvm::FunctionType::get(of<R>(), {of<A>()...}, false);
    }

private:
    llvm::LLVMContext& context;
};

class FunctionCompiler {
public:
    FunctionCompiler(llvm::Module& module, const Types& types,
                     const Proto* proto)
        : module(module), types(types), proto(proto), cfg(proto),
          builder(module.getContext()) {}

    llvm::Function* compile(const std::string& name);

private:
    llvm::Value* run(int pc);
    void terminate(const CFG::Block& block, llvm::Value* next);
    void to_successors(const CFG::Block& block);
    void after_call(llvm::Value* next, llvm::BasicBlock* successor);
    void tail_call(llvm::Value* next);
    llvm::BasicBlock* new_block(const llvm::Twine& name = "") {
        return llvm::BasicBlock::Create(module.getContext(), name,
                                        function);
    }
    template <typename R, typename... A>
    llvm::FunctionCallee declare(const std::string& name, R (*type)(A...)) {
        return module.getOrInsertFunction(name, types.of(type));
    }

    llvm::Module& module;
    const Types& types;
    const Proto* proto;
    CFG cfg;
    llvm::IRBuilder<> builder;
    llvm::Function* function = nullptr;
    llvm::Value* L = nullptr;
    llvm::Value* ci = nullptr;
    llvm::Value* code = nullptr;
    llvm::Value* pc_address = nullptr;
    llvm::BasicBlock* interpret = nullptr;
    // one for each of cfg's blocks
    std::vector<llvm::BasicBlock*> blocks;
};

llvm::Function* FunctionCompiler::compile(const std::string& name) {
    function = llvm::Function::Create(types.of(lua_AotFunction{}),
                                      llvm::GlobalValue::InternalLinkage,
                                      name, module);
    L = function->getArg(0);
    ci = function->getArg(1);

    auto* entry = new_block();
    interpret = new_block();
    // named for reading the IR against the bytecode
    for (const CFG::Block& b : cfg.blocks)
        blocks.push_back(new_block("pc" + llvm::Twine(b.start)));

    builder.SetInsertPoint(interpret);
    builder.CreateRet(ci);

    builder.SetInsertPoint(entry);
    pc_address = builder.CreateAlloca(types.of<const Instruction*>());
    code = builder.CreateCall(RUNTIME(lm_code), {ci});
    auto* saved = builder.CreateCall(RUNTIME(lm_resume), {ci});
    auto* resume = builder.CreateSwitch(saved, interpret);
    for (int e : cfg.entries)
        resume->addCase(builder.getInt32(cfg.blocks[e].start), blocks[e]);

    for (size_t b = 0; b < cfg.blocks.size(); b++) {
        builder.SetInsertPoint(blocks[b]);
        llvm::Value* next = nullptr;
        for (int pc = cfg.blocks[b].start; pc < cfg.blocks[b].end; pc++)
            next = run(pc);
        terminate(cfg.blocks[b], next);
    }
    return function;
}

// Calls lvm.c's function for the instruction, which LLVM inlines. With
// the instruction a constant, what remains is the interpreter's work for
// this instruction alone: the dispatch and decoding fold away.
llvm::Value* FunctionCompiler::run(int pc) {
    Instruction i = proto->code[pc];
    // the interpreter's pc after fetching i
    builder.CreateStore(builder.CreateConstInBoundsGEP1_32(
                            types.of<Instruction>(), code, pc + 1),
                        pc_address);
    auto* next = builder.CreateCall(
        declare(std::string("luaV_OP_") + opnames[GET_OPCODE(i)],
                lm_OpFunction{}),
        {L, ci, pc_address, builder.getInt32(i)});
    next->addFnAttr(llvm::Attribute::AlwaysInline);
    return next;
}

// Control goes where the interpreter would go after the block's last
// instruction, whose function returned next.
void FunctionCompiler::terminate(const CFG::Block& block,
                                 llvm::Value* next) {
    switch (GET_OPCODE(proto->code[block.last()])) {
    case OP_CALL:
        return after_call(next, blocks[block.successors.front()]);
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
        builder.CreateBr(blocks[targets.front()]);
        return;
    }
    auto* moved = builder.CreatePtrDiff(
        types.of<Instruction>(),
        builder.CreateLoad(types.of<const Instruction*>(), pc_address),
        code);
    auto* to = builder.CreateSwitch(moved, blocks[targets.front()]);
    for (int t : llvm::drop_begin(targets))
        to->addCase(builder.getInt64(cfg.blocks[t].start), blocks[t]);
}

// A Lua callee is left for luaV_execute to run. A C function returned
// here, and may have set a hook, such as with debug.sethook, which only
// the interpreter runs.
void FunctionCompiler::after_call(llvm::Value* next,
                                  llvm::BasicBlock* successor) {
    auto* returned = new_block();
    auto* called = new_block();
    builder.CreateCondBr(builder.CreateIsNull(next), returned, called);
    builder.SetInsertPoint(called);
    builder.CreateRet(next);
    builder.SetInsertPoint(returned);

    // volatile, as signal handlers set it
    auto* hookmask = builder.CreateLoad(
        types.of<l_signalT>(),
        builder.CreateInBoundsPtrAdd(
            L, builder.getInt64(offsetof(lua_State, hookmask))),
        true);
    builder.CreateCondBr(builder.CreateIsNotNull(hookmask), interpret,
                         successor);
}

// A Lua callee replaces this function in ci. Entering its native code
// as a tail call keeps the C stack from growing.
void FunctionCompiler::tail_call(llvm::Value* next) {
    auto* callee = new_block();
    auto* compiled = new_block();
    auto* done = new_block();
    builder.CreateCondBr(builder.CreateICmpEQ(next, ci), callee, done);
    builder.SetInsertPoint(done);
    builder.CreateRet(next);

    builder.SetInsertPoint(callee);
    auto* native = builder.CreateCall(RUNTIME(lm_compiled), {L, ci});
    builder.CreateCondBr(builder.CreateIsNull(native), done, compiled);
    builder.SetInsertPoint(compiled);
    auto* call =
        builder.CreateCall(function->getFunctionType(), native, {L, ci});
    call->setTailCallKind(llvm::CallInst::TCK_MustTail);
    builder.CreateRet(call);
}

// The runtime attaches the functions to the chunk's Protos in preorder.
void compile_function(llvm::Module& module, const Types& types,
                      const Proto* p,
                      std::vector<llvm::Constant*>& functions) {
    std::string name = "lm_function_" + std::to_string(functions.size());
    functions.push_back(FunctionCompiler(module, types, p).compile(name));
    for (int i = 0; i < p->sizep; i++)
        compile_function(module, types, p->p[i], functions);
}

} // namespace

std::unique_ptr<llvm::Module> compile_module(llvm::LLVMContext& context,
                                             const Proto* main,
                                             const std::string& chunk) {
    auto module = std::make_unique<llvm::Module>("luamura", context);
    Types types(context);
    std::vector<llvm::Constant*> functions;
    compile_function(*module, types, main, functions);

    auto define = [&](const char* name, llvm::Constant* value) {
        new llvm::GlobalVariable(*module, value->getType(), true,
                                 llvm::GlobalValue::ExternalLinkage, value,
                                 name);
    };
    define(NAME(lm_chunk),
           llvm::ConstantDataArray::getString(context, chunk, false));
    define(NAME(lm_chunk_size),
           llvm::ConstantInt::get(types.of<size_t>(), chunk.size()));
    define(NAME(lm_functions),
           llvm::ConstantArray::get(
               llvm::ArrayType::get(types.of<lua_AotFunction>(),
                                    functions.size()),
               functions));
    define(NAME(lm_nfunctions),
           llvm::ConstantInt::get(types.of<int>(), functions.size()));
    return module;
}
