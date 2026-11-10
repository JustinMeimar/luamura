#ifndef LM_LOWER_H
#define LM_LOWER_H

#include "lm_ir.h"

namespace luamura {

// Lowers LmIR to a backend's code. A module lowers each function, a
// function each block, and a block each node, then its terminator.
class LmIRLowerer {
public:
    virtual ~LmIRLowerer() = default;

    virtual void lower(const LmModule& module);
    virtual void lower(const LmIR& ir);
    virtual void lower(const Block& block);
    void lower(const Node& node);

protected:
    virtual void lowerFrameRead(const Node& node) = 0;
    virtual void lowerPhi(const Node& node) = 0;
    virtual void lowerBytecode(const Node& node) = 0;
    virtual void lowerGuard(const Node& node) = 0;
    virtual void lowerTerminator(const Block& block) = 0;
};

} // namespace luamura

#endif // LM_LOWER_H
