# Makefile for building luamura
# Lua's own Makefile builds liblua.a and the reference lua in ../, with
# LUAMURA defined.

PLAT= guess
LLVM_CONFIG= llvm-config

CC= gcc -std=gnu11
CXX= g++
CFLAGS= -O2 -g -Wall -DLUAMURA -I.. $(MYCFLAGS)
CXXFLAGS= -O2 -g -Wall -DLUAMURA -I.. \
	$(shell $(LLVM_CONFIG) --cxxflags) $(MYCFLAGS)
LIBS= $(shell $(LLVM_CONFIG) --ldflags --libs core support passes) \
	-lm -ldl

MYCFLAGS=
LUA_CFLAGS= -DLUAMURA -Wno-unused-parameter -Wno-unused-but-set-parameter

LUA_A= ../liblua.a
LUA_T= ../lua
LUAMURA_T= luamura
LUAMURA_O= lmc.o lmcfg.o lmssa.o lmbuilder.o lmcodegen.o
RT_O= lmrt.o

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

$(LUA_A): FORCE
	@$(MAKE) --no-print-directory -C .. $(PLAT) ALL=a \
		MYCFLAGS="$(LUA_CFLAGS)"

$(LUA_T): $(LUA_A) FORCE
	@$(MAKE) --no-print-directory -C .. $(PLAT) ALL=lua \
		MYCFLAGS="$(LUA_CFLAGS)"

$(LUAMURA_T): $(LUAMURA_O) $(LUA_A)
	$(CXX) -o $@ $(LUAMURA_O) $(LUA_A) $(LIBS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $<

%.o: %.c
	$(CC) $(CFLAGS) -c $<

check: check-internal check-lua-test

check-internal: $(INTERNAL:$(TESTS)/%.lua=%.run)

check-lua-test: $(UPSTREAM:$(TESTS)/%.lua=%.run)

%.run: $(LUAMURA_T) $(RT_O) $(LUA_T)
	@log=$$(mktemp); \
	LUAMURA_COMPARE=$(COMPARE_$(*D)) timeout 30 \
		$(TESTS)/run_case.sh $(abspath $(LUAMURA_T) $(LUA_T)) \
		$(TESTS)/$*.lua $(abspath $(RT_O) $(LUA_A)) -Wl,-E -ldl \
		> $$log 2>&1; \
	status=$$?; \
	if [ $$status -eq 0 ]; then echo "ok   $*"; \
	else cat $$log; echo "FAIL $*"; fi; \
	rm -f $$log; \
	exit $$status

clean:
	$(RM) $(LUAMURA_T) $(LUAMURA_O) $(RT_O)
	@$(MAKE) -C .. clean

.PHONY: all check check-internal check-lua-test clean FORCE

lmc.o: lmc.cpp lmcodegen.h lmlua.h ../lauxlib.h ../luaconf.h ../lua.h \
 ../lfunc.h ../lobject.h ../llimits.h ../lopcodes.h ../lstate.h ../ltm.h \
 ../lzio.h ../lmem.h lmssa.h
lmcfg.o: lmcfg.cpp lmcfg.h lmlua.h ../lauxlib.h ../luaconf.h ../lua.h \
 ../lfunc.h ../lobject.h ../llimits.h ../lopcodes.h ../lstate.h ../ltm.h \
 ../lzio.h ../lmem.h
lmssa.o: lmssa.cpp lmssa.h lmlua.h ../lauxlib.h ../luaconf.h ../lua.h \
 ../lfunc.h ../lobject.h ../llimits.h ../lopcodes.h ../lstate.h ../ltm.h \
 ../lzio.h ../lmem.h lmcfg.h ../lopnames.h
lmbuilder.o: lmbuilder.cpp lmbuilder.h lmlua.h ../lauxlib.h ../luaconf.h \
 ../lua.h ../lfunc.h ../lobject.h ../llimits.h ../lopcodes.h ../lstate.h \
 ../ltm.h ../lzio.h ../lmem.h
lmcodegen.o: lmcodegen.cpp lmcodegen.h lmlua.h ../lauxlib.h ../luaconf.h \
 ../lua.h ../lfunc.h ../lobject.h ../llimits.h ../lopcodes.h ../lstate.h \
 ../ltm.h ../lzio.h ../lmem.h lmbuilder.h lmcfg.h ../lopnames.h
lmrt.o: lmrt.c luamura.h ../lstate.h ../lua.h ../luaconf.h ../lobject.h \
 ../llimits.h ../ltm.h ../lzio.h ../lmem.h ../lauxlib.h ../ldebug.h \
 ../lfunc.h ../lualib.h
