#ifndef EMIT_H
#define EMIT_H

#include "lua_headers.h"

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

#include <memory>
#include <string>

// Emits a module holding the binary chunk of main and the compiled
// implementations of main and its nested functions, in preorder.
std::unique_ptr<llvm::Module> emit_module(llvm::LLVMContext& context,
                                          const Proto* main,
                                          const std::string& chunk);

#endif // EMIT_H
