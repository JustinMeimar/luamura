print(pcall(function() return 1, 2, 3 end))
print(pcall(function(a, b) return a + b end, 3, 4))
print(pcall(error, "message"))
print(pcall(error, "level zero", 0))
print(pcall(error))
local t = {}
local ok, err = pcall(error, t)
print(ok, err == t)

local function fails() local x = nil; return x.field end
print((pcall(fails)))
print(select("#", pcall(fails)))
print(pcall(pcall, error, "inner"))
print(pcall(pcall))

local function handler(m) return "handled: " .. tostring(m) end
print(xpcall(error, handler, "boom"))
print(xpcall(function(a) return a * 2 end, handler, 21))
print(xpcall(error, function() error("again") end, "x"))
print(xpcall(error, function() end, "x"))

local count = 0
local function protected_loop()
    for i = 1, 3 do
        local ok = pcall(function()
            count = count + i
            if i == 2 then error("skip", 0) end
        end)
        print(i, ok)
    end
end
protected_loop()
print(count)

local ok2, value = pcall(function()
    local inner_ok = pcall(error, "deep")
    return inner_ok, "after"
end)
print(ok2, value)
error({code = 1})
