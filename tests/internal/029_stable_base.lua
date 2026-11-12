local function chain(a, b)
    return (a + b) * b - a
end

assert(chain(7, 17) == 401)
assert(chain(7.0, 17) == 401.0)
assert(math.type(chain(7.0, 17)) == "float")
assert(chain(math.maxinteger, 1) == 1)

local function grow(depth)
    if depth == 0 then return 1 end
    local result = grow(depth - 1)
    return result + 1
end

local function across_call(a, b)
    local before = a + b
    local depth = grow(300)
    return before * b - a + depth
end
assert(across_call(7, 17) == 702)

local values = {}
for i = 1, 6000 do values[i] = i end
local function across_c_call(a, b)
    local before = a + b
    local first, second = table.unpack(values)
    return before * b - a + first + second
end
assert(across_c_call(7, 17) == 404)

local function adjusted(a, b, ...)
    local before = a + b
    local n = select("#", ...)
    return before * b - a + n
end
assert(adjusted(7, 17, 1, 2, 3) == 404)

local function across_metamethod()
    local a = 7
    local object = setmetatable({}, {
        __add = function()
            grow(400)
            a = 11
            collectgarbage("collect")
            return 24
        end,
    })
    local before = a + object
    return before * 17 - a
end
assert(across_metamethod() == 397)

local co = coroutine.create(function(a, b)
    local before = a + b
    coroutine.yield(before)
    return before * b - a
end)
local ok, value = coroutine.resume(co, 7, 17)
assert(ok and value == 24)
ok, value = coroutine.resume(co)
assert(ok and value == 401)
print("stable base cases passed")
