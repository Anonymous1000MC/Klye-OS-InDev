-- hello.lua - the first Lua program on Klye OS
local total = 0

for i = 1, 10 do
  total = total + i
end

print("sum 1..10 =", total)
print("strings work:", ("lua %s" ):format("is in"))
print("math:", math.floor(math.sqrt(2) * 1000) / 1000)
print("tables:", #({10, 20, 30}))

local t = {name = "klye", lang = "lua"}
print("field:", t.name, t.lang)

local function fib(n)
  if n < 2 then return n end
  return fib(n - 1) + fib(n - 2)
end
print("fib(15) =", fib(15))
