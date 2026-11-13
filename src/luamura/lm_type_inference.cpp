#include "lm_type_inference.h"
#include <llvm/ADT/PostOrderIterator.h>
#include <llvm/ADT/STLExtras.h>

namespace luamura {

static bool isNumber(TypeTag tag) {
    return tag == LUA_VNUMINT || tag == LUA_VNUMFLT;
}

static bool isBitwise(OpCode op) {
    return (op >= OP_BANDK && op <= OP_SHRI) ||
           (op >= OP_BAND && op <= OP_SHR);
}

// Lua converts floats to integers for bitwise operations only when they
// have an exact integer value, so float operands may need the metamethod.
static TypeTag arithmeticType(OpCode op, TypeTag left, TypeTag right) {
    if (!isNumber(left) || !isNumber(right))
        return no_type;
    bool integers = left == LUA_VNUMINT && right == LUA_VNUMINT;
    if (isBitwise(op))
        return integers ? LUA_VNUMINT : no_type;
    if (op == OP_DIV || op == OP_DIVK || op == OP_POW || op == OP_POWK)
        return LUA_VNUMFLT;
    return integers ? LUA_VNUMINT : LUA_VNUMFLT;
}

TypeTag resultType(const Proto& proto, Instruction i,
                   llvm::function_ref<TypeTag(Slot)> typeOf) {
    OpCode op = GET_OPCODE(i);
    if (isArithmetic(op)) {
        TypeTag left = typeOf(GETARG_B(i));
        if (op >= OP_ADD)
            return arithmeticType(op, left, typeOf(GETARG_C(i)));
        if (op == OP_ADDI || op == OP_SHLI || op == OP_SHRI)
            return arithmeticType(op, left, LUA_VNUMINT);
        return arithmeticType(op, left, rawtt(&proto.k[GETARG_C(i)]));
    }
    LUAMURA_SWITCH(i) {
    LUAMURA_CASE(MOVE):
        return typeOf(GETARG_B(i));
    LUAMURA_CASE(LOADI):
        return LUA_VNUMINT;
    LUAMURA_CASE(LOADF):
        return LUA_VNUMFLT;
    LUAMURA_CASE(LOADK):
        return rawtt(&proto.k[GETARG_Bx(i)]);
    LUAMURA_CASE(LOADFALSE):
        return LUA_VFALSE;
    LUAMURA_CASE(LOADTRUE):
        return LUA_VTRUE;
    default:
        return no_type;
    }
}

// Visiting blocks in reverse postorder defines each value before its
// uses, except along loops, where a phi's later inputs are not known.
TypeInference::TypeInference(const LmIR& ir)
    : proto(ir.proto), types(ir.values.size(), no_type) {
    for (const Block* block :
         llvm::ReversePostOrderTraversal<const LmIR*>(&ir)) {
        visited.insert(block);
        bool reached = block == ir.blocks.front().get() ||
                       llvm::any_of(block->predecessors, [&](Block* from) {
                           return taken.contains({from, block});
                       });
        if (!reached)
            continue;
        for (const Node& phi : block->phis())
            types[phi.outputs.front().value] = joinIncoming(*block, phi);
        for (const Node& node : *block)
            infer(*block, node);
        for (Block* next : block->successors)
            if (!skipsMetamethod(*block, *next))
                taken.insert({block, next});
    }
}

// Arithmetic with a known result type never runs the metamethod
// instruction after it.
bool TypeInference::skipsMetamethod(const Block& from,
                                    const Block& to) const {
    if (from.empty())
        return false;
    const Node& last = *std::prev(from.end());
    return last.kind == Node::Kind::Bytecode &&
           isArithmetic(GET_OPCODE(last.bytecode)) &&
           to.start == last.pc + 1 &&
           types[last.outputs.front().value] != no_type;
}

TypeTag TypeInference::typeOf(const Block& block, const Node& node,
                              Slot slot) const {
    auto input = llvm::find_if(
        node.inputs, [&](const Operand& op) { return op.slot == slot; });
    if (input == node.inputs.end())
        return no_type;
    auto assumed = block.types.find(input->value);
    return assumed != block.types.end() ? assumed->second
                                        : types[input->value];
}

void TypeInference::infer(const Block& block, const Node& node) {
    if (node.kind != Node::Kind::Bytecode || node.outputs.size() != 1)
        return;
    types[node.outputs.front().value] =
        resultType(proto, node.bytecode,
                   [&](Slot slot) { return typeOf(block, node, slot); });
}

// Inputs along edges the analysis has ruled out do not reach the phi.
TypeTag TypeInference::joinIncoming(const Block& block,
                                    const Node& phi) const {
    auto reaching =
        llvm::make_filter_range(phi.incoming, [&](const PhiInput& in) {
            return !visited.contains(in.predecessor) ||
                   taken.contains({in.predecessor, &block});
        });
    // NOTE(Justin): At some point here, we might consider a type
    // latice where several incoming types recieve an entailing
    // type-set.
    TypeTag first = types[reaching.begin()->value];
    bool same = llvm::all_of(reaching, [&](const PhiInput& in) {
        return types[in.value] == first;
    });
    return same ? first : no_type;
}

} // namespace luamura
