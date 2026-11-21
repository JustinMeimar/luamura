local i = 0
while i < 5 do i = i + 1 end
print(i)

local sum = 0
local n = 0
while true do
    n = n + 1
    if n > 10 then break end
    if n % 2 == 0 then sum = sum + n end
end
print(n, sum)

local k = 0
repeat
    local doubled = k * 2
    k = k + 1
until doubled >= 6
print(k)

repeat print("once") until true

local outer = 0
while outer < 3 do
    outer = outer + 1
    local inner = 0
    while true do
        inner = inner + 1
        if inner == outer then break end
    end
    print(outer, inner)
end

local j = 0
while j < 3 do
    j = j + 1
    local captured = j
    local function get() return captured end
    if j == 1 then f1 = get elseif j == 2 then f2 = get else f3 = get end
end
print(f1(), f2(), f3())

local function find(limit)
    local x = 0
    while true do
        x = x + 1
        if x * x > limit then return x end
    end
end
print(find(50))
