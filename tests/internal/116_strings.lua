local packed = string.pack("<i4I2c3", -12345, 65535, "a\0b")
print(#packed, string.packsize("<i4I2c3"))
print(string.unpack("<i4I2c3", packed))
print(string.gsub("hello hello", "hello", "world"))
print(string.gsub("a1 b22", "(%a)(%d+)", function(letter, digits)
    return letter:upper() .. tostring(tonumber(digits) + 1)
end))
print((pcall(function()
    return string.gsub("a b c", "%a", function(s)
        if s == "b" then error(42) end
        return s
    end)
end)))
print(string.gsub("ok", "%a", function(s) return s:upper() end))
local saved = string.match
string.match = function() return "replacement" end
print(string.match("123", "%d+"), saved("123", "%d+"))
print(utf8.len("A\195\169\240\159\152\128"))
print(utf8.codepoint("A\195\169\240\159\152\128", 1, -1))
print(utf8.char(65, 233, 128512))
print(utf8.offset("A\195\169\240\159\152\128", 3))
