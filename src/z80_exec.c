#include "z80_cpu.h"

#include <stdio.h>
#include <string.h>

#define FLAG_S 0x80
#define FLAG_Z 0x40
#define FLAG_5 0x20
#define FLAG_H 0x10
#define FLAG_3 0x08
#define FLAG_PV 0x04
#define FLAG_N 0x02
#define FLAG_C 0x01

typedef enum IndexMode {
    INDEX_NONE = 0,
    INDEX_IX = 1,
    INDEX_IY = 2,
} IndexMode;

#define STEP_OK(cycles)                                                                                               \
    do {                                                                                                              \
        AddCycles(stats, (cycles));                                                                                   \
        result = Z80_STEP_OK;                                                                                         \
        goto done;                                                                                                    \
    } while (0)

#define STEP_HALT(cycles)                                                                                             \
    do {                                                                                                              \
        state->halted = 1;                                                                                            \
        AddCycles(stats, (cycles));                                                                                   \
        result = Z80_STEP_HALT;                                                                                       \
        goto done;                                                                                                    \
    } while (0)

static UINT16 GetAF(const StateZ80 *state) {
    return (UINT16)((state->a << 8) | state->f);
}

static UINT16 GetBC(const StateZ80 *state) {
    return (UINT16)((state->b << 8) | state->c);
}

static UINT16 GetDE(const StateZ80 *state) {
    return (UINT16)((state->d << 8) | state->e);
}

static UINT16 GetHL(const StateZ80 *state) {
    return (UINT16)((state->h << 8) | state->l);
}

static UINT16 GetAFAlt(const StateZ80 *state) {
    return (UINT16)((state->a_alt << 8) | state->f_alt);
}

static UINT16 GetBCAlt(const StateZ80 *state) {
    return (UINT16)((state->b_alt << 8) | state->c_alt);
}

static UINT16 GetDEAlt(const StateZ80 *state) {
    return (UINT16)((state->d_alt << 8) | state->e_alt);
}

static UINT16 GetHLAlt(const StateZ80 *state) {
    return (UINT16)((state->h_alt << 8) | state->l_alt);
}

static void SetAF(StateZ80 *state, UINT16 value) {
    state->a = (UINT8)(value >> 8);
    state->f = (UINT8)(value & 0xFF);
}

static void SetBC(StateZ80 *state, UINT16 value) {
    state->b = (UINT8)(value >> 8);
    state->c = (UINT8)(value & 0xFF);
}

static void SetDE(StateZ80 *state, UINT16 value) {
    state->d = (UINT8)(value >> 8);
    state->e = (UINT8)(value & 0xFF);
}

static void SetHL(StateZ80 *state, UINT16 value) {
    state->h = (UINT8)(value >> 8);
    state->l = (UINT8)(value & 0xFF);
}

static void SetAFAlt(StateZ80 *state, UINT16 value) {
    state->a_alt = (UINT8)(value >> 8);
    state->f_alt = (UINT8)(value & 0xFF);
}

static void SetBCAlt(StateZ80 *state, UINT16 value) {
    state->b_alt = (UINT8)(value >> 8);
    state->c_alt = (UINT8)(value & 0xFF);
}

static void SetDEAlt(StateZ80 *state, UINT16 value) {
    state->d_alt = (UINT8)(value >> 8);
    state->e_alt = (UINT8)(value & 0xFF);
}

static void SetHLAlt(StateZ80 *state, UINT16 value) {
    state->h_alt = (UINT8)(value >> 8);
    state->l_alt = (UINT8)(value & 0xFF);
}

static UINT8 HighByte(UINT16 value) {
    return (UINT8)(value >> 8);
}

static UINT8 LowByte(UINT16 value) {
    return (UINT8)(value & 0xFF);
}

static UINT16 Compose16(UINT8 hi, UINT8 lo) {
    return (UINT16)((hi << 8) | lo);
}

static int Parity(UINT8 value) {
    int ones = 0;
    for (int i = 0; i < 8; ++i) {
        ones += (value >> i) & 1;
    }
    return (ones & 1) == 0;
}

static UINT8 FlagsSZ53(UINT8 value) {
    UINT8 flags = (UINT8)(value & (FLAG_S | FLAG_5 | FLAG_3));
    if (value == 0) {
        flags |= FLAG_Z;
    }
    return flags;
}

static UINT8 FlagsSZP53(UINT8 value) {
    UINT8 flags = FlagsSZ53(value);
    if (Parity(value)) {
        flags |= FLAG_PV;
    }
    return flags;
}

static void TraceEvent(Z80StepEvents *events, Z80BusEventKind kind, UINT16 addr, UINT8 data) {
    if (!events || events->count >= Z80_MAX_STEP_EVENTS) {
        return;
    }
    events->events[events->count].kind = kind;
    events->events[events->count].addr = addr;
    events->events[events->count].data = data;
    events->count += 1;
}

static void AddCycles(ExecutionStatsZ80 *stats, UINT64 cycles) {
    if (stats) {
        stats->total_tstates += cycles;
    }
}

static void UpdateMinSp(StateZ80 *state, ExecutionStatsZ80 *stats) {
    if (stats && (!stats->min_sp_set || state->sp < stats->min_sp)) {
        stats->min_sp = state->sp;
        stats->min_sp_set = true;
    }
}

static void IncrementR(StateZ80 *state) {
    state->r = (UINT8)((state->r & 0x80) | ((state->r + 1) & 0x7F));
}

static UINT8 FetchOpcodeByte(StateZ80 *state, Z80StepEvents *events) {
    UINT16 addr = state->pc;
    UINT8 value = state->memory[addr];
    TraceEvent(events, Z80_BUS_EVENT_FETCH, addr, value);
    IncrementR(state);
    state->pc = (UINT16)(state->pc + 1);
    return value;
}

static UINT8 ReadMemory(StateZ80 *state, Z80StepEvents *events, UINT16 addr) {
    UINT8 value = state->memory[addr];
    TraceEvent(events, Z80_BUS_EVENT_READ, addr, value);
    return value;
}

static void WriteMemory(StateZ80 *state, Z80StepEvents *events, UINT16 addr, UINT8 value) {
    state->memory[addr] = value;
    TraceEvent(events, Z80_BUS_EVENT_WRITE, addr, value);
}

static UINT8 ReadImmediate8(StateZ80 *state, Z80StepEvents *events) {
    UINT8 value = ReadMemory(state, events, state->pc);
    state->pc = (UINT16)(state->pc + 1);
    return value;
}

static UINT16 ReadImmediate16(StateZ80 *state, Z80StepEvents *events) {
    UINT8 lo = ReadImmediate8(state, events);
    UINT8 hi = ReadImmediate8(state, events);
    return Compose16(hi, lo);
}

static UINT16 ReadMemory16(StateZ80 *state, Z80StepEvents *events, UINT16 addr) {
    UINT8 lo = ReadMemory(state, events, addr);
    UINT8 hi = ReadMemory(state, events, (UINT16)(addr + 1));
    return Compose16(hi, lo);
}

static void WriteMemory16(StateZ80 *state, Z80StepEvents *events, UINT16 addr, UINT16 value) {
    WriteMemory(state, events, addr, LowByte(value));
    WriteMemory(state, events, (UINT16)(addr + 1), HighByte(value));
}

