local function add(a, b) return a + b end
local function twice(f, x) return f(f(x, x), x) end
print(twice(add, 3))

local function wrap(v) return tostring(v) end
print(wrap(42), wrap(nil))

local n = 0
local function bump() n = n + 1 return n end
local function via() return bump() end
print(via(), via(), n)

local function nothing() end
local function forward() return nothing() end
print(forward())

local function first() return (add(1, 2)) end
print(first())

local function more(a, b, c) return a + b + c end
local function fewer(x) return more(x, x, x) end
print(fewer(4))

local function call_nil() return undefined_global(1) end
print("before")
call_nil()
