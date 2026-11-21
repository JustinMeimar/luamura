local function count(...) return select("#", ...) end
print(count(), count(nil), count(1, nil, nil))

local function pass(...) return ... end
print(pass())
print(pass(1, 2, 3))
print(pass(1, nil, 3))
print((pass(1, 2, 3)))

local function first(a, ...) return a, select("#", ...) end
print(first())
print(first(1))
print(first(1, 2, 3))

local function locals(...)
    local a, b, c = ...
    return c, b, a
end
print(locals(1, 2))

local function append(...) return "x", ... end
print(append(1, 2))
local function middle(...) return ..., "x" end
print(middle(1, 2))

local function forward(a, b, ...) return pass(b, a, ...) end
print(forward(1, 2, 3, 4))
local function forward_tail(a, ...) return pass(...) end
print(forward_tail(1, 2, 3))
local function shift(a, b, c, ...) return pass(c, b, a, ...) end
print(shift(1, 2, 3, 4, 5))

print(select(2, "a", "b", "c"))
print(select(-1, "a", "b", "c"))
print(select(5, "a", "b"))

local function closure(...)
    local n = select("#", ...)
    return function() return n end
end
print(closure(1, 2, 3)())
print(select("#", ...))
local function grow(a, ...) return pass(a, a, a, ...) end
print(grow(1, 2, 3, 4))
