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
    bool reads(Slot slot) const;
    bool writes(Slot slot) const;

    void read(Slot slot);
    void write(Slot slot);
    void readRange(Slot from, Slot to);
    void writeRange(Slot from, Slot to);
    void runsLua();
    Slot upTo(uint32_t count, Slot end) const;

    LmIR& ir;
    Slot top;
    // slots that closures capture, which Lua code can change through
    // open upvalues
    std::vector<Slot> captured;
    Node* node = nullptr;
};

} // namespace luamura

#endif
