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

static TypeTag arithmeticType(const Proto& proto, Instruction i,
                              llvm::function_ref<TypeTag(Slot)> typeOf) {
    OpCode op = GET_OPCODE(i);
    TypeTag left = typeOf(GETARG_B(i));
    if (op >= OP_ADD)
        return arithmeticType(op, left, typeOf(GETARG_C(i)));
    if (op == OP_ADDI || op == OP_SHLI || op == OP_SHRI)
        return arithmeticType(op, left, LUA_VNUMINT);
    return arithmeticType(op, left, rawtt(&proto.k[GETARG_C(i)]));
}

// An integer loop starts with integers, and other loops with floats.
// A loop that runs no iterations leaves the registers as they were.
static TypeTag forPrepType(Instruction i, Slot output,
                           llvm::function_ref<TypeTag(Slot)> typeOf) {
    Slot a = GETARG_A(i);
    bool integers =
        typeOf(a) == LUA_VNUMINT && typeOf(a + 2) == LUA_VNUMINT;
    TypeTag entered = integers ? LUA_VNUMINT : LUA_VNUMFLT;
    if (integers && output != a + 1)
        return LUA_VNUMINT;
    return typeOf(output) == entered ? entered : no_type;
}

TypeTag typeTransfer(const Proto& proto, Instruction i, Slot output,
                     llvm::function_ref<TypeTag(Slot)> typeOf) {
    // the type when the instruction sets only R[A]
    auto setsA = [&](TypeTag tag) {
        return output == Slot(GETARG_A(i)) ? tag : no_type;
    };
    LUAMURA_SWITCH(i) {
    LUAMURA_CASE(MOVE):
        return setsA(typeOf(GETARG_B(i)));
    LUAMURA_CASE(LOADI):
        return setsA(LUA_VNUMINT);
    LUAMURA_CASE(LOADF):
        return setsA(LUA_VNUMFLT);
    LUAMURA_CASE(LOADK):
        return setsA(rawtt(&proto.k[GETARG_Bx(i)]));
    LUAMURA_CASE(LOADFALSE):
    LUAMURA_CASE(LFALSESKIP):
        return setsA(LUA_VFALSE);
    LUAMURA_CASE(LOADTRUE):
        return setsA(LUA_VTRUE);
    LUAMURA_CASE(LOADNIL):
        return LUA_VNIL;
    LUAMURA_CASE(ADDI):
    LUAMURA_CASE(ADDK):
    LUAMURA_CASE(SUBK):
    LUAMURA_CASE(MULK):
    LUAMURA_CASE(MODK):
    LUAMURA_CASE(POWK):
    LUAMURA_CASE(DIVK):
    LUAMURA_CASE(IDIVK):
    LUAMURA_CASE(BANDK):
    LUAMURA_CASE(BORK):
    LUAMURA_CASE(BXORK):
    LUAMURA_CASE(SHLI):
    LUAMURA_CASE(SHRI):
    LUAMURA_CASE(ADD):
    LUAMURA_CASE(SUB):
    LUAMURA_CASE(MUL):
    LUAMURA_CASE(MOD):
    LUAMURA_CASE(POW):
    LUAMURA_CASE(DIV):
    LUAMURA_CASE(IDIV):
    LUAMURA_CASE(BAND):
    LUAMURA_CASE(BOR):
    LUAMURA_CASE(BXOR):
    LUAMURA_CASE(SHL):
    LUAMURA_CASE(SHR):
        return setsA(arithmeticType(proto, i, typeOf));
    LUAMURA_CASE(FORPREP):
        return forPrepType(i, output, typeOf);
    // The loop changes values but not tags.
    LUAMURA_CASE(FORLOOP):
        return typeOf(output);
    // values from tables, upvalues, varargs, callees and metamethods,
    // which only run time knows
    LUAMURA_CASE(GETUPVAL):
    LUAMURA_CASE(GETTABUP):
    LUAMURA_CASE(GETTABLE):
    LUAMURA_CASE(GETI):
    LUAMURA_CASE(GETFIELD):
    LUAMURA_CASE(MMBIN):
    LUAMURA_CASE(MMBINI):
    LUAMURA_CASE(MMBINK):
    LUAMURA_CASE(CALL):
    LUAMURA_CASE(TFORPREP):
    LUAMURA_CASE(TFORCALL):
    LUAMURA_CASE(VARARG):
    LUAMURA_CASE(GETVARG):
        return no_type;
    // These set no registers of their own. Any captured registers they
    // set, by running Lua, are unknown.
    LUAMURA_CASE(SETUPVAL):
    LUAMURA_CASE(SETTABUP):
    LUAMURA_CASE(SETTABLE):
    LUAMURA_CASE(SETI):
    LUAMURA_CASE(SETFIELD):
    LUAMURA_CASE(CLOSE):
    LUAMURA_CASE(TBC):
    LUAMURA_CASE(JMP):
    LUAMURA_CASE(EQ):
    LUAMURA_CASE(LT):
    LUAMURA_CASE(LE):
    LUAMURA_CASE(EQK):
    LUAMURA_CASE(EQI):
    LUAMURA_CASE(LTI):
    LUAMURA_CASE(LEI):
    LUAMURA_CASE(GTI):
    LUAMURA_CASE(GEI):
    LUAMURA_CASE(TEST):
    LUAMURA_CASE(TAILCALL):
    LUAMURA_CASE(RETURN):
    LUAMURA_CASE(RETURN0):
    LUAMURA_CASE(RETURN1):
    LUAMURA_CASE(TFORLOOP):
    LUAMURA_CASE(SETLIST):
    LUAMURA_CASE(ERRNNIL):
    LUAMURA_CASE(VARARGPREP):
    LUAMURA_CASE(EXTRAARG):
        return no_type;
    // TODO: not yet modeled, though their operands' types can determine
    // the types they set.
    LUAMURA_CASE(LOADKX):
    LUAMURA_CASE(SELF):
    LUAMURA_CASE(UNM):
    LUAMURA_CASE(BNOT):
    LUAMURA_CASE(NOT):
    LUAMURA_CASE(LEN):
    LUAMURA_CASE(CONCAT):
    LUAMURA_CASE(TESTSET):
    // TODO: these collect garbage after setting R[A], and a finalizer can
    // change it if a closure captures it.
    LUAMURA_CASE(NEWTABLE):
    LUAMURA_CASE(CLOSURE):
        return no_type;
    }
    return no_type;
}

