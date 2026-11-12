#ifndef LM_PASS_INTEGER_SPECIALIZATION_H
#define LM_PASS_INTEGER_SPECIALIZATION_H

#include "lm_pass.h"
#include <vector>

namespace luamura {

// Guards ADD and ADDI operands, then keeps a bounded arithmetic chain
// on its integer path, skipping each operation's metamethod fallback.
class IntegerSpecialization : public LmIRPass {
public:
    void run(LmIR& ir) override;

private:
    bool extendsChain(const Node& node,
                      const std::vector<bool>& integers) const;
    void specialize(LmIR& ir, Block& block, std::vector<bool>& covered);
};

} // namespace luamura

#endif // LM_PASS_INTEGER_SPECIALIZATION_H
