# luamura: build, test, compile and debug Lua programs.
# profile is release (-O2) or debug (-O0); each builds in build/<profile>.

set shell := ["bash", "-euo", "pipefail", "-c"]

make := "make --no-print-directory -C src/luamura -j" + num_cpus()

default:
    @just --list

# build luamura, its runtime and Lua, recording compile_commands.json
build profile="release":
    bear --append --output compile_commands.json -- {{make}} BUILD=../../build/{{profile}} OPT={{ if profile == "debug" { "-O0" } else { "-O2" } }}

# run tests: all, internal, lua-test, or one test such as internal/010_base
test target="all" profile="release": (build profile)
    {{make}} -k BUILD=../../build/{{profile}} {{ if target == "all" { "check" } else if target == "internal" { "check-internal" } else if target == "lua-test" { "check-lua-test" } else { target + ".run" } }}

# compile a Lua program to build/<profile>/out/<name>
compile file profile="release": (build profile)
    mkdir -p build/{{profile}}/out
    build/{{profile}}/luamura {{file}} build/{{profile}}/out/{{file_stem(file)}}.ll
    llc {{ if profile == "debug" { "-O0" } else { "-O2" } }} --relocation-model=pic -filetype=obj \
        build/{{profile}}/out/{{file_stem(file)}}.ll -o build/{{profile}}/out/{{file_stem(file)}}.o
    cc build/{{profile}}/out/{{file_stem(file)}}.o build/{{profile}}/lm_rt.o build/{{profile}}/liblua.a \
        -Wl,-E -ldl -lm -o build/{{profile}}/out/{{file_stem(file)}}

# compile and run a Lua program
run file *args: (compile file)
    build/release/out/{{file_stem(file)}} {{args}}

# run a Lua program with the reference interpreter
interpret file *args: (build)
    {{make}} ../../build/release/lua
    build/release/lua {{file}} {{args}}

# print the LLVM IR luamura emits for a program
ir file: (build)
    build/release/luamura {{file}} /dev/stdout

# debug a compiled program in gdb
debug file *args: (compile file "debug")
    gdb --args build/debug/out/{{file_stem(file)}} {{args}}

# debug the compiler itself in gdb, compiling a program
debug-compiler file: (build "debug")
    mkdir -p build/debug/out
    gdb --args build/debug/luamura {{file}} build/debug/out/{{file_stem(file)}}.ll

clean:
    rm -rf build
