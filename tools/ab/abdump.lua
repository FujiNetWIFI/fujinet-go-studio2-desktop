-- abdump.lua -- the MAME side of tools/ab/mame_ab.py. Every frame: apply the
-- key schedule for that frame, then append the screen's pixels (the visible
-- area, 4 bytes a pixel) to $AB_OUT. Exits after $AB_FRAMES frames.
--
-- $AB_KEYS: comma-separated F:P:K:D -- at frame F, keypad P (A or B) key K
-- (0-9) down (D=1) or up (D=0); P = C is the Clear key (K ignored).
--
-- Reads nothing through the address space (a read there has side effects
-- on the FujiNet device), so it is the same script whether the device is
-- active or not.
local out = assert(io.open(os.getenv("AB_OUT"), "wb"))
local frames = tonumber(os.getenv("AB_FRAMES") or "300")
local evs = {}
for f, p, k, d in string.gmatch(os.getenv("AB_KEYS") or "", "(%d+):(%a):(%d+):(%d)") do
  evs[#evs + 1] = { f = tonumber(f), p = p, k = tonumber(k), d = tonumber(d) }
end

local n = 0
_G.__abdump = emu.add_machine_frame_notifier(function()
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
  local px = manager.machine.screens[":screen"]:pixels()
  out:write(px)
  n = n + 1
  if n >= frames then
    out:close()
    manager.machine:exit()
  end
end)
