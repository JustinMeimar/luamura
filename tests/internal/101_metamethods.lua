local V = {}
V.__index = V
local function vec(x, y) return setmetatable({x = x, y = y}, V) end
V.__add = function(a, b) return vec(a.x + b.x, a.y + b.y) end
V.__sub = function(a, b) return vec(a.x - b.x, a.y - b.y) end
V.__mul = function(a, b)
    if type(a) == "number" then return vec(a * b.x, a * b.y) end
    if type(b) == "number" then return vec(a.x * b, a.y * b) end
    return a.x * b.x + a.y * b.y
end
V.__unm = function(a) return vec(-a.x, -a.y) end
V.__eq = function(a, b) return a.x == b.x and a.y == b.y end
V.__lt = function(a, b) return a.x < b.x end
V.__le = function(a, b) return a.x <= b.x end
V.__len = function(a) return 2 end
V.__concat = function(a, b)
    local function s(v) return type(v) == "table" and ("(" .. v.x .. "," .. v.y .. ")") or tostring(v) end
    return s(a) .. s(b)
end
V.__div = function(a, b) return "div" end
V.__mod = function(a, b) return "mod" end
V.__idiv = function(a, b) return "idiv" end
V.__pow = function(a, b) return "pow" end
V.__band = function(a, b) return "band" end
V.__bor = function(a, b) return "bor" end
V.__bxor = function(a, b) return "bxor" end
V.__shl = function(a, b) return "shl" end
V.__shr = function(a, b) return "shr" end
V.__bnot = function(a) return "bnot" end

local a, b = vec(1, 2), vec(3, 4)
local c = a + b
print(c.x, c.y, (b - a).x, (a * 2).y, (2 * a).x, a * b, (-a).x)
print(a == vec(1, 2), a ~= vec(1, 2), a == b, a < b, a <= b, a > b, a >= b)
print(#a, a .. b, a .. "!", "v=" .. a, 1 .. a)
print(a / b, a % 1, a // b, 2 ^ a, a & 1, 1 | a, a ~ a, a << 1, a >> 1, ~a)

local other = setmetatable({}, {__eq = function() return true end})
print(a == other, other == a, a == 1)
local same = setmetatable({}, {__eq = function() error("not called") end})
print(same == same)

local counter = setmetatable({}, {__index = function(t, k) return 0 end, __len = function() return "len" end})
print(#counter, counter.anything)
print((pcall(function() return {} + 1 end)))
print((pcall(function() return {} < {} end)))
print((pcall(function() return {} .. "x" end)))
