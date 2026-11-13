#ifndef LM_IR_H
#define LM_IR_H

#include "lm_lua.h"
#include <llvm/ADT/BitmaskEnum.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/GraphTraits.h>
#include <llvm/ADT/Twine.h>
#include <llvm/ADT/iterator.h>
#include <llvm/ADT/iterator_range.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/GenericDomTree.h>
#include <cstdint>
#include <iterator>
#include <memory>
#include <type_traits>
#include <vector>

namespace luamura {

LLVM_ENABLE_BITMASK_ENUMS_IN_NAMESPACE();

using PC = int32_t;
using NodeID = uint32_t;
using BlockID = uint32_t;
using ValueID = uint32_t;
using Slot = uint32_t;
// a Lua type variant tag, such as LUA_VNUMINT
using TypeTag = uint8_t;
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
    enum class Kind {
        FrameRead,
        Phi,
        Bytecode,
        // tests that its inputs have the types its first successor
        // assumes, and ends its block
        Guard,
        // reloads the frame's base, which the stack moves with
        UpdateBase,
    };
    
    ///NOTE(Justin): "implies the ones before it" ... ahh i don't
    ///like using an implicit partial order in an enum, specially
    /// with strangely named and poorly described enums.

    // What a node does beyond its frame slot inputs and outputs.
    enum class Effects : uint8_t {
        None = 0,
        // reallocates the Lua stack, so the frame's base must be reloaded
        MovesBase = 1 << 0,
        // runs Lua code, such as a callee, metamethod, __close or
        // finalizer, which can change any captured slot
        RunsLua = 1 << 1,
        LLVM_MARK_AS_BITMASK_ENUM(RunsLua),
    };

    NodeID id;
    Kind kind;
    Effects effects = Effects::None;
    PC pc = -1;
    Instruction bytecode = 0;
    std::vector<Operand> inputs;
    std::vector<Operand> outputs;
    std::vector<PhiInput> incoming;
};

struct Value {
    NodeID node;
    Slot slot;
};

// Visits a block's instructions as nodes rather than as node IDs.
template <typename NodeT>
class NodeIterator
    : public llvm::iterator_adaptor_base<
          NodeIterator<NodeT>, std::vector<NodeID>::const_iterator,
          std::random_access_iterator_tag, NodeT> {
public:
    using Nodes =
        std::conditional_t<std::is_const_v<NodeT>, const std::vector<Node>,
                           std::vector<Node>>;

    NodeIterator() = default;
    NodeIterator(Nodes& nodes, std::vector<NodeID>::const_iterator id)
        : NodeIterator::iterator_adaptor_base(id), nodes(&nodes) {}

    NodeT& operator*() const { return (*nodes)[*this->I]; }
    // the node ID's position in its block
    using NodeIterator::iterator_adaptor_base::wrapped;

private:
    Nodes* nodes = nullptr;
};

struct Block {
    using iterator = NodeIterator<Node>;
    using const_iterator = NodeIterator<const Node>;

    // How control leaves the block, after its last instruction.
    enum class Terminator {
        // to the saved pc: the function's start, or after a call
        Resume,
        // to the successor at the pc the last instruction left
        Branch,
        // a Lua callee is left to the interpreter
        Call,
        TailCall,
        Return,
        // to the first successor if the guard holds, else the second
        Guard,
    };

    LmIR* parent;
    BlockID id;
    // the pc the block starts at, if the interpreter can be there
    PC start = -1;
    Terminator terminator = Terminator::Branch;
    // the types the block assumes its values have, which the guard
    // before it tests
    llvm::DenseMap<ValueID, TypeTag> types;
    std::vector<Block*> predecessors;
    std::vector<Block*> successors;
    std::vector<NodeID> phi_ids;
    std::vector<NodeID> instruction_ids;

