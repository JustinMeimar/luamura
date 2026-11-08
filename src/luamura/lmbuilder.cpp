#include "lmbuilder.h"

Builder::Builder(llvm::LLVMContext& context)
    : llvm::IRBuilder<>(context),
      t_ptr(llvm::PointerType::getUnqual(context)), t_i8(getInt8Ty()),
      t_i32(getInt32Ty()), t_i64(getInt64Ty()), t_double(getDoubleTy()) {}

llvm::Value* Builder::offset(llvm::Value* address, size_t bytes) {
    return CreateConstInBoundsGEP1_64(t_i8, address, bytes);
}

Operand Builder::load(llvm::Value* tvalue) {
    return {CreateLoad(t_i64, tvalue),
            CreateLoad(t_i8, offset(tvalue, offsetof(TValue, tt_)))};
}

void Builder::store(Operand v, llvm::Value* tvalue) {
    CreateStore(v.value, tvalue);
    CreateStore(v.tag, offset(tvalue, offsetof(TValue, tt_)));
}

void Builder::store_value(llvm::Value* v, llvm::Value* tvalue) {
    CreateStore(v, tvalue);
}

Operand Builder::int_operand(llvm::Value* v) {
    return {v, getInt8(LUA_VNUMINT)};
}

Operand Builder::float_operand(llvm::Value* v) {
    return {CreateBitCast(v, t_i64), getInt8(LUA_VNUMFLT)};
}

Operand Builder::tag_operand(int tag) {
    return {getInt64(0), getInt8(tag)};
}

llvm::Value* Builder::has_tag(Operand v, int tag) {
    return CreateICmpEQ(v.tag, getInt8(tag));
}

llvm::Value* Builder::is_number(Operand v) {
    return CreateOr(has_tag(v, LUA_VNUMINT), has_tag(v, LUA_VNUMFLT));
}

llvm::Value* Builder::is_false(Operand v) {
    auto* nil = CreateICmpEQ(CreateAnd(v.tag, 0x0F), getInt8(LUA_TNIL));
    return CreateOr(has_tag(v, LUA_VFALSE), nil);
}

llvm::Value* Builder::to_double(Operand v) {
    return CreateSelect(has_tag(v, LUA_VNUMINT),
                        CreateSIToFP(v.value, t_double),
                        as_double(v.value));
}

llvm::Value* Builder::as_double(llvm::Value* v) {
    return CreateBitCast(v, t_double);
}
