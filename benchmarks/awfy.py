"""Bundle the pinned AWFY Lua modules into AOT compilation units."""

from pathlib import Path
import re


SOURCE = Path(__file__).resolve().parent / "awfy"

# Inner iterations from upstream rebench.conf's BENCHMARKS list.
BENCHMARKS = {
    "Bounce": 1500,
    "CD": 250,
    "DeltaBlue": 12000,
    "Havlak": 1500,
    "Json": 100,
    "List": 1500,
    "Mandelbrot": 500,
    "NBody": 250000,
    "Permute": 1000,
    "Queens": 1000,
    "Richards": 100,
    "Sieve": 3000,
    "Storage": 1000,
    "Towers": 600,
}


def module_source(name):
    text = (SOURCE / f"{name}.lua").read_text()
    if name in {"som", "richards"}:
        operators = {"band": "&", "bxor": "~"}
        if name == "som":
            operators["rshift"] = ">>"
        for function, operator in operators.items():
            body = f"function (a, b) return a {operator} b end"
            original = f"assert(load'--[[{function}]] return {body}')()"
            if text.count(original) != 1:
                raise ValueError(f"unexpected AWFY helper: {name}/{function}")
            text = text.replace(original, body)
    return text


def driver(name):
    return f"""
local benchmark = require('{name.lower()}')
local iterations = assert(tonumber(arg[1]))
for i = 1, iterations do
    assert(benchmark:inner_benchmark_loop({BENCHMARKS[name]}),
           'AWFY {name}: incorrect result')
end
print('AWFY {name}: ok')
"""


def bundle(name):
    modules = {}

    def visit(module):
        if module in modules:
            return
        text = module_source(module)
        modules[module] = text
        for dependency in re.findall(r"require\s*['\"]([^'\"]+)['\"]", text):
            if (SOURCE / f"{dependency}.lua").exists():
                visit(dependency)
            elif dependency != "bit":
                raise ValueError(f"unknown AWFY dependency: {dependency}")

    visit(name.lower())
    parts = ["-- Generated from pinned AWFY sources; see benchmarks/README.md."]
    for module, text in sorted(modules.items()):
        parts.append(f"package.preload['{module}'] = function(...)\n{text}\nend")
    parts.append("""
package.searchers = {package.searchers[1]}
local function no_dynamic_code()
    error('AWFY bundle attempted to load dynamic code')
end
load = no_dynamic_code
loadfile = no_dynamic_code
dofile = no_dynamic_code
""")
    parts.append(driver(name))
    return "\n".join(parts)
