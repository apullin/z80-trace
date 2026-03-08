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

static UINT16 GetHL(const StateZ80 *state) {
    return (UINT16)((state->h << 8) | state->l);
}

static void SetHL(StateZ80 *state, UINT16 value) {
    state->h = (UINT8)(value >> 8);
    state->l = (UINT8)(value & 0xFF);
}

static UINT16 Read16(const UINT8 *mem, UINT16 addr) {
    return (UINT16)(mem[addr] | (mem[(UINT16)(addr + 1)] << 8));
}

static void Write16(UINT8 *mem, UINT16 addr, UINT16 value) {
    mem[addr] = (UINT8)(value & 0xFF);
    mem[(UINT16)(addr + 1)] = (UINT8)(value >> 8);
}

static int Parity(UINT8 value) {
    int ones = 0;
    for (int i = 0; i < 8; ++i) {
        ones += (value >> i) & 1;
    }
    return (ones % 2) == 0;
}

static UINT8 FlagsFromResult(UINT8 result) {
    UINT8 flags = 0;
    if (result & 0x80) {
        flags |= FLAG_S;
    }
    if (result == 0) {
        flags |= FLAG_Z;
    }
    if (result & 0x20) {
        flags |= FLAG_5;
    }
    if (result & 0x08) {
        flags |= FLAG_3;
    }
    return flags;
}

