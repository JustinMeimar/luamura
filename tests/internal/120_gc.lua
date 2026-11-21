-- objects reachable only from compiled locals, temporaries and upvalues
-- survive collection; unreachable ones are collected
local t = {1, 2, 3}
local s = string.rep("x", 100)
collectgarbage()
print(#t, t[3], #s)

local function counter()
  local n = 0
  local cells = {}
  return function()
    n = n + 1
    cells[n] = {n}
    collectgarbage("step")
    return n, #cells, cells[n][1]
  end
end
local c = counter()
for _ = 1, 50 do c() end
collectgarbage()
print(c())

-- a returned function's frame no longer roots its objects
local weak = setmetatable({}, {__mode = "v"})
local function fill()
  local kept = {}
  weak[1] = kept
  weak[2] = {}
  return kept
end
local kept = fill()
collectgarbage()
print(weak[1] == kept, weak[2])

local function build(n)
  if n == 0 then return {} end
  local inner = build(n - 1)
  local outer = {inner, tostring(n) .. "!"}
  collectgarbage("step")
  return outer
end
local chain = build(200)
local depth = 0
while chain[1] do depth = depth + 1; chain = chain[1] end
print(depth)

local parts = {}
for i = 1, 300 do
  parts[#parts + 1] = ("%d:%s"):format(i, string.rep("a", i % 7))
  if i % 50 == 0 then collectgarbage() end
end
print(#parts, parts[300], table.concat(parts, ","):len())

local gen = collectgarbage("generational")
local keep = {}
for i = 1, 2000 do keep[i % 100 + 1] = {i, {i}} end
collectgarbage()
print(keep[1][1], keep[100][2][1])
collectgarbage(gen)

local finalized = 0
local function garbage()
  for _ = 1, 10 do setmetatable({}, {__gc = function() finalized = finalized + 1 end}) end
end
garbage()
collectgarbage()
collectgarbage()
print(finalized)

print(select("#", table.unpack({1, 2, nil, 4}, 1, 4)), select(2, pcall(error, {code = 7})).code)
local ok, e = pcall(error, setmetatable({}, {__tostring = function() return "custom" end}))
print(ok, tostring(e))
