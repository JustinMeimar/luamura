print(math.type(1), math.type(1.5), math.tointeger(3.0), math.tointeger(3.5))
print(math.maxinteger, math.mininteger, math.ult(-1, 0))
print(math.floor(-1.25), math.ceil(1.25), math.abs(-42), math.sqrt(144))
print(math.min(3, 1, 2), math.max(3, 1, 2), math.fmod(-7, 3))
math.randomseed(123, 456)
print(math.random(1, 100), math.random(1, 100), math.random(1, 100))

print(string.find("hello 123", "%d+"))
print(("key=value"):match("(%a+)=(%a+)"))
print(string.match("a (nested (value)) z", "%b()"))
print(string.match("abc def", "%f[%a]def"))
print(string.gsub("a1 b22", "(%a)(%d+)", function(letter, digits)
    return letter:upper() .. tostring(tonumber(digits) + 1)
end))
print(string.gsub("a b c", "%a", function(s)
    if s == "b" then error(42) end
    return s
end))
