local function none() end
local function one() return 1 end
local function three() return 1, 2, 3 end

print(none())
print(one())
print(three())
print(three(), three())
print((three()))
print(none(), "end")
print("start", none())

local a, b, c, d = three()
print(a, b, c, d)
local e, f = 10
print(e, f)

local calls = 0
local function count() calls = calls + 1 return calls end
local g = 1, count(), count()
print(g, calls)

local x, y = 1, 2
x, y = y, x
print(x, y)
gx, gy = three()
print(gx, gy)

local function prefix() return "p", three() end
print(prefix())
local function tail() return three() end
print(tail())
local function middle() return three(), "last" end
print(middle())

local function add3(p, q, r) return p + q + r end
print(add3(three()))
print(add3(1, three()))
print(select_like == nil, add3(10, one(), (three())))

local function args(p, q, r, s) return p, q, r, s end
print(args(three()))
print(args(0, three()))
print(args(three(), 0))
