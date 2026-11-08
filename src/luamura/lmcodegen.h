#ifndef LMCODEGEN_H
#define LMCODEGEN_H

#include "lmlua.h"

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

#include <memory>
#include <string>

// The baseline compiler. Each function of the chunk becomes a native
// function that runs its instructions on the Lua stack: some inline, the
// others by calling lvm.c's function for the instruction.
std::unique_ptr<llvm::Module> compile(llvm::LLVMContext& context,
                                      const Proto* main,
                                      const std::string& chunk);

#endif // LMCODEGEN_H
