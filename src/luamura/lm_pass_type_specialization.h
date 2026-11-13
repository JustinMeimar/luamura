#ifndef LM_PASS_TYPE_SPECIALIZATION_H
#define LM_PASS_TYPE_SPECIALIZATION_H

#include "lm_pass.h"
#include "lm_type_inference.h"
#include <llvm/ADT/SmallPtrSet.h>

namespace luamura {

// Guards ADD and ADDI operands of unknown type as integers, then keeps
// the arithmetic after it on the guarded path while the known types rule
// out each operation's metamethod.
// TODO: only integers are guarded so far. Measure the operand types at
// arithmetic sites across the benchmarks before generalizing.
class TypeSpecialization : public LmIRPass {
public:
    void run(LmIR& ir) override;

private:
    // blocks a specialized chain already copies
    using Covered = llvm::SmallPtrSet<const Block*, 16>;

    void specialize(LmIR& ir, const TypeInference& inference, Block& block,
                    Covered& covered);
};

} // namespace luamura

#endif // LM_PASS_TYPE_SPECIALIZATION_H
