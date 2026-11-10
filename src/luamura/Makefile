# Makefile for building luamura
# Builds Lua with LUAMURA defined, luamura and its runtime in $(BUILD).

BUILD= ../../build/release
LLVM_CONFIG= llvm-config

CC= gcc -std=gnu99
CXX= g++
CLANG= clang -std=gnu99
OPT= -O2
CFLAGS= $(OPT) -g -Wall -DLUAMURA $(SYSCFLAGS) $(MYCFLAGS)
CXXFLAGS= $(OPT) -g -Wall -DLUAMURA -I.. \
	--embed-dir=$(OBJ)/lua $(shell $(LLVM_CONFIG) --cxxflags) $(MYCFLAGS)
LIBS= $(shell $(LLVM_CONFIG) --ldflags \
	--libs core support passes bitreader linker) \
	-lm -ldl

# As Lua's Makefile sets them for Linux.
SYSCFLAGS= -DLUA_USE_LINUX
SYSLIBS= -Wl,-E -ldl

MYCFLAGS=

OBJ= $(BUILD)/obj
LUA_O= $(patsubst ../%.c,$(OBJ)/lua/%.o, \
	$(filter-out ../lua.c ../luac.c,$(wildcard ../*.c)))
LUA_A= $(BUILD)/liblua.a
# lvm.c and ltable.c as bitcode, which luamura embeds to inline them
BITCODE= $(OBJ)/lua/lvm.bc $(OBJ)/lua/ltable.bc
LUA_T= $(BUILD)/lua
LUAMURA_T= $(BUILD)/luamura
LUAMURA_O= $(addprefix $(OBJ)/luamura/, \
	luamura.o lm_ir.o lm_effects.o lm_hot_paths.o lm_lower.o lm_codegen.o)
RT_O= $(BUILD)/lm_rt.o

TESTS= ../../tests
INTERNAL= $(wildcard $(TESTS)/internal/*.lua)
UPSTREAM= $(filter-out %/all.lua %/main.lua %/heavy.lua, \
	$(wildcard $(TESTS)/lua-test/*.lua))

# Upstream files check themselves and print timings and random seeds, so
# only their exit status is compared. all.lua drives the others, main.lua
# tests lua.c's command line and heavy.lua exhausts memory.
COMPARE_internal= output
COMPARE_lua-test= status

all: $(LUAMURA_T) $(RT_O)

$(OBJ)/lua $(OBJ)/luamura:
	mkdir -p $@

$(OBJ)/lua/%.o: ../%.c $(wildcard ../*.h) | $(OBJ)/lua
	$(CC) $(CFLAGS) -c -o $@ $<

# Always optimized, which inlines each instruction's body into its
# function.
$(OBJ)/lua/%.bc: ../%.c $(wildcard ../*.h) | $(OBJ)/lua
	$(CLANG) -O2 -DLUAMURA $(SYSCFLAGS) -emit-llvm -c -o $@ $<

$(LUA_A): $(LUA_O)
	$(AR) rcs $@ $^

$(LUA_T): $(OBJ)/lua/lua.o $(LUA_A)
	$(CC) -o $@ $^ $(SYSLIBS) -lm

$(LUAMURA_T): $(LUAMURA_O) $(LUA_A)
	$(CXX) -o $@ $^ $(LIBS)

$(OBJ)/luamura/%.o: %.cpp $(wildcard *.h ../*.h) | $(OBJ)/luamura
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(RT_O): lm_rt.c | $(OBJ)/luamura
	$(CC) $(CFLAGS) -I.. -c -o $@ $<

check: check-internal check-lua-test

check-internal: $(INTERNAL:$(TESTS)/%.lua=%.run)

check-lua-test: $(UPSTREAM:$(TESTS)/%.lua=%.run)

%.run: $(LUAMURA_T) $(RT_O) $(LUA_T)
	@log=$$(mktemp); \
	LUAMURA_COMPARE=$(COMPARE_$(*D)) timeout 30 \
		$(TESTS)/run_case.sh $(abspath $(LUAMURA_T) $(LUA_T)) \
		$(TESTS)/$*.lua $(abspath $(RT_O) $(LUA_A)) $(SYSLIBS) \
		> $$log 2>&1; \
	status=$$?; \
	if [ $$status -eq 0 ]; then echo "ok   $*"; \
	else cat $$log; echo "FAIL $*"; fi; \
	rm -f $$log; \
	exit $$status

clean:
	$(RM) -r $(BUILD)

.PHONY: all check check-internal check-lua-test clean

$(OBJ)/luamura/luamura.o: $(BITCODE)
$(RT_O): ../lstate.h ../lua.h ../luaconf.h ../lobject.h \
 ../llimits.h ../ltm.h ../lzio.h ../lmem.h ../lauxlib.h ../ldebug.h \
 ../lfunc.h ../lualib.h