static UINT8 Add8(StateZ80 *state, UINT8 lhs, UINT8 rhs, UINT8 carry_in) {
    UINT16 sum = (UINT16)lhs + (UINT16)rhs + (UINT16)carry_in;
    UINT8 result = (UINT8)sum;
    UINT8 flags = FlagsFromResult(result);

    if (((lhs & 0x0F) + (rhs & 0x0F) + carry_in) & 0x10) {
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

static UINT8 Sub8(StateZ80 *state, UINT8 lhs, UINT8 rhs, UINT8 carry_in, int store_result) {
    UINT16 diff = (UINT16)lhs - (UINT16)rhs - (UINT16)carry_in;
    UINT8 result = (UINT8)diff;
    UINT8 flags = FlagsFromResult(result) | FLAG_N;

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
    if (store_result) {
        return result;
    }
    return lhs;
}

static UINT8 Inc8(StateZ80 *state, UINT8 value) {
    UINT8 result = (UINT8)(value + 1);
    UINT8 flags = (UINT8)(state->f & FLAG_C);
    flags |= FlagsFromResult(result);
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
    flags |= FlagsFromResult(result);
    flags |= FLAG_N;
    if ((value & 0x0F) == 0x00) {
        flags |= FLAG_H;
    }
    if (value == 0x80) {
        flags |= FLAG_PV;
    }
    state->f = flags;
    return result;
}

static UINT8 LogicXor(StateZ80 *state, UINT8 lhs, UINT8 rhs) {
    UINT8 result = (UINT8)(lhs ^ rhs);
    UINT8 flags = FlagsFromResult(result);
    if (Parity(result)) {
        flags |= FLAG_PV;
    }
    state->f = flags;
    return result;
}

static UINT8 LogicOr(StateZ80 *state, UINT8 lhs, UINT8 rhs) {
    UINT8 result = (UINT8)(lhs | rhs);
    UINT8 flags = FlagsFromResult(result);
    if (Parity(result)) {
        flags |= FLAG_PV;
    }
    state->f = flags;
    return result;
}

static UINT8 ReadReg8(const StateZ80 *state, int index) {
    switch (index & 7) {
    case 0:
        return state->b;
    case 1:
        return state->c;
    case 2:
        return state->d;
    case 3:
        return state->e;
    case 4:
        return state->h;
    case 5:
        return state->l;
    case 6:
        return state->memory[GetHL(state)];
    default:
        return state->a;
    }
}

static void WriteReg8(StateZ80 *state, int index, UINT8 value) {
    switch (index & 7) {
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
        state->h = value;
        break;
    case 5:
        state->l = value;
        break;
    case 6:
        state->memory[GetHL(state)] = value;
        break;
    default:
        state->a = value;
        break;
    }
}

static void UpdateMinSp(StateZ80 *state, ExecutionStatsZ80 *stats) {
    if (!stats) {
        return;
    }
    if (!stats->min_sp_set || state->sp < stats->min_sp) {
        stats->min_sp = state->sp;
        stats->min_sp_set = true;
    }
}

static void AddCycles(ExecutionStatsZ80 *stats, UINT64 cycles) {
    if (stats) {
        stats->total_tstates += cycles;
    }
}

static void SetError(StateZ80 *state, UINT8 opcode) {
    snprintf(state->last_error, sizeof(state->last_error), "unimplemented opcode 0x%02X at 0x%04X", opcode, state->pc);
}

static void Push16(StateZ80 *state, ExecutionStatsZ80 *stats, UINT16 value) {
    state->sp = (UINT16)(state->sp - 1);
    state->memory[state->sp] = (UINT8)(value >> 8);
    state->sp = (UINT16)(state->sp - 1);
    state->memory[state->sp] = (UINT8)(value & 0xFF);
    UpdateMinSp(state, stats);
}

static UINT16 Pop16(StateZ80 *state) {
    UINT16 value = Read16(state->memory, state->sp);
    state->sp = (UINT16)(state->sp + 2);
    return value;
}

Z80StepResult EmulateZ80Op(StateZ80 *state, ExecutionStatsZ80 *stats) {
    UINT8 op;
    UINT16 addr;
    UINT8 imm8;
    UINT8 value;

    if (!state || !state->memory) {
        return Z80_STEP_UNIMPLEMENTED;
    }

    memset(state->last_error, 0, sizeof(state->last_error));
    state->r = (UINT8)((state->r & 0x80) | ((state->r + 1) & 0x7F));

    op = state->memory[state->pc];

    switch (op) {
    case 0x00:
        state->pc += 1;
        AddCycles(stats, 4);
        return Z80_STEP_OK;
    case 0x01:
        state->c = state->memory[(UINT16)(state->pc + 1)];
        state->b = state->memory[(UINT16)(state->pc + 2)];
        state->pc += 3;
        AddCycles(stats, 10);
        return Z80_STEP_OK;
    case 0x11:
        state->e = state->memory[(UINT16)(state->pc + 1)];
        state->d = state->memory[(UINT16)(state->pc + 2)];
        state->pc += 3;
        AddCycles(stats, 10);
        return Z80_STEP_OK;
    case 0x21:
        state->l = state->memory[(UINT16)(state->pc + 1)];
        state->h = state->memory[(UINT16)(state->pc + 2)];
        state->pc += 3;
        AddCycles(stats, 10);
        return Z80_STEP_OK;
    case 0x22:
        addr = Read16(state->memory, (UINT16)(state->pc + 1));
        Write16(state->memory, addr, GetHL(state));
        state->pc += 3;
        AddCycles(stats, 16);
        return Z80_STEP_OK;
    case 0x23:
        SetHL(state, (UINT16)(GetHL(state) + 1));
        state->pc += 1;
        AddCycles(stats, 6);
        return Z80_STEP_OK;
    case 0x2A:
        addr = Read16(state->memory, (UINT16)(state->pc + 1));
        SetHL(state, Read16(state->memory, addr));
        state->pc += 3;
        AddCycles(stats, 16);
        return Z80_STEP_OK;
    case 0x31:
        state->sp = Read16(state->memory, (UINT16)(state->pc + 1));
        UpdateMinSp(state, stats);
        state->pc += 3;
        AddCycles(stats, 10);
        return Z80_STEP_OK;
    case 0x32:
        addr = Read16(state->memory, (UINT16)(state->pc + 1));
        state->memory[addr] = state->a;
        state->pc += 3;
        AddCycles(stats, 13);
        return Z80_STEP_OK;
    case 0x3A:
        addr = Read16(state->memory, (UINT16)(state->pc + 1));
        state->a = state->memory[addr];
        state->pc += 3;
        AddCycles(stats, 13);
        return Z80_STEP_OK;
    case 0x76:
        state->halted = 1;
        state->pc += 1;
        AddCycles(stats, 4);
        return Z80_STEP_HALT;
    case 0xC3:
        state->pc = Read16(state->memory, (UINT16)(state->pc + 1));
        AddCycles(stats, 10);
        return Z80_STEP_OK;
    case 0xC6:
        imm8 = state->memory[(UINT16)(state->pc + 1)];
        state->a = Add8(state, state->a, imm8, 0);
        state->pc += 2;
        AddCycles(stats, 7);
        return Z80_STEP_OK;
    case 0xC9:
        state->pc = Pop16(state);
        AddCycles(stats, 10);
        return Z80_STEP_OK;
    case 0xCD:
        addr = Read16(state->memory, (UINT16)(state->pc + 1));
        Push16(state, stats, (UINT16)(state->pc + 3));
        state->pc = addr;
        AddCycles(stats, 17);
        return Z80_STEP_OK;
    case 0xD6:
        imm8 = state->memory[(UINT16)(state->pc + 1)];
        state->a = Sub8(state, state->a, imm8, 0, 1);
        state->pc += 2;
        AddCycles(stats, 7);
        return Z80_STEP_OK;
    case 0xEE:
        imm8 = state->memory[(UINT16)(state->pc + 1)];
        state->a = LogicXor(state, state->a, imm8);
        state->pc += 2;
        AddCycles(stats, 7);
        return Z80_STEP_OK;
    case 0xF6:
        imm8 = state->memory[(UINT16)(state->pc + 1)];
        state->a = LogicOr(state, state->a, imm8);
        state->pc += 2;
        AddCycles(stats, 7);
        return Z80_STEP_OK;
    case 0xFE:
        imm8 = state->memory[(UINT16)(state->pc + 1)];
        (void)Sub8(state, state->a, imm8, 0, 0);
        state->pc += 2;
        AddCycles(stats, 7);
        return Z80_STEP_OK;
    case 0x18:
        imm8 = state->memory[(UINT16)(state->pc + 1)];
        state->pc = (UINT16)(state->pc + 2 + (int8_t)imm8);
        AddCycles(stats, 12);
        return Z80_STEP_OK;
    case 0x20:
        imm8 = state->memory[(UINT16)(state->pc + 1)];
        if ((state->f & FLAG_Z) == 0) {
            state->pc = (UINT16)(state->pc + 2 + (int8_t)imm8);
            AddCycles(stats, 12);
        } else {
            state->pc += 2;
            AddCycles(stats, 7);
        }
        return Z80_STEP_OK;
    case 0x28:
        imm8 = state->memory[(UINT16)(state->pc + 1)];
        if (state->f & FLAG_Z) {
            state->pc = (UINT16)(state->pc + 2 + (int8_t)imm8);
            AddCycles(stats, 12);
        } else {
            state->pc += 2;
            AddCycles(stats, 7);
        }
        return Z80_STEP_OK;
    case 0x30:
        imm8 = state->memory[(UINT16)(state->pc + 1)];
        if ((state->f & FLAG_C) == 0) {
            state->pc = (UINT16)(state->pc + 2 + (int8_t)imm8);
            AddCycles(stats, 12);
        } else {
            state->pc += 2;
            AddCycles(stats, 7);
        }
        return Z80_STEP_OK;
    case 0x38:
        imm8 = state->memory[(UINT16)(state->pc + 1)];
        if (state->f & FLAG_C) {
            state->pc = (UINT16)(state->pc + 2 + (int8_t)imm8);
            AddCycles(stats, 12);
        } else {
            state->pc += 2;
            AddCycles(stats, 7);
        }
        return Z80_STEP_OK;
    default:
        break;
    }

    if ((op & 0xC7) == 0x06) {
        imm8 = state->memory[(UINT16)(state->pc + 1)];
        WriteReg8(state, (op >> 3) & 7, imm8);
        state->pc += 2;
        AddCycles(stats, ((op >> 3) & 7) == 6 ? 10 : 7);
        return Z80_STEP_OK;
    }

    if ((op & 0xC7) == 0x04) {
        int index = (op >> 3) & 7;
        value = ReadReg8(state, index);
        value = Inc8(state, value);
        WriteReg8(state, index, value);
        state->pc += 1;
        AddCycles(stats, index == 6 ? 11 : 4);
        return Z80_STEP_OK;
    }

    if ((op & 0xC7) == 0x05) {
        int index = (op >> 3) & 7;
        value = ReadReg8(state, index);
        value = Dec8(state, value);
        WriteReg8(state, index, value);
        state->pc += 1;
        AddCycles(stats, index == 6 ? 11 : 4);
        return Z80_STEP_OK;
    }

    if ((op & 0xC0) == 0x40 && op != 0x76) {
        int dst = (op >> 3) & 7;
        int src = op & 7;
        value = ReadReg8(state, src);
        WriteReg8(state, dst, value);
        state->pc += 1;
        AddCycles(stats, (dst == 6 || src == 6) ? 7 : 4);
        return Z80_STEP_OK;
    }

    if ((op & 0xF8) == 0x80) {
        value = ReadReg8(state, op & 7);
        state->a = Add8(state, state->a, value, 0);
        state->pc += 1;
        AddCycles(stats, (op & 7) == 6 ? 7 : 4);
        return Z80_STEP_OK;
    }

    if ((op & 0xF8) == 0x90) {
        value = ReadReg8(state, op & 7);
        state->a = Sub8(state, state->a, value, 0, 1);
        state->pc += 1;
        AddCycles(stats, (op & 7) == 6 ? 7 : 4);
        return Z80_STEP_OK;
    }

    if ((op & 0xF8) == 0xA8) {
        value = ReadReg8(state, op & 7);
        state->a = LogicXor(state, state->a, value);
        state->pc += 1;
        AddCycles(stats, (op & 7) == 6 ? 7 : 4);
        return Z80_STEP_OK;
    }

    if ((op & 0xF8) == 0xB0) {
        value = ReadReg8(state, op & 7);
        state->a = LogicOr(state, state->a, value);
        state->pc += 1;
        AddCycles(stats, (op & 7) == 6 ? 7 : 4);
        return Z80_STEP_OK;
    }

    if ((op & 0xF8) == 0xB8) {
        value = ReadReg8(state, op & 7);
        (void)Sub8(state, state->a, value, 0, 0);
        state->pc += 1;
        AddCycles(stats, (op & 7) == 6 ? 7 : 4);
        return Z80_STEP_OK;
    }

    SetError(state, op);
    return Z80_STEP_UNIMPLEMENTED;
}