// Arithmetic with a known result type never runs the metamethod
// instruction after it.
// TODO: tests and comparisons, whose operand types can decide them.
bool typesRuleOut(const Proto& proto, const Node& last, const Block& to,
                  llvm::function_ref<TypeTag(Slot)> typeOf) {
    if (last.kind != Node::Kind::Bytecode ||
        !isArithmetic(GET_OPCODE(last.bytecode)) || to.start != last.pc + 1)
        return false;
    Slot output = last.outputs.front().slot;
    return typeTransfer(proto, last.bytecode, output, typeOf) != no_type;
}

// Visiting blocks in reverse postorder defines each value before its
// uses, except along loops. Inputs along edges not yet taken are left
// out of phis, and the visits repeat until no type or edge changes.
TypeInference::TypeInference(const LmIR& ir)
    : proto(ir.proto), types(ir.values.size(), no_type) {
    llvm::ReversePostOrderTraversal<const LmIR*> order(&ir);
    for (bool changed = true; changed;) {
        changed = false;
        for (const Block* block : order) {
            bool reached =
                block == ir.blocks.front().get() ||
                llvm::any_of(block->predecessors, [&](Block* from) {
                    return taken.contains({from, block});
                });
            if (!reached)
                continue;
            for (const Node& phi : block->phis())
                changed |= update(phi.outputs.front().value,
                                  joinIncoming(*block, phi));
            for (const Node& node : *block)
                changed |= infer(*block, node);
            for (Block* next : block->successors) {
                if (!block->empty()) {
                    const Node& last = *std::prev(block->end());
                    if (typesRuleOut(proto, last, *next, [&](Slot slot) {
                            return typeOf(*block, last, slot);
                        }))
                        continue;
                }
                changed |= taken.insert({block, next}).second;
            }
        }
    }
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

bool TypeInference::update(ValueID value, TypeTag type) {
    if (types[value] == type)
        return false;
    types[value] = type;
    return true;
}

bool TypeInference::infer(const Block& block, const Node& node) {
    if (node.kind != Node::Kind::Bytecode)
        return false;
    bool changed = false;
    for (const Operand& output : node.outputs)
        changed |= update(
            output.value,
            typeTransfer(proto, node.bytecode, output.slot, [&](Slot slot) {
                return typeOf(block, node, slot);
            }));
    return changed;
}

TypeTag TypeInference::joinIncoming(const Block& block,
                                    const Node& phi) const {
    auto reaching =
        llvm::make_filter_range(phi.incoming, [&](const PhiInput& in) {
            return taken.contains({in.predecessor, &block});
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
