local P = {}
P.__tostring = function(p) return "P(" .. p.v .. ")" end
local p = setmetatable({v = 1}, P)
print(p, tostring(p), "x" .. tostring(p))

print(pcall(tostring, setmetatable({}, {__tostring = function() return 1 end})))

local proxy = setmetatable({}, {__pairs = function(t)
    local i = 0
    return function() i = i + 1 if i <= 3 then return i, i * i end end, t, nil
end})
for k, v in pairs(proxy) do print(k, v) end
local plain = setmetatable({a = 1}, {})
for k, v in pairs(plain) do print(k, v) end
print(pcall(tostring, setmetatable({}, {__tostring = function() return {} end})))