static UINT8 ReadPort(StateZ80 *state, Z80StepEvents *events, UINT16 addr) {
    UINT8 value = state->io_space ? state->io_space[addr] : 0xFF;
    if (state->io_read) {
        value = state->io_read(state->io_user, addr, value);
        if (state->io_space) {
            state->io_space[addr] = value;
        }
    }
    TraceEvent(events, Z80_BUS_EVENT_IO_READ, addr, value);
    return value;
}

static void WritePort(StateZ80 *state, Z80StepEvents *events, UINT16 addr, UINT8 value) {
    if (state->io_write) {
        state->io_write(state->io_user, addr, value);
    }
    if (state->io_space) {
        state->io_space[addr] = value;
    }
    TraceEvent(events, Z80_BUS_EVENT_IO_WRITE, addr, value);
}

static UINT16 GetIndexValue(const StateZ80 *state, IndexMode index) {
    if (index == INDEX_IX) {
        return state->ix;
    }
    if (index == INDEX_IY) {
        return state->iy;
    }
    return GetHL(state);
}

static void SetIndexValue(StateZ80 *state, IndexMode index, UINT16 value) {
    if (index == INDEX_IX) {
        state->ix = value;
    } else if (index == INDEX_IY) {
        state->iy = value;
    } else {
        SetHL(state, value);
    }
}

static UINT16 EffectiveIndexedAddress(const StateZ80 *state, IndexMode index, int8_t displacement) {
    return (UINT16)(GetIndexValue(state, index) + displacement);
}

static UINT8 GetReg8Simple(const StateZ80 *state, int reg, IndexMode index) {
    switch (reg & 7) {
    case 0:
        return state->b;
    case 1:
        return state->c;
    case 2:
        return state->d;
    case 3:
        return state->e;
    case 4:
        if (index == INDEX_IX) {
            return HighByte(state->ix);
        }
        if (index == INDEX_IY) {
            return HighByte(state->iy);
        }
        return state->h;
    case 5:
        if (index == INDEX_IX) {
            return LowByte(state->ix);
        }
        if (index == INDEX_IY) {
            return LowByte(state->iy);
        }
        return state->l;
    default:
        return state->a;
    }
}

static void SetReg8Simple(StateZ80 *state, int reg, IndexMode index, UINT8 value) {
    switch (reg & 7) {
    case 0:
        state->b = value;
        break;
    case 1:
        state->c = value;
        break;
    case 2:
        state->d = value;
        break;
    case 3:
        state->e = value;
        break;
    case 4:
        if (index == INDEX_IX) {
            state->ix = Compose16(value, LowByte(state->ix));
        } else if (index == INDEX_IY) {
            state->iy = Compose16(value, LowByte(state->iy));
        } else {
            state->h = value;
        }
        break;
    case 5:
        if (index == INDEX_IX) {
            state->ix = Compose16(HighByte(state->ix), value);
        } else if (index == INDEX_IY) {
            state->iy = Compose16(HighByte(state->iy), value);
        } else {
            state->l = value;
        }
        break;
    default:
        state->a = value;
        break;
    }
}

static UINT8 ReadReg8(StateZ80 *state, Z80StepEvents *events, int reg, IndexMode index, int indexed_mem, int8_t disp) {
    if ((reg & 7) == 6) {
        UINT16 addr = indexed_mem ? EffectiveIndexedAddress(state, index, disp) : GetHL(state);
        return ReadMemory(state, events, addr);
    }
    return GetReg8Simple(state, reg, index);
}

static void WriteReg8(StateZ80 *state, Z80StepEvents *events, int reg, IndexMode index, int indexed_mem, int8_t disp,
                      UINT8 value) {
    if ((reg & 7) == 6) {
        UINT16 addr = indexed_mem ? EffectiveIndexedAddress(state, index, disp) : GetHL(state);
        WriteMemory(state, events, addr, value);
        return;
    }
    SetReg8Simple(state, reg, index, value);
}

static UINT16 GetRP(StateZ80 *state, int rp, IndexMode index) {
    switch (rp & 3) {
    case 0:
        return GetBC(state);
    case 1:
        return GetDE(state);
    case 2:
        return GetIndexValue(state, index);
    default:
        return state->sp;
    }
}

static void SetRP(StateZ80 *state, int rp, IndexMode index, UINT16 value) {
    switch (rp & 3) {
    case 0:
        SetBC(state, value);
        break;
    case 1:
        SetDE(state, value);
        break;
    case 2:
        SetIndexValue(state, index, value);
        break;
    default:
        state->sp = value;
        break;
    }
}

static UINT16 GetRP2(StateZ80 *state, int rp, IndexMode index) {
    switch (rp & 3) {
    case 0:
        return GetBC(state);
    case 1:
        return GetDE(state);
    case 2:
        return GetIndexValue(state, index);
    default:
        return GetAF(state);
    }
}

static void SetRP2(StateZ80 *state, int rp, IndexMode index, UINT16 value) {
    switch (rp & 3) {
    case 0:
        SetBC(state, value);
        break;
    case 1:
        SetDE(state, value);
        break;
    case 2:
        SetIndexValue(state, index, value);
        break;
    default:
        SetAF(state, value);
        break;
    }
}

static void Push16(StateZ80 *state, ExecutionStatsZ80 *stats, Z80StepEvents *events, UINT16 value) {
    state->sp = (UINT16)(state->sp - 1);
    WriteMemory(state, events, state->sp, HighByte(value));
    state->sp = (UINT16)(state->sp - 1);
    WriteMemory(state, events, state->sp, LowByte(value));
    UpdateMinSp(state, stats);
}

static UINT16 Pop16(StateZ80 *state, Z80StepEvents *events) {
    UINT16 value = ReadMemory16(state, events, state->sp);
    state->sp = (UINT16)(state->sp + 2);
    return value;
}

static void SetErrorText(StateZ80 *state, const char *fmt, UINT8 opcode, UINT16 pc) {
    snprintf(state->last_error, sizeof(state->last_error), fmt, opcode, pc);
}

static int ConditionMet(const StateZ80 *state, int condition) {
    switch (condition & 7) {
    case 0:
        return (state->f & FLAG_Z) == 0;
    case 1:
        return (state->f & FLAG_Z) != 0;
    case 2:
        return (state->f & FLAG_C) == 0;
    case 3:
        return (state->f & FLAG_C) != 0;
    case 4:
        return (state->f & FLAG_PV) == 0;
    case 5:
        return (state->f & FLAG_PV) != 0;
    case 6:
        return (state->f & FLAG_S) == 0;
    default:
        return (state->f & FLAG_S) != 0;
    }
}

static UINT8 Add8(StateZ80 *state, UINT8 lhs, UINT8 rhs, UINT8 carry) {
    UINT16 sum = (UINT16)lhs + (UINT16)rhs + (UINT16)carry;
    UINT8 result = (UINT8)sum;
    UINT8 flags = FlagsSZ53(result);
    if (((lhs & 0x0F) + (rhs & 0x0F) + carry) & 0x10) {
        flags |= FLAG_H;
    }
    if (((~(lhs ^ rhs)) & (lhs ^ result) & 0x80) != 0) {
        flags |= FLAG_PV;
    }
    if (sum & 0x100) {
        flags |= FLAG_C;
    }
    state->f = flags;
    return result;
}

