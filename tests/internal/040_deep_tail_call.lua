local function count(n, acc)
    if n == 0 then return acc end
    return count(n - 1, acc + 1)
end
print(count(1000000, 0))

local is_odd
local function is_even(n)
    if n == 0 then return true end
    return is_odd(n - 1)
end
is_odd = function(n)
    if n == 0 then return false end
    return is_even(n - 1)
end
print(is_even(1000001), is_odd(1000001))
