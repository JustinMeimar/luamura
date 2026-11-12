local function chain(a, b)
    return (a + b) * b - a
end

local function live_results(a, b)
    local x = a + b
    local y = x * b
    local z = y - a
    return x, y, z, a, b
end

local function overwritten(a, b)
    a = a + b
    b = a * b
    a = b - a
    return a, b
end

local function unknown(a, b, c)
    return ((a + b) * c) - a
end

local function long_chain(a)
    a = a + 1
    a = a + 1
    a = a + 1
    a = a + 1
    a = a + 1
    a = a + 1
    a = a + 1
    a = a + 1
    a = a + 1
    a = a + 1
    return a
end

for _, pair in ipairs({{7, 17}, {7.5, 17}, {7, 17.5},
                      {math.maxinteger, 3}, {math.mininteger, -1}}) do
    local a, b = pair[1], pair[2]
    print(chain(a, b), math.type(chain(a, b)))
    print(live_results(a, b))
    print(overwritten(a, b))
    print(unknown(a, b, 2.5))
    print(long_chain(a))
end

local events = {}
local function object(value)
    return setmetatable({value = value}, {
        __add = function(a, b)
            events[#events + 1] = "add"
            return object(a.value + b.value)
        end,
        __mul = function(a, b)
            events[#events + 1] = "mul"
            return object(a.value * b.value)
        end,
        __sub = function(a, b)
            events[#events + 1] = "sub"
            return object(a.value - b.value)
        end,
    })
end
assert(chain(object(7), object(17)).value == 401)
assert(table.concat(events, ",") == "add,mul,sub")

local calls = 0
local multiplier = setmetatable({}, {
    __mul = function(a)
        calls = calls + 1
        return a * 2
    end,
})
assert(unknown(7, 17, multiplier) == 41)
assert(calls == 1)

local sum = 0
for i = 1, 100 do
    local a = i % 2 == 0 and i or i + 0.5
    sum = sum + chain(a, 3)
end
print(sum)
assert(not pcall(chain, nil, 3))
