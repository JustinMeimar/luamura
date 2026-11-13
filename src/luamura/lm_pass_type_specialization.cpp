#include "lm_pass_type_specialization.h"
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/DenseSet.h>
#include <llvm/ADT/STLExtras.h>

namespace luamura {

// TODO: predict from type profiles. Until then, arithmetic operands are
// predicted to be integers, and nothing else is predicted.
static TypeTag predictedType(const Node& node, Slot) {
    return isArithmetic(GET_OPCODE(node.bytecode)) ? LUA_VNUMINT : no_type;
}

static bool runsLua(const Node& node) {
    return any(node.effects & Node::Effects::RunsLua);
}

// After it, the types of the values Lua code could change are unknown.
static bool endsRegion(const Node& node) {
    return node.kind == Node::Kind::FrameRead || runsLua(node);
}

// Once regions end at block ends, a block that runs Lua is outside them.
static bool inRegion(const Block& block) {
    return !block.empty() && !endsRegion(*block.begin());
}

void TypeSpecialization::run(LmIR& ir) {
    splitAtRegionEnds(ir);
    TypeInference inference(ir);
    std::vector<Channel> channels;
    for (Block* start : regionStarts(ir)) {
        Channel channel = walk(ir, inference, *start);
        if (!channel.guards.empty())
            channels.push_back(std::move(channel));
    }
    auto copies = llvm::to_vector(
        llvm::map_range(channels, [&](const Channel& channel) {
            return specialize(ir, channel);
        }));
    ir.updateSSA();
    // The guard's inputs are the values at the region's start, which
    // dominates every copy.
    for (auto [channel, blocks] : llvm::zip_equal(channels, copies)) {
        const Node& guard = blocks.front()->predecessors.front()->back();
        for (auto [input, guarded] :
             llvm::zip_equal(guard.inputs, channel.guards))
            for (Block* block : blocks)
                block->types[input.value] = guarded.second;
    }
}

// Places each node that runs Lua in a block of its own.
void TypeSpecialization::splitAtRegionEnds(LmIR& ir) const {
    for (size_t index = 1; index < ir.blocks.size(); ++index) {
        Block& block = *ir.blocks[index];
        Block::iterator at = llvm::find_if(block, runsLua);
        if (at == block.begin() && at != block.end())
            ++at;
        if (at == block.end())
            continue;
        Block& rest = ir.splitBlock(block, at);
        block.terminator = Block::Terminator::Branch;
        ir.addEdge(block, rest);
    }
}

std::vector<Block*> TypeSpecialization::regionStarts(LmIR& ir) const {
    std::vector<Block*> starts;
    for (Block& block : ir)
        if (inRegion(block) &&
            llvm::all_of(block.predecessors, [](Block* from) {
                return !from->empty() && endsRegion(from->back());
            }))
            starts.push_back(&block);
    return starts;
}

// Infers the region's types from its start, guarding the values from
// before it that it reads, where their types are not known.
//
// TODO: a fork in the design. Each block is copied once, so a loop's
// header joins the types entering the loop with those around it, and
// the channel leaves for the generic one where the join loses a type.
// Copying a block once per incoming context, as basic block versioning
// does, keeps those types, at the cost of more copies.
TypeSpecialization::Channel
TypeSpecialization::walk(LmIR& ir, const TypeInference& inference,
                         Block& start) const {
    Channel channel{&start};
    TypeInference unguarded(inference, start, inRegion, {});
    llvm::DenseSet<ValueID> inside;
    for (Block& block : ir) {
        if (!unguarded.reaches(block))
            continue;
        if (&block != &start)
            for (const Node& phi : block.phis())
                inside.insert(phi.outputs.front().value);
        for (const Node& node : block)
            for (const Operand& output : node.outputs)
                inside.insert(output.value);
    }
    auto guarded = [&](ValueID value) {
        return llvm::any_of(channel.guards, [&](auto guard) {
            return guard.first == value;
        });
    };
    for (Block& block : ir) {
        if (!unguarded.reaches(block))
            continue;
        for (const Node& node : block)
            for (const Operand& input : node.inputs) {
                if (inside.contains(input.value) || guarded(input.value) ||
                    unguarded.typeOf(block, node, input.slot) != no_type)
                    continue;
                TypeTag predicted = predictedType(node, input.slot);
                if (predicted != no_type)
                    channel.guards.push_back({input.value, predicted});
            }
    }
    TypeInference region(inference, start, inRegion, channel.guards);
    // Guards can close the only edges to a value's readers.
    llvm::DenseSet<ValueID> read;
    for (Block& block : ir) {
        if (!region.reaches(block))
            continue;
        for (const Node& node : block)
            for (const Operand& input : node.inputs)
                read.insert(input.value);
        if (&block != &start)
            channel.blocks.push_back(&block);
        for (Block* to : block.successors)
            if (region.takes(block, *to))
                channel.edges.push_back({&block, to});
    }
    channel.blocks.insert(channel.blocks.begin(), &start);
    llvm::erase_if(channel.guards,
                   [&](auto guard) { return !read.contains(guard.first); });
    return channel;
}

// Copies the channel's blocks, with the start's copy first, and places
// the guard before the region's start.
std::vector<Block*>
TypeSpecialization::specialize(LmIR& ir, const Channel& channel) const {
    llvm::DenseMap<const Block*, Block*> copyOf;
    std::vector<Block*> copies;
    for (Block* block : channel.blocks) {
        Block& copy = ir.addBlock();
        copy.start = block->start;
        copy.terminator = block->terminator;
        for (NodeID id : block->instruction_ids) {
            Node original = ir.nodes[id];
            Node& node =
                ir.addNode(copy, Node::Kind::Bytecode, original.pc);
            node.bytecode = original.bytecode;
            node.effects = original.effects;
            node.inputs = original.inputs;
            for (const Operand& output : original.outputs)
                ir.addOutput(node, output.slot);
        }
        copyOf[block] = &copy;
        copies.push_back(&copy);
    }
    // Edges out of the region rejoin the generic channel.
    for (auto [from, to] : channel.edges)
        ir.addEdge(*copyOf[from], *copyOf.lookup_or(to, to));

    Block& start = *channel.start;
    PC pc = start.front().pc;
    Block& generic = ir.splitBlock(start, start.begin());
    Node& guard = ir.addNode(start, Node::Kind::Guard, pc);
    for (auto [value, type] : channel.guards)
        guard.inputs.push_back({ir.values[value].slot});
    start.terminator = Block::Terminator::Guard;
    ir.addEdge(start, *copies.front());
    ir.addEdge(start, generic);
    return copies;
}

} // namespace luamura
