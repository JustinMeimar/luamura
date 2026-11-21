local function sign(n)
    if n < 0 then
        return "negative"
    elseif n == 0 then
        return "zero"
    else
        return "positive"
    end
end
print(sign(-3), sign(0), sign(7))

local function truthy(v)
    if v then return "yes" end
    return "no"
end
print(truthy(nil), truthy(false), truthy(0), truthy(""))

local x = 1
if x == 1 then
    local x = 2
    print(x)
end
print(x)

if false then print("never") elseif nil then print("never") end
print("done")