static UINT8 Sub8(StateZ80 *state, UINT8 lhs, UINT8 rhs, UINT8 carry, int store_result) {
    UINT16 diff = (UINT16)lhs - (UINT16)rhs - (UINT16)carry;
    UINT8 result = (UINT8)diff;
    UINT8 flags = (UINT8)(FlagsSZ53(result) | FLAG_N);
    if (((lhs ^ rhs ^ result) & 0x10) != 0) {
        flags |= FLAG_H;
    }
    if (((lhs ^ rhs) & (lhs ^ result) & 0x80) != 0) {
        flags |= FLAG_PV;
    }
    if (diff & 0x100) {
        flags |= FLAG_C;
    }
    state->f = flags;
    return store_result ? result : lhs;
}

static UINT8 Inc8(StateZ80 *state, UINT8 value) {
    UINT8 result = (UINT8)(value + 1);
    UINT8 flags = (UINT8)(state->f & FLAG_C);
    flags |= FlagsSZ53(result);
    if (((value & 0x0F) + 1) & 0x10) {
        flags |= FLAG_H;
    }
    if (value == 0x7F) {
        flags |= FLAG_PV;
    }
    state->f = flags;
    return result;
}

static UINT8 Dec8(StateZ80 *state, UINT8 value) {
    UINT8 result = (UINT8)(value - 1);
    UINT8 flags = (UINT8)(state->f & FLAG_C);
    flags |= (UINT8)(FlagsSZ53(result) | FLAG_N);
    if ((value & 0x0F) == 0x00) {
        flags |= FLAG_H;
    }
    if (value == 0x80) {
        flags |= FLAG_PV;
    }
    state->f = flags;
    return result;
}

static UINT8 LogicAnd(StateZ80 *state, UINT8 lhs, UINT8 rhs) {
    UINT8 result = (UINT8)(lhs & rhs);
    state->f = (UINT8)(FlagsSZP53(result) | FLAG_H);
    return result;
}

static UINT8 LogicOr(StateZ80 *state, UINT8 lhs, UINT8 rhs) {
    UINT8 result = (UINT8)(lhs | rhs);
    state->f = FlagsSZP53(result);
    return result;
}

static UINT8 LogicXor(StateZ80 *state, UINT8 lhs, UINT8 rhs) {
    UINT8 result = (UINT8)(lhs ^ rhs);
    state->f = FlagsSZP53(result);
    return result;
}

static void Compare8(StateZ80 *state, UINT8 lhs, UINT8 rhs) {
    UINT16 diff = (UINT16)lhs - (UINT16)rhs;
    UINT8 result = (UINT8)diff;
    UINT8 flags = (UINT8)((result & FLAG_S) | (rhs & (FLAG_5 | FLAG_3)) | FLAG_N);
    if (result == 0) {
        flags |= FLAG_Z;
    }
    if (((lhs ^ rhs ^ result) & 0x10) != 0) {
        flags |= FLAG_H;
    }
    if (((lhs ^ rhs) & (lhs ^ result) & 0x80) != 0) {
        flags |= FLAG_PV;
    }
    if (diff & 0x100) {
        flags |= FLAG_C;
    }
    state->f = flags;
}

static UINT16 Add16(StateZ80 *state, UINT16 lhs, UINT16 rhs) {
    UINT32 sum = (UINT32)lhs + (UINT32)rhs;
    UINT16 result = (UINT16)sum;
    UINT8 flags = (UINT8)(state->f & (FLAG_S | FLAG_Z | FLAG_PV));
    flags |= (UINT8)(HighByte(result) & (FLAG_5 | FLAG_3));
    if (((lhs & 0x0FFF) + (rhs & 0x0FFF)) & 0x1000) {
        flags |= FLAG_H;
    }
    if (sum & 0x10000u) {
        flags |= FLAG_C;
    }
    state->f = flags;
    return result;
}

static UINT16 Add16Carry(StateZ80 *state, UINT16 lhs, UINT16 rhs, UINT8 carry) {
    UINT32 sum = (UINT32)lhs + (UINT32)rhs + (UINT32)carry;
    UINT16 result = (UINT16)sum;
    UINT8 flags = (UINT8)(HighByte(result) & (FLAG_S | FLAG_5 | FLAG_3));
    if (result == 0) {
        flags |= FLAG_Z;
    }
    if (((lhs & 0x0FFF) + (rhs & 0x0FFF) + carry) & 0x1000) {
        flags |= FLAG_H;
    }
    if (((~(lhs ^ rhs)) & (lhs ^ result) & 0x8000) != 0) {
        flags |= FLAG_PV;
    }
    if (sum & 0x10000u) {
        flags |= FLAG_C;
    }
    state->f = flags;
    return result;
}

static UINT16 Sub16Carry(StateZ80 *state, UINT16 lhs, UINT16 rhs, UINT8 carry) {
    UINT32 diff = (UINT32)lhs - (UINT32)rhs - (UINT32)carry;
    UINT16 result = (UINT16)diff;
    UINT8 flags = (UINT8)((HighByte(result) & (FLAG_S | FLAG_5 | FLAG_3)) | FLAG_N);
    if (result == 0) {
        flags |= FLAG_Z;
    }
    if (((lhs ^ rhs ^ result) & 0x1000) != 0) {
        flags |= FLAG_H;
    }
    if (((lhs ^ rhs) & (lhs ^ result) & 0x8000) != 0) {
        flags |= FLAG_PV;
    }
    if (diff & 0x10000u) {
        flags |= FLAG_C;
    }
    state->f = flags;
    return result;
}

static UINT8 RotateLeftCarry(UINT8 value) {
    return (UINT8)((value << 1) | (value >> 7));
}

static UINT8 RotateRightCarry(UINT8 value) {
    return (UINT8)((value >> 1) | (value << 7));
}

static UINT8 RotateCB(StateZ80 *state, UINT8 value, int kind) {
    UINT8 carry_in = (UINT8)((state->f & FLAG_C) ? 1 : 0);
    UINT8 carry_out = 0;
    UINT8 result = value;
    switch (kind) {
    case 0:
        carry_out = (UINT8)((value >> 7) & 1);
        result = RotateLeftCarry(value);
        break;
    case 1:
        carry_out = (UINT8)(value & 1);
        result = RotateRightCarry(value);
        break;
    case 2:
        carry_out = (UINT8)((value >> 7) & 1);
        result = (UINT8)((value << 1) | carry_in);
        break;
    case 3:
        carry_out = (UINT8)(value & 1);
        result = (UINT8)((value >> 1) | (carry_in << 7));
        break;
    case 4:
        carry_out = (UINT8)((value >> 7) & 1);
        result = (UINT8)(value << 1);
        break;
    case 5:
        carry_out = (UINT8)(value & 1);
        result = (UINT8)((value >> 1) | (value & 0x80));
        break;
    case 6:
        carry_out = (UINT8)((value >> 7) & 1);
        result = (UINT8)((value << 1) | 1);
        break;
    default:
        carry_out = (UINT8)(value & 1);
        result = (UINT8)(value >> 1);
        break;
    }
    state->f = FlagsSZP53(result);
    if (carry_out) {
        state->f |= FLAG_C;
    }
    return result;
}

