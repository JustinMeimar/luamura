for i = 1, 3 do print(i) end
for i = 3, 1, -1 do print(i) end
for i = 1, 0 do print("never") end
for i = 1, 10, 4 do print(i) end
for i = 10, 1, -4 do print(i) end
for i = 1.5, 3 do print(i) end
for i = 1, 3.7 do print(i) end
for i = 1, 2, 0.5 do print(i) end
for i = 3, 1.2, -1 do print(i) end
for i = 9223372036854775806, 9223372036854775807 do print(i) end
for i = -9223372036854775807, -9223372036854775808, -1 do print(i) end
for i = 1, 1e300 do if i > 2 then break end print(i) end
for i = 1, -1e300 do print("never") end
for i = 1, 3 do print(1, 3) local i = i * 10 print(i) end

local sum = 0
for i = 1, 100 do sum = sum + i end
print(sum)

local a, b
for i = 1, 2 do
    local function get() return i end
    if i == 1 then a = get else b = get end
end
print(a(), b())

local function nested()
    local count = 0
    for i = 1, 4 do
        for j = i, 4 do
            if j == 3 then break end
            count = count + 1
        end
    end
    return count
end
print(nested())

local function first_square_over(n)
    for i = 1, n do
        if i * i > n then return i end
    end
end
print(first_square_over(30))
for i = 1, 2, 0 do end
