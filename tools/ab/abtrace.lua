-- abtrace.lua -- every bus read MAME makes between frames $AB_TRACE_FROM and
-- $AB_TRACE_TO, one "cycle addr data" line each (hex), to $AB_TRACE; the
-- same key schedule as abdump.lua ($AB_KEYS). The other side is
-- studio2_headless -trace. For stock MAME runs (no FujiNet device: its read
-- handler, installed at reset, would replace a tap set before it).
local out = assert(io.open(os.getenv("AB_TRACE"), "w"))
local from = tonumber(os.getenv("AB_TRACE_FROM") or "0")
local to = tonumber(os.getenv("AB_TRACE_TO") or "10")
local evs = {}
for f, p, k, d in string.gmatch(os.getenv("AB_KEYS") or "", "(%d+):(%a):(%d+):(%d)") do
  evs[#evs + 1] = { f = tonumber(f), p = p, k = tonumber(k), d = tonumber(d) }
end
local space = manager.machine.devices[":ic1"].spaces["program"]
local tracing = false

local function tap(offset, data, mask)
  if tracing then
    local c = math.floor(manager.machine.time:as_double() * 1760000 + 0.5)
    out:write(string.format("%d %04X %02X\n", c, offset, data))
  end
end

local n = 0
_G.__abtrace = emu.add_machine_frame_notifier(function()
  if not _G.__abtap then
    _G.__abtap = space:install_read_tap(0x0000, 0xFFFF, "abtrace", tap)
  end
  for _, e in ipairs(evs) do
    if e.f == n then
      local field
      if e.p == "C" then
        field = manager.machine.ioport.ports[":CLEAR"].fields["Clear"]
      else
        field = manager.machine.ioport.ports[":" .. e.p].fields[e.p .. " " .. e.k]
      end
      if e.d == 1 then field:set_value(1) else field:clear_value() end
    end
  end
  tracing = n >= from and n < to
  n = n + 1
  if n >= to then
    out:close()
    manager.machine:exit()
  end
end)
