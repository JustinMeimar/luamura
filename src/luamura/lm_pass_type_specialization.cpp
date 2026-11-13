#include "lm_pass_type_specialization.h"
#include <llvm/ADT/STLExtras.h>
#include <llvm/ADT/SmallBitVector.h>
#include <llvm/ADT/SmallPtrSet.h>

namespace luamura {

// TODO: predict from type profiles. Until then, arithmetic operands are
// predicted to be integers, and nothing else is predicted.
static TypeTag predictedType(const Node& node, Slot) {
    return isArithmetic(GET_OPCODE(node.bytecode)) ? LUA_VNUMINT : no_type;
}

// After it, the types of the values Lua code could change are unknown.
static bool endsRegion(const Node& node) {
    return node.kind == Node::Kind::FrameRead ||
           any(node.effects & Node::Effects::RunsLua);
}

static Block& blockContaining(LmIR& ir, NodeID id) {
    return *llvm::find_if(ir, [&](const Block& block) {
        return llvm::is_contained(block.instruction_ids, id);
    });
}

static Block::iterator positionOf(Block& block, NodeID id) {
    return llvm::find_if(block,
                         [&](const Node& node) { return node.id == id; });
}

void TypeSpecialization::run(LmIR& ir) {
    TypeInference inference(ir);
    std::vector<Channel> channels;
    for (NodeID start : regionStarts(ir)) {
        Channel channel = walk(ir, inference, start);
        if (!channel.guards.empty())
            channels.push_back(std::move(channel));
    }
    auto specialized = llvm::to_vector(
        llvm::map_range(channels, [&](const Channel& channel) {
            return &specialize(ir, channel);
        }));
    ir.updateSSA();
    for (auto [channel, block] : llvm::zip_equal(channels, specialized)) {
        const Node& guard = block->predecessors.front()->back();
        for (auto [input, guarded] :
             llvm::zip_equal(guard.inputs, channel.guards))
            block->types[input.value] = guarded.second;
    }
}

std::vector<NodeID> TypeSpecialization::regionStarts(LmIR& ir) const {
    std::vector<NodeID> starts;
    for (Block& block : llvm::drop_begin(ir)) {
        if (block.empty())
            continue;
        if (llvm::all_of(block.predecessors, [](Block* from) {
                return !from->empty() && endsRegion(from->back());
            }))
            starts.push_back(block.front().id);
        for (auto [node, next] : llvm::zip(block, llvm::drop_begin(block)))
            if (endsRegion(node))
                starts.push_back(next.id);
    }
    return starts;
}

// Follows the region from its start while the types the channel knows
// leave one way on, predicting the types of the values it reads that
// are not known.
TypeSpecialization::Channel
TypeSpecialization::walk(LmIR& ir, const TypeInference& inference,
                         NodeID start) const {
    Channel channel{start};
    // the tag of each register, once the channel sets or guards it
    std::vector<TypeTag> types(ir.proto.maxstacksize, no_type);
    llvm::SmallBitVector set(ir.proto.maxstacksize);
    Block* block = &blockContaining(ir, start);
    llvm::SmallPtrSet<const Block*, 8> visited = {block};
    for (Block::iterator at = positionOf(*block, start);; ++at) {
        if (at == block->end()) {
            const Node& last = block->back();
            auto typeOf = [&](Slot slot) {
                return types[slot] != no_type || set[slot]
                           ? types[slot]
                           : inference.typeOf(*block, last, slot);
            };
            auto open =
                llvm::make_filter_range(block->successors, [&](Block* to) {
                    return !typesRuleOut(ir.proto, last, *to, typeOf);
                });
            channel.successors.assign(open.begin(), open.end());
            channel.terminator = block->terminator;
            if (block->terminator != Block::Terminator::Branch ||
                channel.successors.size() != 1 ||
                !visited.insert(channel.successors.front()).second)
                return channel;
            block = channel.successors.front();
            at = block->begin();
        }
        const Node& node = *at;
        if (node.kind != Node::Kind::Bytecode ||
            any(node.effects & Node::Effects::RunsLua)) {
            channel.stop = node.id;
            return channel;
        }
        auto typeOf = [&](Slot slot) {
            return types[slot] != no_type || set[slot]
                       ? types[slot]
                       : inference.typeOf(*block, node, slot);
        };
        // The guard at the start tests only values from before it.
        // TODO: guards within a channel, for the values it sets.
        for (const Operand& input : node.inputs) {
            if (set[input.slot] || typeOf(input.slot) != no_type)
                continue;
            TypeTag predicted = predictedType(node, input.slot);
            if (predicted == no_type)
                continue;
            channel.guards.push_back({input.slot, predicted});
            types[input.slot] = predicted;
        }
        auto outputs = llvm::to_vector(
            llvm::map_range(node.outputs, [&](const Operand& output) {
                return typeTransfer(ir.proto, node.bytecode, output.slot,
                                    typeOf);
            }));
        for (auto [output, type] : llvm::zip_equal(node.outputs, outputs)) {
            types[output.slot] = type;
            set.set(output.slot);
        }
        channel.nodes.push_back(node.id);
    }
}

// Places the guard before the region's start and returns the
// specialized channel's block.
Block& TypeSpecialization::specialize(LmIR& ir,
                                      const Channel& channel) const {
    Block* rejoin = nullptr;
    if (channel.stop) {
        Block& before = blockContaining(ir, *channel.stop);
        rejoin = &ir.splitBlock(before, positionOf(before, *channel.stop));
        before.terminator = Block::Terminator::Branch;
        ir.addEdge(before, *rejoin);
    }
    Block& head = blockContaining(ir, channel.start);
    PC pc = positionOf(head, channel.start)->pc;
    Block& generic = ir.splitBlock(head, positionOf(head, channel.start));
    Node& guard = ir.addNode(head, Node::Kind::Guard, pc);
    for (auto [slot, type] : channel.guards)
        guard.inputs.push_back({slot});
    head.terminator = Block::Terminator::Guard;
    Block& specialized = ir.addBlock();
    ir.addEdge(head, specialized);
    ir.addEdge(head, generic);

    for (NodeID id : channel.nodes) {
        Node original = ir.nodes[id];
        Node& copy =
            ir.addNode(specialized, Node::Kind::Bytecode, original.pc);
        copy.bytecode = original.bytecode;
        copy.effects = original.effects;
        copy.inputs = original.inputs;
        for (const Operand& output : original.outputs)
            ir.addOutput(copy, output.slot);
    }
    if (rejoin) {
        ir.addEdge(specialized, *rejoin);
        return specialized;
    }
    specialized.terminator = channel.terminator;
    for (Block* to : channel.successors)
        ir.addEdge(specialized, *to);
    return specialized;
}

} // namespace luamura