static UINT8 Daa(StateZ80 *state, UINT8 value) {
    UINT8 adjust = 0;
    UINT8 old_carry = (UINT8)(state->f & FLAG_C);
    UINT8 carry = 0;
    if ((state->f & FLAG_H) || (!(state->f & FLAG_N) && (value & 0x0F) > 9)) {
        adjust |= 0x06;
    }
    if (old_carry || (!(state->f & FLAG_N) && value > 0x99)) {
        adjust |= 0x60;
        carry = FLAG_C;
    }
    value = (state->f & FLAG_N) ? (UINT8)(value - adjust) : (UINT8)(value + adjust);
    state->f = (UINT8)((state->f & FLAG_N) | FlagsSZP53(value) | carry);
    if (adjust & 0x06) {
        state->f |= FLAG_H;
    }
    return value;
}

static void ExchangeAF(StateZ80 *state) {
    UINT16 temp = GetAF(state);
    SetAF(state, GetAFAlt(state));
    SetAFAlt(state, temp);
}

static void ExchangeShadow(StateZ80 *state) {
    UINT16 temp = GetBC(state);
    SetBC(state, GetBCAlt(state));
    SetBCAlt(state, temp);
    temp = GetDE(state);
    SetDE(state, GetDEAlt(state));
    SetDEAlt(state, temp);
    temp = GetHL(state);
    SetHL(state, GetHLAlt(state));
    SetHLAlt(state, temp);
}

static void BitTest(StateZ80 *state, UINT8 value, int bit) {
    UINT8 mask = (UINT8)(1u << (bit & 7));
    UINT8 flags = (UINT8)((state->f & FLAG_C) | FLAG_H | (value & (FLAG_5 | FLAG_3)));
    if ((value & mask) == 0) {
        flags |= FLAG_Z | FLAG_PV;
    }
    if (bit == 7 && (value & mask) != 0) {
        flags |= FLAG_S;
    }
    state->f = flags;
}

static void ServiceNMI(StateZ80 *state, ExecutionStatsZ80 *stats, Z80StepEvents *events) {
    state->nmi_pending = 0;
    state->halted = 0;
    state->iff2 = state->iff1;
    state->iff1 = 0;
    Push16(state, stats, events, state->pc);
    state->pc = 0x0066;
    AddCycles(stats, 11);
}

static int ServiceMaskableInterrupt(StateZ80 *state, ExecutionStatsZ80 *stats, Z80StepEvents *events) {
    state->irq_pending = 0;
    state->halted = 0;
    state->iff1 = 0;
    state->iff2 = 0;
    TraceEvent(events, Z80_BUS_EVENT_INTERRUPT_ACK, state->pc, state->irq_vector);
    if (state->im == 2) {
        UINT16 table = Compose16(state->i, (UINT8)(state->irq_vector & 0xFE));
        Push16(state, stats, events, state->pc);
        state->pc = ReadMemory16(state, events, table);
        AddCycles(stats, 19);
        return 1;
    }
    if (state->im == 0) {
        UINT8 vector = state->irq_vector;
        if ((vector & 0xC7) == 0xC7) {
            Push16(state, stats, events, state->pc);
            state->pc = (UINT16)(vector & 0x38);
            AddCycles(stats, 13);
            return 1;
        }
        SetErrorText(state, "unsupported IM 0 interrupt opcode 0x%02X at 0x%04X", vector, state->pc);
        return 0;
    }
    Push16(state, stats, events, state->pc);
    state->pc = 0x0038;
    AddCycles(stats, 13);
    return 1;
}

static Z80StepResult ExecuteCB(StateZ80 *state, ExecutionStatsZ80 *stats, Z80StepEvents *events, UINT8 op,
                               IndexMode index, int indexed_mem, int8_t disp) {
    int group = (op >> 6) & 0x03;
    int y = (op >> 3) & 0x07;
    int z = op & 0x07;
    UINT8 value = ReadReg8(state, events, z, index, indexed_mem, disp);
    UINT8 result = value;

    if (group == 0) {
        result = RotateCB(state, value, y);
        WriteReg8(state, events, z, index, indexed_mem, disp, result);
        if (indexed_mem && z != 6) {
            SetReg8Simple(state, z, INDEX_NONE, result);
        }
        AddCycles(stats, indexed_mem ? 23 : ((z == 6) ? 15 : 8));
        return Z80_STEP_OK;
    }

    if (group == 1) {
        BitTest(state, value, y);
        AddCycles(stats, indexed_mem ? 20 : ((z == 6) ? 12 : 8));
        return Z80_STEP_OK;
    }

    if (group == 2) {
        result = (UINT8)(value & ~(1u << y));
    } else {
        result = (UINT8)(value | (1u << y));
    }
    WriteReg8(state, events, z, index, indexed_mem, disp, result);
    if (indexed_mem && z != 6) {
        SetReg8Simple(state, z, INDEX_NONE, result);
    }
    AddCycles(stats, indexed_mem ? 23 : ((z == 6) ? 15 : 8));
    return Z80_STEP_OK;
}

