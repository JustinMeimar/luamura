#ifndef LM_TYPE_INFERENCE_H
#define LM_TYPE_INFERENCE_H

#include "lm_ir.h"
#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/DenseSet.h>
#include <llvm/ADT/STLFunctionalExtras.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <vector>

namespace luamura {

// The type transfer function: the tag the instruction sets the output
// register to, given the tag of each register it reads, or no_type if
// those do not determine it. Arithmetic sets a tag exactly when its
// operands rule out the metamethod.
TypeTag typeTransfer(const Proto& proto, Instruction i, Slot output,
                     llvm::function_ref<TypeTag(Slot)> typeOf);

// Whether the tags of the registers a block's last node reads rule out
// the edge to the successor.
bool typesRuleOut(const Proto& proto, const Node& last, const Block& to,
                  llvm::function_ref<TypeTag(Slot)> typeOf);

// The tag each value has wherever it is used, where its definition
// determines one.
class TypeInference {
public:
    explicit TypeInference(const LmIR& ir);
    // Infers only the blocks within a region from its entry, given the
    // types inferred before it and those assumed there.
    TypeInference(const TypeInference& before, const Block& entry,
                  llvm::function_ref<bool(const Block&)> within,
                  llvm::ArrayRef<std::pair<ValueID, TypeTag>> assumed);

    bool reaches(const Block& block) const {
        return reached.contains(&block);
    }
    // whether the known types leave the edge open
    bool takes(const Block& from, const Block& to) const;

    TypeTag typeOf(ValueID value) const { return types[value]; }
    // the tag of the value the node reads from the register, which the
    // node's block may assume
    TypeTag typeOf(const Block& block, const Node& node, Slot slot) const;

private:
    void inferFrom(const Block& entry,
                   llvm::function_ref<bool(const Block&)> within);
    // the tag of the value, which the block may assume
    TypeTag typeIn(const Block& block, ValueID value) const;
    // whether the value's type changed
    bool update(ValueID value, TypeTag type);
    bool infer(const Block& block, const Node& node);
    TypeTag joinIncoming(const Block& block, const Node& phi) const;

    const Proto& proto;
    std::vector<TypeTag> types;
    // edges from reached blocks that the known types do not rule out
    llvm::DenseSet<std::pair<const Block*, const Block*>> taken;
    llvm::SmallPtrSet<const Block*, 32> reached;
};

} // namespace luamura

#endif // LM_TYPE_INFERENCE_H
