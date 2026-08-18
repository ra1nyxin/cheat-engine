local integer_min = 0x8000000000000000
local integer_max = 0x7fffffffffffffff

-- These expressions previously invoked signed-overflow undefined behavior.
assert(integer_min % -1 == 0)
assert(integer_min % integer_max == integer_max - 1)
assert(-5 % 3 == 1)
assert(5 % -3 == -1)
assert(integer_max + 1 == 9223372036854775808.0)
assert(integer_min - 1 == -9223372036854775809.0)
assert(integer_min / -1 == 9223372036854775808.0)
assert(integer_min ^ 1 == integer_min)

-- A chained __newindex table may be rehashed while its metamethod is in use.
local calls = 0
local grandparent = {}
grandparent.__newindex = function(subject, key, value)
  calls = calls + 1
  assert(subject ~= nil and key == "foo" and value == 10)
end
local parent = {}
parent.__newindex = parent
setmetatable(parent, grandparent)
local child = setmetatable({}, parent)
child.foo = 10
assert(calls == 1)

local self_metatable = {}
setmetatable(self_metatable, self_metatable)
self_metatable.__newindex = function(_, key, value)
  calls = calls + 1
  assert(key == "bar" and value == 20)
end
local self_metatable_child = setmetatable({}, self_metatable)
self_metatable_child.bar = 20
assert(calls == 2)

local function old_style_vararg(...)
  -- Deliberately use the Lua 5.0-compatible arg table, not the ... operator.
  local a, b, c, d, e, f, g, h = 1, 2, 3, 4, 5, 6, 7, 8
  assert(a + b + c + d + e + f + g + h == 36)
  assert(arg.n == 3 and arg[1] == "a" and arg[3] == "c")
end
old_style_vararg("a", "b", "c")

-- Old-style varargs need enough stack space before creating the arg table.
local function lunpack(depth, ...)
  if depth == 0 then
    return ...
  end
  return lunpack(depth - 1, 1, ...)
end
local function check_varargs(...)
  assert(select("#", ...) == 129)
  assert(select(129, ...) == "tail")
end
check_varargs(lunpack(128, "tail"))

print("Lua security regression tests passed")
