local base = {greet = function(self) return "hello from " .. self.name end, kind = "base"}
local obj = setmetatable({name = "obj"}, {__index = base})
print(obj:greet(), obj.kind, obj.missing, rawget(obj, "kind"))

local Animal = {}
Animal.__index = Animal
function Animal.new(name, sound) return setmetatable({name = name, sound = sound}, Animal) end
function Animal:speak() return self.name .. " says " .. self.sound end
local Dog = setmetatable({}, {__index = Animal})
Dog.__index = Dog
function Dog.new(name) local d = Animal.new(name, "woof") return setmetatable(d, Dog) end
function Dog:fetch() return self.name .. " fetches" end
local d = Dog.new("rex")
print(d:speak(), d:fetch(), getmetatable(d) == Dog)

local computed = setmetatable({}, {__index = function(t, k) return k .. "!" end})
print(computed.abc, computed[1])

local proxy = setmetatable({}, {__newindex = function(t, k, v) rawset(t, k, v * 2) end})
proxy.x = 5
proxy.x = 7
print(proxy.x)

local store = {}
local redirect = setmetatable({}, {__newindex = store})
redirect.y = 3
print(rawget(redirect, "y"), store.y)

local defaults = setmetatable({}, {__index = setmetatable({a = 1}, {__index = {b = 2}})})
print(defaults.a, defaults.b, defaults.c)

print(getmetatable({}), getmetatable(1))
local locked = setmetatable({}, {__metatable = "locked"})
print(getmetatable(locked), pcall(setmetatable, locked, {}))
print(setmetatable(obj, nil) == obj, getmetatable(obj), obj.kind)

local seq = setmetatable({}, {__index = function(t, i) if i <= 3 then return i * 10 end end})
for i, v in ipairs(seq) do print(i, v) end
