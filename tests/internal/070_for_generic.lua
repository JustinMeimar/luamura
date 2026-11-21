local function range(n)
    return function(_, i)
        if i < n then return i + 1 end
    end, nil, 0
end
for i in range(3) do print(i) end
for i in range(0) do print("never") end

local function pairs_of(n)
    return function(limit, i)
        if i < limit then return i + 1, (i + 1) * (i + 1), "extra" end
    end, n, 0
end
for i, square in pairs_of(4) do print(i, square) end
for i, square, extra, missing in pairs_of(1) do print(i, square, extra, missing) end

local function falsy()
    local done = false
    return function()
        if done then return nil end
        done = true
        return false
    end
end
for v in falsy() do print("got", v) end

for i in range(10) do
    if i > 2 then break end
    for j in range(i) do print(i, j) end
end

local first, second
for i in range(2) do
    if i == 1 then first = function() return i end else second = function() return i end end
end
print(first(), second())

local function find(n)
    for i in range(100) do
        if i * i >= n then return i end
    end
end
print(find(50))

local calls = 0
local function counted()
    calls = calls + 1
    return range(2)
end
for i in counted() do end
print(calls)
