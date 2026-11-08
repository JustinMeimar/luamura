local x = 5
local y = assert(x == 5, "unused")
print(y)
print(type(x), type("s"), type(nil), type(true), type(print))
print(tostring(7), tostring(7 / 2), tostring(4 / 2), tostring(nil))
print(not nil, not 0, not false)
