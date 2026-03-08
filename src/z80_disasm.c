#include "z80_cpu.h"

#include <stdio.h>
#include <string.h>

static UINT16 Read16(const UINT8 *mem, int pc) {
    return (UINT16)(mem[(pc + 1) & 0xFFFF] | (mem[(pc + 2) & 0xFFFF] << 8));
}

static UINT16 RelativeTarget(int pc, UINT8 disp) {
    return (UINT16)((pc + 2 + (int8_t)disp) & 0xFFFF);
}

static const char *RegName(int index) {
    static const char *kNames[8] = {"B", "C", "D", "E", "H", "L", "(HL)", "A"};
    return kNames[index & 7];
}

int DisassembleZ80Op(const UINT8 *codebuffer, int pc, char *out, size_t out_len) {
    UINT8 op = codebuffer[pc & 0xFFFF];

    if (!out || out_len == 0) {
        return 1;
    }

    switch (op) {
    case 0x00:
        snprintf(out, out_len, "NOP");
        return 1;
    case 0x01:
        snprintf(out, out_len, "LD BC,$%04X", Read16(codebuffer, pc));
        return 3;
    case 0x11:
        snprintf(out, out_len, "LD DE,$%04X", Read16(codebuffer, pc));
        return 3;
    case 0x21:
        snprintf(out, out_len, "LD HL,$%04X", Read16(codebuffer, pc));
        return 3;
    case 0x22:
        snprintf(out, out_len, "LD ($%04X),HL", Read16(codebuffer, pc));
        return 3;
    case 0x23:
        snprintf(out, out_len, "INC HL");
        return 1;
    case 0x2A:
        snprintf(out, out_len, "LD HL,($%04X)", Read16(codebuffer, pc));
        return 3;
    case 0x31:
        snprintf(out, out_len, "LD SP,$%04X", Read16(codebuffer, pc));
        return 3;
    case 0x32:
        snprintf(out, out_len, "LD ($%04X),A", Read16(codebuffer, pc));
        return 3;
    case 0x3A:
        snprintf(out, out_len, "LD A,($%04X)", Read16(codebuffer, pc));
        return 3;
    case 0x76:
        snprintf(out, out_len, "HALT");
        return 1;
    case 0xC3:
        snprintf(out, out_len, "JP $%04X", Read16(codebuffer, pc));
        return 3;
    case 0xC6:
        snprintf(out, out_len, "ADD A,$%02X", codebuffer[(pc + 1) & 0xFFFF]);
        return 2;
    case 0xC9:
        snprintf(out, out_len, "RET");
        return 1;
    case 0xCD:
        snprintf(out, out_len, "CALL $%04X", Read16(codebuffer, pc));
        return 3;
    case 0xD6:
        snprintf(out, out_len, "SUB $%02X", codebuffer[(pc + 1) & 0xFFFF]);
        return 2;
    case 0xEE:
        snprintf(out, out_len, "XOR $%02X", codebuffer[(pc + 1) & 0xFFFF]);
        return 2;
    case 0xF6:
        snprintf(out, out_len, "OR $%02X", codebuffer[(pc + 1) & 0xFFFF]);
        return 2;
    case 0xFE:
        snprintf(out, out_len, "CP $%02X", codebuffer[(pc + 1) & 0xFFFF]);
        return 2;
    case 0x18:
        snprintf(out, out_len, "JR $%04X", RelativeTarget(pc, codebuffer[(pc + 1) & 0xFFFF]));
        return 2;
    case 0x20:
        snprintf(out, out_len, "JR NZ,$%04X", RelativeTarget(pc, codebuffer[(pc + 1) & 0xFFFF]));
        return 2;
    case 0x28:
        snprintf(out, out_len, "JR Z,$%04X", RelativeTarget(pc, codebuffer[(pc + 1) & 0xFFFF]));
        return 2;
    case 0x30:
        snprintf(out, out_len, "JR NC,$%04X", RelativeTarget(pc, codebuffer[(pc + 1) & 0xFFFF]));
        return 2;
    case 0x38:
        snprintf(out, out_len, "JR C,$%04X", RelativeTarget(pc, codebuffer[(pc + 1) & 0xFFFF]));
        return 2;
    default:
        break;
    }

    if ((op & 0xC7) == 0x06) {
        snprintf(out, out_len, "LD %s,$%02X", RegName((op >> 3) & 7), codebuffer[(pc + 1) & 0xFFFF]);
        return 2;
    }

    if ((op & 0xC7) == 0x04) {
        snprintf(out, out_len, "INC %s", RegName((op >> 3) & 7));
        return 1;
    }

    if ((op & 0xC7) == 0x05) {
        snprintf(out, out_len, "DEC %s", RegName((op >> 3) & 7));
        return 1;
    }

    if ((op & 0xC0) == 0x40 && op != 0x76) {
        snprintf(out, out_len, "LD %s,%s", RegName((op >> 3) & 7), RegName(op & 7));
        return 1;
    }

    if ((op & 0xF8) == 0x80) {
        snprintf(out, out_len, "ADD A,%s", RegName(op & 7));
        return 1;
    }

    if ((op & 0xF8) == 0x90) {
        snprintf(out, out_len, "SUB %s", RegName(op & 7));
        return 1;
    }

    if ((op & 0xF8) == 0xA8) {
        snprintf(out, out_len, "XOR %s", RegName(op & 7));
        return 1;
    }

    if ((op & 0xF8) == 0xB0) {
        snprintf(out, out_len, "OR %s", RegName(op & 7));
        return 1;
    }

    if ((op & 0xF8) == 0xB8) {
        snprintf(out, out_len, "CP %s", RegName(op & 7));
        return 1;
    }

    snprintf(out, out_len, "DB $%02X", op);
    return 1;
}