static Z80StepResult ExecuteED(StateZ80 *state, ExecutionStatsZ80 *stats, Z80StepEvents *events, UINT8 op) {
    UINT16 addr;
    UINT16 value16;
    UINT8 value8;
    switch (op) {
    case 0x40:
    case 0x48:
    case 0x50:
    case 0x58:
    case 0x60:
    case 0x68:
    case 0x78:
        addr = GetBC(state);
        value8 = ReadPort(state, events, addr);
        SetReg8Simple(state, (op >> 3) & 7, INDEX_NONE, value8);
        state->f = (UINT8)((state->f & FLAG_C) | FlagsSZP53(value8));
        AddCycles(stats, 12);
        return Z80_STEP_OK;
    case 0x70:
        value8 = ReadPort(state, events, GetBC(state));
        state->f = (UINT8)((state->f & FLAG_C) | FlagsSZP53(value8));
        AddCycles(stats, 12);
        return Z80_STEP_OK;
    case 0x41:
    case 0x49:
    case 0x51:
    case 0x59:
    case 0x61:
    case 0x69:
    case 0x79:
        WritePort(state, events, GetBC(state), GetReg8Simple(state, (op >> 3) & 7, INDEX_NONE));
        AddCycles(stats, 12);
        return Z80_STEP_OK;
    case 0x71:
        WritePort(state, events, GetBC(state), 0x00);
        AddCycles(stats, 12);
        return Z80_STEP_OK;
    case 0x42:
    case 0x52:
    case 0x62:
    case 0x72:
        value16 = Sub16Carry(state, GetHL(state), GetRP(state, (op >> 4) & 3, INDEX_NONE),
                             (UINT8)((state->f & FLAG_C) ? 1 : 0));
        SetHL(state, value16);
        AddCycles(stats, 15);
        return Z80_STEP_OK;
    case 0x4A:
    case 0x5A:
    case 0x6A:
    case 0x7A:
        value16 = Add16Carry(state, GetHL(state), GetRP(state, (op >> 4) & 3, INDEX_NONE),
                             (UINT8)((state->f & FLAG_C) ? 1 : 0));
        SetHL(state, value16);
        AddCycles(stats, 15);
        return Z80_STEP_OK;
    case 0x43:
    case 0x53:
    case 0x63:
    case 0x73:
        addr = ReadImmediate16(state, events);
        WriteMemory16(state, events, addr, GetRP(state, (op >> 4) & 3, INDEX_NONE));
        AddCycles(stats, 20);
        return Z80_STEP_OK;
    case 0x4B:
    case 0x5B:
    case 0x6B:
    case 0x7B:
        addr = ReadImmediate16(state, events);
        SetRP(state, (op >> 4) & 3, INDEX_NONE, ReadMemory16(state, events, addr));
        AddCycles(stats, 20);
        return Z80_STEP_OK;
    case 0x44:
    case 0x4C:
    case 0x54:
    case 0x5C:
    case 0x64:
    case 0x6C:
    case 0x74:
    case 0x7C:
        state->a = Sub8(state, 0, state->a, 0, 1);
        AddCycles(stats, 8);
        return Z80_STEP_OK;
    case 0x45:
    case 0x55:
    case 0x5D:
    case 0x65:
    case 0x6D:
    case 0x75:
        state->iff1 = state->iff2;
        state->pc = Pop16(state, events);
        AddCycles(stats, 14);
        return Z80_STEP_OK;
    case 0x4D:
        state->pc = Pop16(state, events);
        AddCycles(stats, 14);
        return Z80_STEP_OK;
    case 0x46:
    case 0x4E:
    case 0x66:
    case 0x6E:
        state->im = 0;
        AddCycles(stats, 8);
        return Z80_STEP_OK;
    case 0x56:
    case 0x76:
        state->im = 1;
        AddCycles(stats, 8);
        return Z80_STEP_OK;
    case 0x5E:
    case 0x7E:
        state->im = 2;
        AddCycles(stats, 8);
        return Z80_STEP_OK;
    case 0x47:
        state->i = state->a;
        AddCycles(stats, 9);
        return Z80_STEP_OK;
    case 0x4F:
        state->r = (UINT8)((state->r & 0x80) | (state->a & 0x7F));
        AddCycles(stats, 9);
        return Z80_STEP_OK;
    case 0x57:
        state->a = state->i;
        state->f = (UINT8)((state->f & FLAG_C) | FlagsSZ53(state->a) | (state->iff2 ? FLAG_PV : 0));
        AddCycles(stats, 9);
        return Z80_STEP_OK;
    case 0x5F:
        state->a = state->r;
        state->f = (UINT8)((state->f & FLAG_C) | FlagsSZ53(state->a) | (state->iff2 ? FLAG_PV : 0));
        AddCycles(stats, 9);
        return Z80_STEP_OK;
    case 0x67: {
        UINT8 mem = ReadMemory(state, events, GetHL(state));
        UINT8 new_mem = (UINT8)((mem << 4) | (state->a & 0x0F));
        state->a = (UINT8)((state->a & 0xF0) | (mem >> 4));
        WriteMemory(state, events, GetHL(state), new_mem);
        state->f = (UINT8)((state->f & FLAG_C) | FlagsSZP53(state->a));
        AddCycles(stats, 18);
        return Z80_STEP_OK;
    }
    case 0x6F: {
        UINT8 mem = ReadMemory(state, events, GetHL(state));
        UINT8 new_mem = (UINT8)((state->a << 4) | (mem >> 4));
        state->a = (UINT8)((state->a & 0xF0) | (mem & 0x0F));
        WriteMemory(state, events, GetHL(state), new_mem);
        state->f = (UINT8)((state->f & FLAG_C) | FlagsSZP53(state->a));
        AddCycles(stats, 18);
        return Z80_STEP_OK;
    }
    case 0xA0:
    case 0xA8:
    case 0xB0:
    case 0xB8: {
        int decrement = (op & 0x08) != 0;
        int repeat = (op & 0x10) != 0;
        UINT8 mem = ReadMemory(state, events, GetHL(state));
        WriteMemory(state, events, GetDE(state), mem);
        SetHL(state, (UINT16)(GetHL(state) + (decrement ? -1 : 1)));
        SetDE(state, (UINT16)(GetDE(state) + (decrement ? -1 : 1)));
        SetBC(state, (UINT16)(GetBC(state) - 1));
        state->f = (UINT8)((state->f & FLAG_C) | (((GetBC(state) != 0) ? FLAG_PV : 0)));
        state->f |= (UINT8)(((state->a + mem) & (FLAG_5 | FLAG_3)));
        AddCycles(stats, repeat && GetBC(state) != 0 ? 21 : 16);
        if (repeat && GetBC(state) != 0) {
            state->pc = (UINT16)(state->pc - 2);
        }
        return Z80_STEP_OK;
    }
    case 0xA1:
    case 0xA9:
    case 0xB1:
    case 0xB9: {
        int decrement = (op & 0x08) != 0;
        int repeat = (op & 0x10) != 0;
        UINT8 mem = ReadMemory(state, events, GetHL(state));
        UINT8 result = (UINT8)(state->a - mem);
        UINT16 bc = (UINT16)(GetBC(state) - 1);
        SetBC(state, bc);
        SetHL(state, (UINT16)(GetHL(state) + (decrement ? -1 : 1)));
        state->f = (UINT8)((state->f & FLAG_C) | FLAG_N | (result & FLAG_S));
        if (result == 0) {
            state->f |= FLAG_Z;
        }
        if (((state->a ^ mem ^ result) & 0x10) != 0) {
            state->f |= FLAG_H;
        }
        if (bc != 0) {
            state->f |= FLAG_PV;
        }
        state->f |= (UINT8)(result & (FLAG_5 | FLAG_3));
        AddCycles(stats, repeat && bc != 0 && result != 0 ? 21 : 16);
        if (repeat && bc != 0 && result != 0) {
            state->pc = (UINT16)(state->pc - 2);
        }
        return Z80_STEP_OK;
    }
    case 0xA2:
    case 0xAA:
    case 0xB2:
    case 0xBA: {
        int decrement = (op & 0x08) != 0;
        int repeat = (op & 0x10) != 0;
        UINT8 data = ReadPort(state, events, GetBC(state));
        WriteMemory(state, events, GetHL(state), data);
        state->b = (UINT8)(state->b - 1);
        SetHL(state, (UINT16)(GetHL(state) + (decrement ? -1 : 1)));
        state->f = (UINT8)((state->b == 0 ? FLAG_Z : 0) | FLAG_N);
        AddCycles(stats, repeat && state->b != 0 ? 21 : 16);
        if (repeat && state->b != 0) {
            state->pc = (UINT16)(state->pc - 2);
        }
        return Z80_STEP_OK;
    }
    case 0xA3:
    case 0xAB:
    case 0xB3:
    case 0xBB: {
        int decrement = (op & 0x08) != 0;
        int repeat = (op & 0x10) != 0;
        UINT8 data = ReadMemory(state, events, GetHL(state));
        WritePort(state, events, GetBC(state), data);
        state->b = (UINT8)(state->b - 1);
        SetHL(state, (UINT16)(GetHL(state) + (decrement ? -1 : 1)));
        state->f = (UINT8)((state->b == 0 ? FLAG_Z : 0) | FLAG_N);
        AddCycles(stats, repeat && state->b != 0 ? 21 : 16);
        if (repeat && state->b != 0) {
            state->pc = (UINT16)(state->pc - 2);
        }
        return Z80_STEP_OK;
    }
    default:
        AddCycles(stats, 8);
        return Z80_STEP_OK;
    }
}

