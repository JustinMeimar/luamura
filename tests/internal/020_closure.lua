local function counter()
    local n = 0
    return function()
        n = n + 1
        return n
    end
end
local c1 = counter()
local c2 = counter()
print(c1(), c1(), c2(), c1())

local x = 1
local function get() return x end
x = 2
print(get())

local function outer()
    local a = 10
    local function middle()
        return function() return a end
    end
    a = a + 1
    return middle()
end
print(outer()())

local function add(a, b) return a + b end
print(add(1, 2))

local function self_ref() return self_ref end
print(self_ref() == self_ref)

local function set(v) x = v end
set(5)
print(x)

g = function() return "global" end
print(g())
print(type(counter), tostring(nil))
