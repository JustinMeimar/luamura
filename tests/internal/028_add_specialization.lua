local function add(a, b)
    return a + b
end

local function alias_left(a, b)
    a = a + b
    return a
end

local function alias_right(a, b)
    b = a + b
    return b
end

local function add_immediate(a)
    return a + 1
end

local function check(value, expected, kind)
    assert(value == expected)
    assert(math.type(value) == kind)
end

for _, f in ipairs({add, alias_left, alias_right}) do
    check(f(20, 22), 42, "integer")
    check(f(math.maxinteger, 1), math.mininteger, "integer")
    check(f(math.mininteger, -1), math.maxinteger, "integer")
    check(f(20, 22.0), 42.0, "float")
    check(f(20.0, 22), 42.0, "float")
    check(f(20.5, 21.5), 42.0, "float")
    check(f("20.5", 22), 42.5, "float")

    local calls = 0
    local object = setmetatable({}, {
        __add = function(a, b)
            calls = calls + 1
            assert(a == 7 or b == 7)
            return 99
        end,
    })
    check(f(object, 7), 99, "integer")
    check(f(7, object), 99, "integer")
    assert(calls == 2)
    assert(not pcall(f, nil, 1))
    assert(not pcall(f, 1, {}))
end

check(add_immediate(41), 42, "integer")
check(add_immediate(math.maxinteger), math.mininteger, "integer")
check(add_immediate(41.0), 42.0, "float")
check(add_immediate("41.5"), 42.5, "float")
local calls = 0
local object = setmetatable({}, {
    __add = function(_, b)
        calls = calls + 1
        assert(b == 1)
        return 42
    end,
})
check(add_immediate(object), 42, "integer")
assert(calls == 1)
assert(not pcall(add_immediate, false))
print("addition cases passed")
