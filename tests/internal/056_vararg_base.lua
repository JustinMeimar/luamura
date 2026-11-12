local function expand(x, ...)
    local y = x + 1
    local t = {...}
    local a, b, c = ...
    return y + x + #t + a + c
end
local args = {}
for i = 1, 5000 do args[i] = i end
for n = 1, 3 do
    print(expand(n, table.unpack(args)))
end
