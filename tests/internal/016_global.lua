#!/usr/bin/env lua
-- global declarations and variable attributes
global <const> *
global x, y <const> = 1, 2
print(x, y, print ~= nil)
x = x + 10
print(x)

global function count(n)
  if n == 0 then return "done" end
  return count(n - 1)
end
print(count(5))

print((pcall(function () global function count() end end)))
print((pcall(function () global x = 3 end)))
global fresh1, fresh2 = 7
print(fresh1, fresh2)

local a <const>, b = 10, 20
local <const> c, d = 30, nil
print(a + b + c)

do
  global none
  global print, tostring
  print(tostring(none))
end

local global = "not a declaration"
print(global)
global = 1
print(global)
