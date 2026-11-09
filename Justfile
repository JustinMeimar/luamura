# Use `just profile=debug build` for an unoptimized compiler and runtime.
set shell := ["bash", "-euo", "pipefail", "-c"]
set positional-arguments

profile := "release"
opt := if profile == "release" {
    "-O2"
} else if profile == "debug" {
    "-O0"
} else {
    error("profile must be release or debug")
}
build_dir := "build" / profile
out := build_dir / "out"
make := "make --no-print-directory -C src/luamura -j" + num_cpus() \
    + " BUILD=../../" + build_dir + " OPT=" + opt

default:
    @just --list

# Build the compiler, runtime and interpreter; update compile_commands.json.
build:
    bear --append --output compile_commands.json -- {{make}} all ../../{{build_dir}}/lua

# Test all, internal, lua-test, or a case such as internal/010_base.
test target="all": build
    #!/usr/bin/env bash
    set -euo pipefail
    case "$1" in
        all)      target=check ;;
        internal) target=check-internal ;;
        lua-test) target=check-lua-test ;;
        *)        target="$1.run" ;;
    esac
    {{make}} -k "$target"

# Compile a Lua program to build/<profile>/out/<name>.
compile file: build
    #!/usr/bin/env bash
    set -euo pipefail
    exe={{quote(out / file_stem(file))}}
    mkdir -p {{quote(out)}}
    {{build_dir}}/luamura "$1" "$exe.ll"
    llc {{opt}} --relocation-model=pic -filetype=obj "$exe.ll" -o "$exe.o"
    cc "$exe.o" {{build_dir}}/lm_rt.o {{build_dir}}/liblua.a \
        -Wl,-E -ldl -lm -o "$exe"

# Compile and run a Lua program.
run file *args: (compile file)
    {{quote(out / file_stem(file))}} "${@:2}"

# Run with the reference interpreter.
interpret file *args: build
    {{build_dir}}/lua "$@"

# Print the generated LLVM IR.
ir file: build
    {{build_dir}}/luamura "$1" /dev/stdout

# Debug a compiled program.
debug file *args:
    just profile=debug compile "$1"
    gdb --args {{quote("build/debug/out" / file_stem(file))}} "${@:2}"

# Debug the compiler while compiling a program.
debug-compiler file:
    just profile=debug build
    mkdir -p build/debug/out
    gdb --args build/debug/luamura "$1" \
        {{quote("build/debug/out" / file_stem(file) + ".ll")}}

clean:
    rm -rf build
