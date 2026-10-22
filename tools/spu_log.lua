-- PCSX-Redux script: logs the PlayStation's SPU register writes, in the format of the port's CRASH2_SPU_LOG (a line
-- a 16-bit write: the vsync, the address and the value, in hexadecimal), for tools/compare_spu.py.
--
-- SPU_LOG names the file written, SPU_LOG_VSYNCS the vsync to quit at. Run in decomp's PCSX-Redux
-- (tools/setup_toolchain.py), whose interpreter and debugger the write breakpoint needs:
--   pcsx-redux -cli -testmode -run -interpreter -debugger -portable DIR -iso DISC.cue -dofile tools/spu_log.lua
-- The breakpoint gives the address written, not the value: the store at the program counter (or the one before it,
-- once the counter has moved on) is decoded, its value read from its register.

local SPU_REGISTERS = 0x1f801c00
local SPU_REGISTERS_BYTES = 0x200
local HALF_BITS = 16
local HALF_BYTES = 2
local HALF_MASK = 0xffff
local WORD_MASK = 0xffffffff
local INSTRUCTION_BYTES = 4
-- MIPS stores: their opcodes, and their fields
local OPCODE_SHIFT = 26
local OPCODE_SB = 0x28
local OPCODE_SH = 0x29
local OPCODE_SW = 0x2b
local REGISTER_MASK = 31
local BASE_SHIFT = 21
local SOURCE_SHIFT = 16
local IMMEDIATE_SIGN = 0x8000
local IMMEDIATE_RANGE = 0x10000
-- Where the code runs: main RAM (any segment) or the BIOS's ROM
local RAM_MASK = 0x1fffff
local ROM_START = 0xbfc00000
local ROM_MASK = 0x7ffff
-- The SPU's Speed setting: 0 is unbounded; the pads read nothing from the host
local UNBOUNDED_SPEED = 0
local HOST_INPUT_NONE = 'Keyboard'
local PAD_PORTS = 2

local logPath = os.getenv('SPU_LOG')
local lastVsync = tonumber(os.getenv('SPU_LOG_VSYNCS'))
if logPath == nil or lastVsync == nil then
    error('SPU_LOG and SPU_LOG_VSYNCS must be set')
else
    -- Both given
end

PCSX.settings.spu.Speed = UNBOUNDED_SPEED
for port = 1, PAD_PORTS do
    PCSX.settings.pads[port].PadType = HOST_INPUT_NONE
end

local log = io.open(logPath, 'w')
local ram = PCSX.getMemPtr()
local rom = PCSX.getRomPtr()
local vsync = 0

local function instruction(pc)
    if pc >= ROM_START then
        return ffi.cast('uint32_t *', rom + bit.band(pc, ROM_MASK))[0]
    else
        return ffi.cast('uint32_t *', ram + bit.band(pc, RAM_MASK))[0]
    end
end

-- The store at pc: its address, value and opcode, or nil for another instruction
local function store(pc, registers)
    local word = instruction(pc)
    local opcode = bit.rshift(word, OPCODE_SHIFT)
    if opcode == OPCODE_SB or opcode == OPCODE_SH or opcode == OPCODE_SW then
        local base = bit.band(bit.rshift(word, BASE_SHIFT), REGISTER_MASK)
        local source = bit.band(bit.rshift(word, SOURCE_SHIFT), REGISTER_MASK)
        local offset = bit.band(word, HALF_MASK)
        if offset >= IMMEDIATE_SIGN then
            offset = offset - IMMEDIATE_RANGE
        else
            -- A positive offset
        end
        local address = bit.band(registers.GPR.r[base] + offset, WORD_MASK)
        return address, registers.GPR.r[source], opcode
    else
        return nil
    end
end

local function write(address, value)
    log:write(string.format('%d %08x %04x\n', vsync, address, bit.band(value, HALF_MASK)))
end

Crash2SpuLogWrite = PCSX.addBreakpoint(SPU_REGISTERS, 'Write', SPU_REGISTERS_BYTES, 'spu log', function()
    local registers = PCSX.getRegisters()
    local address, value, opcode = store(registers.pc, registers)
    if address == nil then
        address, value, opcode = store(registers.pc - INSTRUCTION_BYTES, registers)
    else
        -- The store is at the program counter
    end
    if address == nil then
        error(string.format('no store at %08x for an SPU write', registers.pc))
    elseif opcode == OPCODE_SW then
        write(address, value)
        write(address + HALF_BYTES, bit.rshift(value, HALF_BITS))
    else
        write(address, value)
    end
    return true
end)

Crash2SpuLogVsync = PCSX.Events.createEventListener('GPU::Vsync', function()
    vsync = vsync + 1
    if vsync >= lastVsync then
        log:close()
        PCSX.quit(0)
    else
        -- Still logging
    end
end)
