local function depth(n) return depth(n + 1) + 1 end
print((pcall(depth, 1)))
print((pcall(depth, 1)))

local function sum(n) if n == 0 then return 0 end return n + sum(n - 1) end
print(sum(10000))

local function nest(n) if n == 0 then return "bottom" end return (pcall(nest, n - 1)) end
print(nest(100))
print("recovered")
