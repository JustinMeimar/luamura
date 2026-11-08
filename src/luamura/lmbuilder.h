#ifndef LMBUILDER_H
#define LMBUILDER_H

#include "lmlua.h"

#include <llvm/IR/IRBuilder.h>

#include <cstddef>

// A TValue loaded from memory: its value as 64 bits, and its tag.
struct Operand {
    llvm::Value* value;
    llvm::Value* tag;
};

// An IRBuilder that knows Lua's object layout.
class Builder : public llvm::IRBuilder<> {
public:
    explicit Builder(llvm::LLVMContext& context);

    llvm::Value* offset(llvm::Value* address, size_t bytes);

    Operand load(llvm::Value* tvalue);
    void store(Operand v, llvm::Value* tvalue);
    // Replaces the value and keeps the tag, as chgivalue does.
    void store_value(llvm::Value* v, llvm::Value* tvalue);

    Operand int_operand(llvm::Value* v);
    Operand float_operand(llvm::Value* v);
    Operand tag_operand(int tag);

    llvm::Value* has_tag(Operand v, int tag);
    llvm::Value* is_number(Operand v);
    llvm::Value* is_false(Operand v);
    // A number as a double, converting integers.
    llvm::Value* to_double(Operand v);
    llvm::Value* as_double(llvm::Value* v);

    llvm::PointerType* t_ptr;
    llvm::IntegerType* t_i8;
    llvm::IntegerType* t_i32;
    llvm::IntegerType* t_i64;
    llvm::Type* t_double;
};

#endif // LMBUILDER_H
