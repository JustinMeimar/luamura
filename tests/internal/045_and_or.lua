print(1 and 2, nil and 2, false and 2)
print(1 or 2, nil or 2, false or nil)

local calls = 0
local function touch(v) calls = calls + 1 return v end
print(touch(false) and touch(1), calls)
print(touch(1) or touch(2), calls)
print(touch(nil) or touch(false), calls)

local function pick(c, a, b) return c and a or b end
print(pick(true, "yes", "no"), pick(false, "yes", "no"))

local x = 5
if x > 0 and x < 10 or x == 100 then print("in range") end
if not (x > 0 and x > 10) then print("not both") end

print(nil and undefined_global())
print(1 or undefined_global())
print((x > 3 and "big" or "small") .. "!")
