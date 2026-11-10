#ifndef LM_IR_H
#define LM_IR_H

#include "lm_lua.h"

#include <llvm/ADT/GraphTraits.h>
#include <llvm/ADT/Twine.h>
#include <llvm/ADT/iterator.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/GenericDomTree.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace luamura {

using PC = int32_t;
using ValueID = uint32_t;
using Slot = uint32_t;
constexpr ValueID no_value = UINT32_MAX;
class LmIR;
struct Block;

struct Operand {
    Slot slot;
    ValueID value = no_value;
};

struct PhiInput {
    Block* predecessor;
    ValueID value;
};

struct Node {
    enum class Kind { FrameRead, Phi, Bytecode };
    enum class Effects { Frame, Runtime };

    uint32_t id;
    Kind kind;
    Effects effects = Effects::Frame;
    PC pc = -1;
    Instruction bytecode = 0;
    std::vector<Operand> inputs;
    std::vector<Operand> outputs;
    std::vector<PhiInput> incoming;
};

struct Value {
    uint32_t node;
    Slot slot;
};

struct Block {
    LmIR* parent;
    uint32_t id;
    PC start;
    PC end;
    std::vector<Block*> predecessors;
    std::vector<Block*> successors;
    std::vector<uint32_t> phis;
    std::vector<uint32_t> instructions;

    LmIR* getParent() const { return parent; }
    PC last() const { return end - 1; }
    void printAsOperand(llvm::raw_ostream& out, bool) const;
};

class LmIR {
public:
    using iterator = llvm::pointee_iterator<
        std::vector<std::unique_ptr<Block>>::iterator>;

    explicit LmIR(const Proto& proto);
    LmIR(const LmIR&) = delete;
    LmIR& operator=(const LmIR&) = delete;

    iterator begin() { return blocks.begin(); }
    iterator end() { return blocks.end(); }
    size_t size() const { return blocks.size(); }
    Block& getEntryBlock() { return *blocks.front(); }

    Node& addNode(Block& block, Node::Kind kind, PC pc = -1);
    void addOutput(Node& node, Slot slot);
    void print(llvm::raw_ostream& out) const;
    void printBlock(llvm::raw_ostream& out, const Block& block) const;
    void writeGraph(llvm::raw_ostream& out, const llvm::Twine& name);

    const Proto& proto;
    std::vector<std::unique_ptr<Block>> blocks;
    std::vector<Node> nodes;
    std::vector<Value> values;

private:
    void buildBlocks();
    void buildNodes();
    using DominatorTree = llvm::DominatorTreeBase<Block, false>;
    void insertPhis(DominatorTree& dominators);
    void renameValues(DominatorTree& dominators);
};

} // namespace luamura

namespace llvm {

template <> struct GraphTraits<luamura::Block*> {
    using NodeRef = luamura::Block*;
    using ChildIteratorType = std::vector<NodeRef>::const_iterator;
    static NodeRef getEntryNode(NodeRef block) { return block; }
    static ChildIteratorType child_begin(NodeRef block) {
        return block->successors.begin();
    }
    static ChildIteratorType child_end(NodeRef block) {
        return block->successors.end();
    }
};

template <>
struct GraphTraits<Inverse<luamura::Block*>>
    : GraphTraits<luamura::Block*> {
    static NodeRef getEntryNode(Inverse<NodeRef> graph) {
        return graph.Graph;
    }
    static ChildIteratorType child_begin(NodeRef block) {
        return block->predecessors.begin();
    }
    static ChildIteratorType child_end(NodeRef block) {
        return block->predecessors.end();
    }
};

template <>
struct GraphTraits<luamura::LmIR*> : GraphTraits<luamura::Block*> {
    using nodes_iterator = pointer_iterator<luamura::LmIR::iterator>;
    static NodeRef getEntryNode(luamura::LmIR* function) {
        return &function->getEntryBlock();
    }
    static nodes_iterator nodes_begin(luamura::LmIR* function) {
        return nodes_iterator(function->begin());
    }
    static nodes_iterator nodes_end(luamura::LmIR* function) {
        return nodes_iterator(function->end());
    }
    static size_t size(luamura::LmIR* function) { return function->size(); }
};

} // namespace llvm

#endif