    // a block iterates its instructions
    iterator begin();
    iterator end();
    const_iterator begin() const;
    const_iterator end() const;
    llvm::iterator_range<iterator> phis();
    llvm::iterator_range<const_iterator> phis() const;
    Node& front() { return *begin(); }
    Node& back() { return *std::prev(end()); }
    size_t size() const { return instruction_ids.size(); }
    bool empty() const { return instruction_ids.empty(); }

    LmIR* getParent() const { return parent; }
    void printAsOperand(llvm::raw_ostream& out, bool) const;
};

/// [LMDOC] LmIR is the SSA intermediate representation for Luamura.
/// An instance of LmIR is constructed from a Lua function's `Proto`
/// type, which analyzes the bytecode, extracts the CFG then applies
/// the SSA construction algorithm.
///
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

    Block& addBlock();
    void addEdge(Block& from, Block& to);
    // Moves the instructions from at on, with the block's terminator
    // and successors, to a new block. The caller ends the block.
    Block& splitBlock(Block& block, Block::iterator at);
    Node& addNode(Block& block, Node::Kind kind, PC pc = -1);
    // Returns the inserted node's position, as std::vector::insert does.
    Block::iterator insertNode(Block& block, Block::iterator at,
                               Node::Kind kind);
    void addOutput(Node& node, Slot slot);
    // Places the phis a changed graph needs and resolves each input to
    // its value. Existing nodes and values keep their IDs.
    void updateSSA();
    void print(llvm::raw_ostream& out) const;
    void printBlock(llvm::raw_ostream& out, const Block& block) const;
    void writeGraph(llvm::raw_ostream& out, const llvm::Twine& name);

    const Proto& proto;
    std::vector<std::unique_ptr<Block>> blocks;
    std::vector<Node> nodes;
    std::vector<Value> values;

private:
    Node& createNode(Node::Kind kind, PC pc);
    std::vector<PC> buildBlocks();
    void buildNodes(const std::vector<PC>& ends);
    Block::Terminator terminatorAt(PC last) const;
    using DominatorTree = llvm::DominatorTreeBase<Block, false>;
    bool hasPhi(const Block& block, Slot slot) const;
    void insertPhis(DominatorTree& dominators);
    void renameValues(DominatorTree& dominators);
};

// The frame slots each bytecode instruction reads and writes, and
// whether it can move the frame or run Lua code. Lowering still runs each
// instruction's handler, so the model describes the handlers in lvmops.h.
class LmSlotModel {
public:
    explicit LmSlotModel(LmIR& ir);

    void model(Node& node);

private:
    void findCapturedSlots();
    bool reads(Slot slot) const;
    bool writes(Slot slot) const;

    void read(Slot slot);
    void write(Slot slot);
    void readRange(Slot from, Slot to);
    void writeRange(Slot from, Slot to);
    void movesBase();
    void runsLua();
    Slot upTo(uint32_t count, Slot end) const;

    LmIR& ir;
    Slot top;
    // slots that closures capture, which Lua code can change through
    // open upvalues
    std::vector<Slot> captured;
    Node* node = nullptr;
};

// A chunk's functions, in the preorder the runtime attaches them to
// its Protos.
// a function's position in its module
using FunctionIndex = size_t;

class LmModule {
public:
    explicit LmModule(const Proto& main);

    std::vector<std::unique_ptr<LmIR>> functions;

private:
    void addFunction(const Proto& proto);
};

inline Block::iterator Block::begin() {
    return {parent->nodes, instruction_ids.begin()};
}

inline Block::iterator Block::end() {
    return {parent->nodes, instruction_ids.end()};
}

inline Block::const_iterator Block::begin() const {
    return {parent->nodes, instruction_ids.begin()};
}

inline Block::const_iterator Block::end() const {
    return {parent->nodes, instruction_ids.end()};
}

inline llvm::iterator_range<Block::iterator> Block::phis() {
    return {{parent->nodes, phi_ids.begin()},
            {parent->nodes, phi_ids.end()}};
}

inline llvm::iterator_range<Block::const_iterator> Block::phis() const {
    return {{parent->nodes, phi_ids.begin()},
            {parent->nodes, phi_ids.end()}};
}

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
