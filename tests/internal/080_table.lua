local t = {10, 20, 30}
print(t[1], t[2], t[3], t[4], #t)
t[4] = 40
print(t[4], #t)

local point = {x = 1, y = 2, ["z"] = 3}
print(point.x, point.y, point.z, point.w)
point.x = 100
point["y"] = point.y + 1
print(point.x, point.y)

local nested = {inner = {value = "deep"}}
print(nested.inner.value)
nested.inner.value = "changed"
print(nested.inner.value)

local keys = {}
keys[1.0] = "one"
keys[2] = "two"
keys[true] = "yes"
keys[keys] = "self"
print(keys[1], keys[2.0], keys[true], keys[keys])

local list = {}
for i = 1, 100 do list[i] = i * i end
print(#list, list[50], list[100])
list[100] = nil
print(#list)

local sum = 0
for i, v in ipairs({5, 6, 7}) do sum = sum + i * v end
print(sum)

local counted = 0
local total = 0
for k, v in pairs({a = 1, b = 2, c = 3, 4, 5}) do
    counted = counted + 1
    total = total + v
end
print(counted, total)

local function three() return 1, 2, 3 end
local packed = {three()}
print(#packed, packed[3])
local truncated = {three(), three()}
print(#truncated, truncated[1], truncated[2], truncated[4])
local wrapped = {(three())}
print(#wrapped)
local function pack(...) return {...} end
print(#pack(), #pack(nil, 2) >= 0, #pack(1, 2, 3, 4))

local overwrite = {[1] = "keyed", "positional"}
print(overwrite[1])

local obj = {name = "obj"}
function obj.greet(self, greeting) return greeting .. ", " .. self.name end
function obj:shout() return self:greet("HEY") end
print(obj:greet("hi"), obj:shout())

local a, b = {}, {}
a.x, b.x = 1, 2
print(a.x, b.x)
local swap = {1, 2}
swap[1], swap[2] = swap[2], swap[1]
print(swap[1], swap[2])

print(rawget(point, "x"), rawlen({1, 2}), rawequal(a, a), rawequal(a, b))
print(rawset(a, "y", 5) == a, a.y)
print(next({}), select("#", next({})))
print(type({}), {} == {}, a == a)
print(_G.print == print, _G._G == _G)
global_value = 42
print(_G.global_value)

local grid = {}
for i = 1, 3 do
    grid[i] = {}
    for j = 1, 3 do grid[i][j] = i * j end
end
print(grid[2][3], grid[3][3])

local big = {}
for i = 1, 1000 do big["k" .. i] = i end
local check = 0
for k, v in pairs(big) do check = check + v end
print(check, big.k500)
for i = 1, 1000, 2 do big["k" .. i] = nil end
check = 0
for k, v in pairs(big) do check = check + v end
print(check)
