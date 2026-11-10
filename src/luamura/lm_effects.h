#ifndef LM_EFFECTS_H
#define LM_EFFECTS_H

#include "lm_ir.h"

#include <vector>

namespace luamura {

// The frame slots each bytecode instruction reads and writes, and
// whether it can run Lua code. Lowering still runs each instruction's
// handler, so the model describes the handlers in lvmops.h.
class EffectModel {
public:
    explicit EffectModel(LmIR& ir);

    void model(Node& node);

private:
    void findCapturedSlots();
    bool reads(unsigned slot) const;
    bool writes(unsigned slot) const;

    void read(unsigned slot);
    void write(unsigned slot);
    void readRange(unsigned from, unsigned to);
    void writeRange(unsigned from, unsigned to);
    void runsLua();
    unsigned upTo(unsigned count, unsigned end) const;

    LmIR& ir;
    unsigned top;
    // slots that closures capture, which Lua code can change through
    // open upvalues
    std::vector<unsigned> captured;
    Node* node = nullptr;
};

} // namespace luamura

#endif