Z80StepResult EmulateZ80Op(StateZ80 *state, ExecutionStatsZ80 *stats, Z80StepEvents *events) {
    Z80StepResult result = Z80_STEP_UNIMPLEMENTED;
    UINT8 suppress_irq;
    UINT8 op;
    IndexMode index = INDEX_NONE;
    int set_ei_pending = 0;

    if (!state || !state->memory) {
        return Z80_STEP_UNIMPLEMENTED;
    }

    memset(state->last_error, 0, sizeof(state->last_error));
    if (events) {
        events->count = 0;
    }

    suppress_irq = state->ei_pending;
    state->ei_pending = 0;

    if (state->nmi_pending) {
        ServiceNMI(state, stats, events);
        result = Z80_STEP_OK;
        goto done;
    }
    if (state->irq_pending && state->iff1 && !suppress_irq) {
        if (!ServiceMaskableInterrupt(state, stats, events)) {
            result = Z80_STEP_UNIMPLEMENTED;
            goto done;
        }
        result = Z80_STEP_OK;
        goto done;
    }
    if (state->halted) {
        result = Z80_STEP_HALT;
        goto done;
    }

    op = FetchOpcodeByte(state, events);
    while (op == 0xDD || op == 0xFD) {
        index = (op == 0xDD) ? INDEX_IX : INDEX_IY;
        op = FetchOpcodeByte(state, events);
    }

    if (op == 0xCB) {
        if (index != INDEX_NONE) {
            int8_t disp = (int8_t)ReadImmediate8(state, events);
            UINT8 cbop = ReadImmediate8(state, events);
            result = ExecuteCB(state, stats, events, cbop, index, 1, disp);
        } else {
            UINT8 cbop = FetchOpcodeByte(state, events);
            result = ExecuteCB(state, stats, events, cbop, INDEX_NONE, 0, 0);
        }
        goto done;
    }

    if (op == 0xED) {
        UINT8 edop = FetchOpcodeByte(state, events);
        result = ExecuteED(state, stats, events, edop);
        goto done;
    }

    switch (op) {
    case 0x00:
        STEP_OK(4);
    case 0x01:
    case 0x11:
    case 0x21:
    case 0x31:
        SetRP(state, (op >> 4) & 3, (op == 0x21 && index != INDEX_NONE) ? index : INDEX_NONE, ReadImmediate16(state, events));
        STEP_OK(10);
    case 0x02:
        WriteMemory(state, events, GetBC(state), state->a);
        STEP_OK(7);
    case 0x03:
    case 0x13:
    case 0x23:
    case 0x33:
        SetRP(state, (op >> 4) & 3, (op == 0x23 && index != INDEX_NONE) ? index : INDEX_NONE,
              (UINT16)(GetRP(state, (op >> 4) & 3, (op == 0x23 && index != INDEX_NONE) ? index : INDEX_NONE) + 1));
        STEP_OK((op == 0x23 && index != INDEX_NONE) ? 10 : 6);
    case 0x04:
    case 0x0C:
    case 0x14:
    case 0x1C:
    case 0x24:
    case 0x2C:
    case 0x3C: {
        int reg = (op >> 3) & 7;
        UINT8 value = GetReg8Simple(state, reg, (reg >= 4 && reg <= 5) ? index : INDEX_NONE);
        SetReg8Simple(state, reg, (reg >= 4 && reg <= 5) ? index : INDEX_NONE, Inc8(state, value));
        STEP_OK(((reg >= 4 && reg <= 5) && index != INDEX_NONE) ? 8 : 4);
    }
    case 0x05:
    case 0x0D:
    case 0x15:
    case 0x1D:
    case 0x25:
    case 0x2D:
    case 0x3D: {
        int reg = (op >> 3) & 7;
        UINT8 value = GetReg8Simple(state, reg, (reg >= 4 && reg <= 5) ? index : INDEX_NONE);
        SetReg8Simple(state, reg, (reg >= 4 && reg <= 5) ? index : INDEX_NONE, Dec8(state, value));
        STEP_OK(((reg >= 4 && reg <= 5) && index != INDEX_NONE) ? 8 : 4);
    }
    case 0x06:
    case 0x0E:
    case 0x16:
    case 0x1E:
    case 0x26:
    case 0x2E:
    case 0x3E: {
        int reg = (op >> 3) & 7;
        UINT8 imm = ReadImmediate8(state, events);
        SetReg8Simple(state, reg, (reg >= 4 && reg <= 5) ? index : INDEX_NONE, imm);
        STEP_OK(((reg >= 4 && reg <= 5) && index != INDEX_NONE) ? 11 : 7);
    }
    case 0x07:
        state->a = RotateLeftCarry(state->a);
        state->f = (UINT8)((state->f & (FLAG_S | FLAG_Z | FLAG_PV)) | (state->a & (FLAG_5 | FLAG_3)) |
                           ((state->a & 1) ? FLAG_C : 0));
        STEP_OK(4);
    case 0x08:
        ExchangeAF(state);
        STEP_OK(4);
    case 0x09:
    case 0x19:
    case 0x29:
    case 0x39: {
        IndexMode pair_index = index;
        UINT16 lhs = GetIndexValue(state, pair_index);
        UINT16 rhs = GetRP(state, (op >> 4) & 3, pair_index);
        SetIndexValue(state, pair_index, Add16(state, lhs, rhs));
        STEP_OK(index != INDEX_NONE ? 15 : 11);
    }
    case 0x0A:
        state->a = ReadMemory(state, events, GetBC(state));
        STEP_OK(7);
    case 0x0B:
    case 0x1B:
    case 0x2B:
    case 0x3B:
        SetRP(state, (op >> 4) & 3, (op == 0x2B && index != INDEX_NONE) ? index : INDEX_NONE,
              (UINT16)(GetRP(state, (op >> 4) & 3, (op == 0x2B && index != INDEX_NONE) ? index : INDEX_NONE) - 1));
        STEP_OK((op == 0x2B && index != INDEX_NONE) ? 10 : 6);
    case 0x0F:
        state->f = (UINT8)((state->f & (FLAG_S | FLAG_Z | FLAG_PV)) | (state->a & 1 ? FLAG_C : 0));
        state->a = RotateRightCarry(state->a);
        state->f |= (UINT8)(state->a & (FLAG_5 | FLAG_3));
        STEP_OK(4);
    case 0x10: {
        int8_t disp = (int8_t)ReadImmediate8(state, events);
        state->b = (UINT8)(state->b - 1);
        if (state->b != 0) {
            state->pc = (UINT16)(state->pc + disp);
            STEP_OK(13);
        }
        STEP_OK(8);
    }
    case 0x12:
        WriteMemory(state, events, GetDE(state), state->a);
        STEP_OK(7);
    case 0x17: {
        UINT8 old_carry = (UINT8)((state->f & FLAG_C) ? 1 : 0);
        UINT8 new_carry = (UINT8)((state->a >> 7) & 1);
        state->a = (UINT8)((state->a << 1) | old_carry);
        state->f = (UINT8)((state->f & (FLAG_S | FLAG_Z | FLAG_PV)) | (state->a & (FLAG_5 | FLAG_3)) |
                           (new_carry ? FLAG_C : 0));
        STEP_OK(4);
    }
    case 0x18: {
        int8_t disp = (int8_t)ReadImmediate8(state, events);
        state->pc = (UINT16)(state->pc + disp);
        STEP_OK(12);
    }
    case 0x1A:
        state->a = ReadMemory(state, events, GetDE(state));
        STEP_OK(7);
    case 0x1F: {
        UINT8 old_carry = (UINT8)((state->f & FLAG_C) ? 1 : 0);
        UINT8 new_carry = (UINT8)(state->a & 1);
        state->a = (UINT8)((state->a >> 1) | (old_carry << 7));
        state->f = (UINT8)((state->f & (FLAG_S | FLAG_Z | FLAG_PV)) | (state->a & (FLAG_5 | FLAG_3)) |
                           (new_carry ? FLAG_C : 0));
        STEP_OK(4);
    }
    case 0x20:
    case 0x28:
    case 0x30:
    case 0x38: {
        int8_t disp = (int8_t)ReadImmediate8(state, events);
        if (ConditionMet(state, (op >> 3) & 0x03)) {
            state->pc = (UINT16)(state->pc + disp);
            STEP_OK(12);
        }
        STEP_OK(7);
    }
    case 0x22: {
        UINT16 addr = ReadImmediate16(state, events);
        WriteMemory16(state, events, addr, GetIndexValue(state, index));
        STEP_OK(index != INDEX_NONE ? 20 : 16);
    }
    case 0x27:
        state->a = Daa(state, state->a);
        STEP_OK(4);
    case 0x2A: {
        UINT16 addr = ReadImmediate16(state, events);
        SetIndexValue(state, index, ReadMemory16(state, events, addr));
        STEP_OK(index != INDEX_NONE ? 20 : 16);
    }
    case 0x2F:
        state->a ^= 0xFF;
        state->f = (UINT8)((state->f & (FLAG_S | FLAG_Z | FLAG_PV | FLAG_C)) | FLAG_H | FLAG_N |
                           (state->a & (FLAG_5 | FLAG_3)));
        STEP_OK(4);
    case 0x32: {
        UINT16 addr = ReadImmediate16(state, events);
        WriteMemory(state, events, addr, state->a);
        STEP_OK(13);
    }
    case 0x34:
    case 0x35:
    case 0x36: {
        int8_t disp = 0;
        int indexed_mem = index != INDEX_NONE;
        UINT16 addr;
        if (indexed_mem) {
            disp = (int8_t)ReadImmediate8(state, events);
            addr = EffectiveIndexedAddress(state, index, disp);
        } else {
            addr = GetHL(state);
        }
        if (op == 0x34) {
            UINT8 value = ReadMemory(state, events, addr);
            value = Inc8(state, value);
            WriteMemory(state, events, addr, value);
        } else if (op == 0x35) {
            UINT8 value = ReadMemory(state, events, addr);
            value = Dec8(state, value);
            WriteMemory(state, events, addr, value);
        } else {
            WriteMemory(state, events, addr, ReadImmediate8(state, events));
        }
        STEP_OK(indexed_mem ? ((op == 0x36) ? 19 : 23) : ((op == 0x36) ? 10 : 11));
    }
    case 0x37:
        state->f = (UINT8)((state->f & (FLAG_S | FLAG_Z | FLAG_PV)) | (state->a & (FLAG_5 | FLAG_3)) | FLAG_C);
        STEP_OK(4);
    case 0x3A: {
        UINT16 addr = ReadImmediate16(state, events);
        state->a = ReadMemory(state, events, addr);
        STEP_OK(13);
    }
    case 0x3F: {
        UINT8 old_carry = (UINT8)((state->f & FLAG_C) ? 1 : 0);
        state->f = (UINT8)((state->f & (FLAG_S | FLAG_Z | FLAG_PV)) | (state->a & (FLAG_5 | FLAG_3)) |
                           (old_carry ? FLAG_H : 0) | (old_carry ? 0 : FLAG_C));
        STEP_OK(4);
    }
    case 0x76:
        STEP_HALT(4);
    case 0xD9:
        ExchangeShadow(state);
        STEP_OK(4);
    case 0xDB: {
        UINT8 port = ReadImmediate8(state, events);
        state->a = ReadPort(state, events, Compose16(state->a, port));
        STEP_OK(11);
    }
    case 0xD3: {
        UINT8 port = ReadImmediate8(state, events);
        WritePort(state, events, Compose16(state->a, port), state->a);
        STEP_OK(11);
    }
    case 0xE3: {
        UINT16 value = ReadMemory16(state, events, state->sp);
        WriteMemory16(state, events, state->sp, GetIndexValue(state, index));
        SetIndexValue(state, index, value);
        STEP_OK(index != INDEX_NONE ? 23 : 19);
    }
    case 0xE9:
        state->pc = GetIndexValue(state, index);
        STEP_OK(4);
    case 0xEB: {
        UINT16 temp = GetDE(state);
        SetDE(state, GetHL(state));
        SetHL(state, temp);
        STEP_OK(4);
    }
    case 0xF3:
        state->iff1 = 0;
        state->iff2 = 0;
        STEP_OK(4);
    case 0xF9:
        state->sp = GetIndexValue(state, index);
        STEP_OK(index != INDEX_NONE ? 10 : 6);
    case 0xFB:
        state->iff1 = 1;
        state->iff2 = 1;
        set_ei_pending = 1;
        STEP_OK(4);
    default:
        break;
    }

    if ((op & 0xC0) == 0x40 && op != 0x76) {
        int dst = (op >> 3) & 7;
        int src = op & 7;
        int indexed = index != INDEX_NONE && (dst == 6 || src == 6);
        int8_t disp = 0;
        if (indexed) {
            disp = (int8_t)ReadImmediate8(state, events);
        }
        WriteReg8(state, events, dst, index, indexed, disp, ReadReg8(state, events, src, index, indexed, disp));
        STEP_OK(indexed ? ((dst == 6 || src == 6) ? 19 : 8) : ((dst == 6 || src == 6) ? 7 : 4));
    }

    if ((op & 0xC7) == 0x06) {
        int reg = (op >> 3) & 7;
        if (reg == 6) {
            int8_t disp = 0;
            UINT16 addr;
            if (index != INDEX_NONE) {
                disp = (int8_t)ReadImmediate8(state, events);
                addr = EffectiveIndexedAddress(state, index, disp);
            } else {
                addr = GetHL(state);
            }
            WriteMemory(state, events, addr, ReadImmediate8(state, events));
            STEP_OK(index != INDEX_NONE ? 19 : 10);
        }
        SetReg8Simple(state, reg, (reg >= 4 && reg <= 5) ? index : INDEX_NONE, ReadImmediate8(state, events));
        STEP_OK(((reg >= 4 && reg <= 5) && index != INDEX_NONE) ? 11 : 7);
    }

    if ((op & 0xC7) == 0x04 || (op & 0xC7) == 0x05) {
        int reg = (op >> 3) & 7;
        if (reg == 6) {
            int8_t disp = 0;
            UINT16 addr;
            UINT8 value;
            if (index != INDEX_NONE) {
                disp = (int8_t)ReadImmediate8(state, events);
                addr = EffectiveIndexedAddress(state, index, disp);
            } else {
                addr = GetHL(state);
            }
            value = ReadMemory(state, events, addr);
            value = ((op & 0xC7) == 0x04) ? Inc8(state, value) : Dec8(state, value);
            WriteMemory(state, events, addr, value);
            STEP_OK(index != INDEX_NONE ? 23 : 11);
        }
        if ((op & 0xC7) == 0x04) {
            SetReg8Simple(state, reg, (reg >= 4 && reg <= 5) ? index : INDEX_NONE,
                          Inc8(state, GetReg8Simple(state, reg, (reg >= 4 && reg <= 5) ? index : INDEX_NONE)));
        } else {
            SetReg8Simple(state, reg, (reg >= 4 && reg <= 5) ? index : INDEX_NONE,
                          Dec8(state, GetReg8Simple(state, reg, (reg >= 4 && reg <= 5) ? index : INDEX_NONE)));
        }
        STEP_OK(((reg >= 4 && reg <= 5) && index != INDEX_NONE) ? 8 : 4);
    }

    if ((op & 0xF8) == 0x80 || (op & 0xF8) == 0x88 || (op & 0xF8) == 0x90 || (op & 0xF8) == 0x98 ||
        (op & 0xF8) == 0xA0 || (op & 0xF8) == 0xA8 || (op & 0xF8) == 0xB0 || (op & 0xF8) == 0xB8) {
        int src = op & 7;
        int indexed = index != INDEX_NONE && src == 6;
        int8_t disp = indexed ? (int8_t)ReadImmediate8(state, events) : 0;
        UINT8 value = ReadReg8(state, events, src, index, indexed, disp);
        switch (op & 0xF8) {
        case 0x80:
            state->a = Add8(state, state->a, value, 0);
            break;
        case 0x88:
            state->a = Add8(state, state->a, value, (UINT8)((state->f & FLAG_C) ? 1 : 0));
            break;
        case 0x90:
            state->a = Sub8(state, state->a, value, 0, 1);
            break;
        case 0x98:
            state->a = Sub8(state, state->a, value, (UINT8)((state->f & FLAG_C) ? 1 : 0), 1);
            break;
        case 0xA0:
            state->a = LogicAnd(state, state->a, value);
            break;
        case 0xA8:
            state->a = LogicXor(state, state->a, value);
            break;
        case 0xB0:
            state->a = LogicOr(state, state->a, value);
            break;
        default:
            Compare8(state, state->a, value);
            break;
        }
        STEP_OK(indexed ? 19 : (src == 6 ? 7 : 4));
    }

    switch (op) {
    case 0xC0:
    case 0xC8:
    case 0xD0:
    case 0xD8:
    case 0xE0:
    case 0xE8:
    case 0xF0:
    case 0xF8:
        if (ConditionMet(state, (op >> 3) & 7)) {
            state->pc = Pop16(state, events);
            STEP_OK(11);
        }
        STEP_OK(5);
    case 0xC1:
    case 0xD1:
    case 0xE1:
    case 0xF1:
        SetRP2(state, (op >> 4) & 3, index, Pop16(state, events));
        STEP_OK((index != INDEX_NONE && (op == 0xE1)) ? 14 : 10);
    case 0xC2:
    case 0xCA:
    case 0xD2:
    case 0xDA:
    case 0xE2:
    case 0xEA:
    case 0xF2:
    case 0xFA: {
        UINT16 addr = ReadImmediate16(state, events);
        if (ConditionMet(state, (op >> 3) & 7)) {
            state->pc = addr;
        }
        STEP_OK(10);
    }
    case 0xC3:
        state->pc = ReadImmediate16(state, events);
        STEP_OK(10);
    case 0xC4:
    case 0xCC:
    case 0xD4:
    case 0xDC:
    case 0xE4:
    case 0xEC:
    case 0xF4:
    case 0xFC: {
        UINT16 addr = ReadImmediate16(state, events);
        if (ConditionMet(state, (op >> 3) & 7)) {
            Push16(state, stats, events, state->pc);
            state->pc = addr;
            STEP_OK(17);
        }
        STEP_OK(10);
    }
    case 0xC5:
    case 0xD5:
    case 0xE5:
    case 0xF5:
        Push16(state, stats, events, GetRP2(state, (op >> 4) & 3, index));
        STEP_OK((index != INDEX_NONE && op == 0xE5) ? 15 : 11);
    case 0xC6:
        state->a = Add8(state, state->a, ReadImmediate8(state, events), 0);
        STEP_OK(7);
    case 0xC7:
    case 0xCF:
    case 0xD7:
    case 0xDF:
    case 0xE7:
    case 0xEF:
    case 0xF7:
    case 0xFF:
        Push16(state, stats, events, state->pc);
        state->pc = (UINT16)(op & 0x38);
        STEP_OK(11);
    case 0xC9:
        state->pc = Pop16(state, events);
        STEP_OK(10);
    case 0xCD: {
        UINT16 addr = ReadImmediate16(state, events);
        Push16(state, stats, events, state->pc);
        state->pc = addr;
        STEP_OK(17);
    }
    case 0xCE:
        state->a = Add8(state, state->a, ReadImmediate8(state, events), (UINT8)((state->f & FLAG_C) ? 1 : 0));
        STEP_OK(7);
    case 0xD6:
        state->a = Sub8(state, state->a, ReadImmediate8(state, events), 0, 1);
        STEP_OK(7);
    case 0xDE:
        state->a = Sub8(state, state->a, ReadImmediate8(state, events), (UINT8)((state->f & FLAG_C) ? 1 : 0), 1);
        STEP_OK(7);
    case 0xE6:
        state->a = LogicAnd(state, state->a, ReadImmediate8(state, events));
        STEP_OK(7);
    case 0xEE:
        state->a = LogicXor(state, state->a, ReadImmediate8(state, events));
        STEP_OK(7);
    case 0xF6:
        state->a = LogicOr(state, state->a, ReadImmediate8(state, events));
        STEP_OK(7);
    case 0xFE:
        Compare8(state, state->a, ReadImmediate8(state, events));
        STEP_OK(7);
    default:
        break;
    }

    SetErrorText(state, "unimplemented opcode 0x%02X at 0x%04X", op, (UINT16)(state->pc - 1));
    result = Z80_STEP_UNIMPLEMENTED;

done:
    if (set_ei_pending) {
        state->ei_pending = 1;
    }
    return result;
}
