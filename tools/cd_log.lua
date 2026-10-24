-- PCSX-Redux script: logs the game's calls into libcd on the PlayStation, in the format of the port's CRASH2_CD_LOG (a
-- line a call: the game frame, the CPU's cycles at the call and at its return, the function, its arguments and what it
-- returned).
--
-- Runs decomp's tools/run_game.lua first (RUN_GAME_LUA names it), for its game clock, pace and presses, with its own
-- variables: a schedule run_game.py wrote (RUN_GAME_SCHEDULE) and a folder (RUN_GAME_OUT). CD_LOG names the file
-- written. In decomp's PCSX-Redux (tools/setup_toolchain.py), as run_game.py starts it:
--   RUN_GAME_LUA=DECOMP/tools/run_game.lua RUN_GAME_SCHEDULE=FILE RUN_GAME_OUT=DIR CD_LOG=FILE \
--   pcsx-redux -cli -testmode -run -interpreter -debugger -portable DIR -iso DISC.cue -dofile tools/cd_log.lua
-- Only calls from outside libcd are logged (libcd's own calls into itself are its business), each with the value it
-- returned, read where it returns to. A call repeated with the same arguments and result in the same game frame (a
-- poll's loop) is one line, the first's call and the last's return, its count after an x.

local HEXADECIMAL = 16
local BREAKPOINT_WIDTH = 4
local RAM_MASK = 0x1fffff
-- libcd in the retail executable (decomp's config/symbol_addrs.txt): from CdInit to the end of CdReadMode
local LIBCD_START = 0x80046884
local LIBCD_END = 0x80049170
local FUNCTIONS = {
    { name = 'CdControl', address = 0x80046b50 },
    { name = 'CdControlF', address = 0x80046c88 },
    { name = 'CdSync', address = 0x80046ae0 },
    { name = 'CdDataSync', address = 0x80046f7c },
    { name = 'CdRead', address = 0x80048f74 },
    { name = 'CdReadSync', address = 0x80049078 },
}
-- CdControl's commands whose parameter is a position: CdlSetloc, CdlSeekL
local POSITION_COMMANDS = { [0x02] = true, [0x15] = true }
-- A position's minute, second and sector in BCD, counted from 2 seconds before the first sector
local BCD_SHIFT = 4
local BCD_BASE = 10
local BCD_LOW_MASK = 0xf
local SECTORS_PER_SECOND = 75
local SECONDS_PER_MINUTE = 60
local PREGAP_SECTORS = 150

local logPath = os.getenv('CD_LOG')
local runGame = os.getenv('RUN_GAME_LUA')
local schedulePath = os.getenv('RUN_GAME_SCHEDULE')
if logPath == nil or runGame == nil or schedulePath == nil then
    error('CD_LOG, RUN_GAME_LUA and RUN_GAME_SCHEDULE must be set')
else
    -- All given
end

dofile(runGame)

-- Gfx_EndFrame's address, from the schedule's clock line
local endFrame = nil
for line in io.lines(schedulePath) do
    local words = {}
    for word in line:gmatch('%S+') do words[#words + 1] = word end
    if words[1] == 'clock' and words[2] == 'game' then
        endFrame = tonumber(words[3], HEXADECIMAL)
    else
        -- Another command
    end
end
if endFrame == nil then
    error('the schedule has no game clock')
else
    -- On the game clock
end

local log = io.open(logPath, 'w')
local ram = PCSX.getMemPtr()
local gameFrame = 0
-- The calls not yet returned, by return address
local pending = {}
-- The return addresses' breakpoints, by address; the others in CdLogBreakpoints: a breakpoint nothing holds is deleted
-- when Lua collects it
local returns = {}
CdLogBreakpoints = {}

local function fromBcd(value)
    return bit.rshift(value, BCD_SHIFT) * BCD_BASE + bit.band(value, BCD_LOW_MASK)
end

local function position(address)
    local bytes = ffi.cast('uint8_t *', ram + bit.band(address, RAM_MASK))
    return (fromBcd(bytes[0]) * SECONDS_PER_MINUTE + fromBcd(bytes[1])) * SECTORS_PER_SECOND + fromBcd(bytes[2])
        - PREGAP_SECTORS
end

local function arguments(name, registers)
    local a0 = tonumber(registers.GPR.n.a0)
    local a1 = tonumber(registers.GPR.n.a1)
    if (name == 'CdControl' or name == 'CdControlF') and POSITION_COMMANDS[a0] and a1 ~= 0 then
        return string.format('%x %d', a0, position(a1))
    elseif name == 'CdControl' or name == 'CdControlF' then
        return string.format('%x', a0)
    elseif name == 'CdRead' then
        return string.format('%d %x', a0, tonumber(registers.GPR.n.a2))
    else
        return string.format('%d', a0)
    end
end

-- The call held back while it repeats: what identifies a repeat, its line and result, and its count
local heldKey = nil
local held = nil
local heldCount = 0

local function flush()
    if held == nil then
        return
    elseif heldCount > 1 then
        log:write(string.format('%s -> %d x%d\n', held.line, held.v0, heldCount))
    else
        log:write(string.format('%s -> %d\n', held.line, held.v0))
    end
    held = nil
    heldKey = nil
    log:flush()
end

local function onReturn(address)
    return function()
        local call = pending[address]
        if call ~= nil then
            local v0 = bit.tobit(tonumber(PCSX.getRegisters().GPR.n.v0))
            local key = string.format('%s -> %d', call.what, v0)
            if key ~= heldKey then
                flush()
                held = { start = string.format('%d %d', gameFrame, call.cycle), what = call.what, v0 = v0 }
                heldKey = key
                heldCount = 0
            else
                -- The same call again
            end
            heldCount = heldCount + 1
            held.line = string.format('%s %d %s', held.start, tonumber(PCSX.getCPUCycles()), held.what)
            pending[address] = nil
        else
            -- Passed without a call of ours
        end
        return true
    end
end

local function keep(breakpoint)
    CdLogBreakpoints[#CdLogBreakpoints + 1] = breakpoint
end

keep(PCSX.addBreakpoint(endFrame, 'Exec', BREAKPOINT_WIDTH, 'cd_log frame', function()
    flush()
    gameFrame = gameFrame + 1
    return true
end))

for _, f in ipairs(FUNCTIONS) do
    keep(PCSX.addBreakpoint(f.address, 'Exec', BREAKPOINT_WIDTH, 'cd_log ' .. f.name, function()
        local registers = PCSX.getRegisters()
        local ra = tonumber(registers.GPR.n.ra)
        if ra >= LIBCD_START and ra < LIBCD_END then
            return true
        else
            -- The game's call
        end
        pending[ra] = { cycle = tonumber(PCSX.getCPUCycles()), what = f.name .. ' ' .. arguments(f.name, registers) }
        if returns[ra] == nil then
            returns[ra] = PCSX.addBreakpoint(ra, 'Exec', BREAKPOINT_WIDTH, 'cd_log return', onReturn(ra))
        else
            -- Already watched
        end
        return true
    end))
end
