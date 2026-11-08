local ab = "a" .. "b"
print(ab, #ab, ab == "ab", ab ~= "ab")
print("abc" < "abd", "ab" < "abc", "b" > "abc", "" < "a", "a" <= "a")
print(1 .. "", 1.5 .. "|", -0.0 .. "", 1e100 .. "")
print(#"", #"hello", #("x" .. 12))

local s = ""
for i = 1, 5 do s = s .. i end
print(s, s == "12345", #s)

local function key(n) return "k" .. n end
print(key(1) == key(1), key(1) == key(2))
print(tostring(12) == "12", tostring(nil) .. tostring(true))
print("tab\tand\\backslash", 'single "quoted"')
