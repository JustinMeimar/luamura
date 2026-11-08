local callable = setmetatable({count = 0}, {__call = function(self, a, b)
    self.count = self.count + 1
    return a + b, self.count
end})
print(callable(1, 2))
print(callable(3, 4))
local function forward(...) return callable(...) end
print(forward(10, 20))
print(pcall(callable, 5, 6))

local chained = setmetatable({}, {__call = callable})
print((pcall(chained, 1)))

local Class = setmetatable({}, {__call = function(cls, name) return setmetatable({name = name}, cls) end})
Class.__index = Class
function Class:hello() return "hi " .. self.name end
local instance = Class("x")
print(instance:hello())

local function three() return 1, 2, 3 end
local spread = setmetatable({}, {__call = function(self, ...) return select("#", ...) end})
print(spread(three()))
print((pcall(function() local t = {} return t() end)))
print((pcall(function() return (nil)() end)))
