#include "z80_cpu.h"

#include <stdio.h>

typedef enum IndexMode {
    INDEX_NONE = 0,
    INDEX_IX = 1,
    INDEX_IY = 2,
} IndexMode;

static UINT16 Read16(const UINT8 *mem, int pc) {
    return (UINT16)(mem[(pc + 1) & 0xFFFF] | (mem[(pc + 2) & 0xFFFF] << 8));
}

static UINT16 RelativeTarget(int pc, UINT8 disp, int len) {
    return (UINT16)((pc + len + (int8_t)disp) & 0xFFFF);
}

static const char *BaseRegName(int reg) {
    static const char *kNames[8] = {"B", "C", "D", "E", "H", "L", "(HL)", "A"};
    return kNames[reg & 7];
}

static const char *IndexPairName(IndexMode index) {
    return index == INDEX_IX ? "IX" : "IY";
}

static const char *IndexRegName(int reg, IndexMode index) {
    if (index == INDEX_NONE) {
        return BaseRegName(reg);
    }
    switch (reg & 7) {
    case 4:
        return index == INDEX_IX ? "IXH" : "IYH";
    case 5:
        return index == INDEX_IX ? "IXL" : "IYL";
    default:
        return BaseRegName(reg);
    }
}

static void FormatIndexedRef(char *out, size_t out_len, IndexMode index, UINT8 disp) {
    int8_t signed_disp = (int8_t)disp;
    if (signed_disp < 0) {
        snprintf(out, out_len, "(%s-%d)", IndexPairName(index), -signed_disp);
    } else {
        snprintf(out, out_len, "(%s+%d)", IndexPairName(index), signed_disp);
    }
}

static int DisassembleCB(const UINT8 *mem, int pc, char *out, size_t out_len, IndexMode index, int indexed_cb) {
    static const char *kMnemonics[8] = {"RLC", "RRC", "RL", "RR", "SLA", "SRA", "SLL", "SRL"};
    UINT8 op;
    int y;
    int z;
    char memref[32];

    if (indexed_cb) {
        UINT8 disp = mem[(pc + 1) & 0xFFFF];
        op = mem[(pc + 2) & 0xFFFF];
        FormatIndexedRef(memref, sizeof(memref), index, disp);
    } else {
        op = mem[(pc + 1) & 0xFFFF];
        memref[0] = '\0';
    }

    y = (op >> 3) & 7;
    z = op & 7;

    if ((op & 0xC0) == 0x00) {
        if (indexed_cb) {
            if (z == 6) {
                snprintf(out, out_len, "%s %s", kMnemonics[y], memref);
            } else {
                snprintf(out, out_len, "%s %s,%s", kMnemonics[y], memref, BaseRegName(z));
            }
            return 3;
        }
        snprintf(out, out_len, "%s %s", kMnemonics[y], BaseRegName(z));
        return 2;
    }

    if ((op & 0xC0) == 0x40) {
        snprintf(out, out_len, "BIT %d,%s", y, indexed_cb ? memref : BaseRegName(z));
        return indexed_cb ? 3 : 2;
    }

    if ((op & 0xC0) == 0x80) {
        if (indexed_cb && z != 6) {
            snprintf(out, out_len, "RES %d,%s,%s", y, memref, BaseRegName(z));
        } else {
            snprintf(out, out_len, "RES %d,%s", y, indexed_cb ? memref : BaseRegName(z));
        }
        return indexed_cb ? 3 : 2;
    }

    if (indexed_cb && z != 6) {
        snprintf(out, out_len, "SET %d,%s,%s", y, memref, BaseRegName(z));
    } else {
        snprintf(out, out_len, "SET %d,%s", y, indexed_cb ? memref : BaseRegName(z));
    }
    return indexed_cb ? 3 : 2;
}

static int DisassembleED(const UINT8 *mem, int pc, char *out, size_t out_len) {
    UINT8 op = mem[(pc + 1) & 0xFFFF];
    switch (op) {
    case 0x40:
        snprintf(out, out_len, "IN B,(C)");
        return 2;
    case 0x41:
        snprintf(out, out_len, "OUT (C),B");
        return 2;
    case 0x42:
        snprintf(out, out_len, "SBC HL,BC");
        return 2;
    case 0x43:
        snprintf(out, out_len, "LD ($%04X),BC", Read16(mem, pc + 1));
        return 4;
    case 0x44:
    case 0x4C:
    case 0x54:
    case 0x5C:
    case 0x64:
    case 0x6C:
    case 0x74:
    case 0x7C:
        snprintf(out, out_len, "NEG");
        return 2;
    case 0x45:
    case 0x55:
    case 0x5D:
    case 0x65:
    case 0x6D:
    case 0x75:
        snprintf(out, out_len, "RETN");
        return 2;
    case 0x4D:
        snprintf(out, out_len, "RETI");
        return 2;
    case 0x46:
    case 0x4E:
    case 0x66:
    case 0x6E:
        snprintf(out, out_len, "IM 0");
        return 2;
    case 0x47:
        snprintf(out, out_len, "LD I,A");
        return 2;
    case 0x48:
        snprintf(out, out_len, "IN C,(C)");
        return 2;
    case 0x49:
        snprintf(out, out_len, "OUT (C),C");
        return 2;
    case 0x4A:
        snprintf(out, out_len, "ADC HL,BC");
        return 2;
    case 0x4B:
        snprintf(out, out_len, "LD BC,($%04X)", Read16(mem, pc + 1));
        return 4;
    case 0x4F:
        snprintf(out, out_len, "LD R,A");
        return 2;
    case 0x50:
        snprintf(out, out_len, "IN D,(C)");
        return 2;
    case 0x51:
        snprintf(out, out_len, "OUT (C),D");
        return 2;
    case 0x52:
        snprintf(out, out_len, "SBC HL,DE");
        return 2;
    case 0x53:
        snprintf(out, out_len, "LD ($%04X),DE", Read16(mem, pc + 1));
        return 4;
    case 0x56:
    case 0x76:
        snprintf(out, out_len, "IM 1");
        return 2;
    case 0x57:
        snprintf(out, out_len, "LD A,I");
        return 2;
    case 0x58:
        snprintf(out, out_len, "IN E,(C)");
        return 2;
    case 0x59:
        snprintf(out, out_len, "OUT (C),E");
        return 2;
    case 0x5A:
        snprintf(out, out_len, "ADC HL,DE");
        return 2;
    case 0x5B:
        snprintf(out, out_len, "LD DE,($%04X)", Read16(mem, pc + 1));
        return 4;
    case 0x5E:
    case 0x7E:
        snprintf(out, out_len, "IM 2");
        return 2;
    case 0x5F:
        snprintf(out, out_len, "LD A,R");
        return 2;
    case 0x60:
        snprintf(out, out_len, "IN H,(C)");
        return 2;
    case 0x61:
        snprintf(out, out_len, "OUT (C),H");
        return 2;
    case 0x62:
        snprintf(out, out_len, "SBC HL,HL");
        return 2;
    case 0x63:
        snprintf(out, out_len, "LD ($%04X),HL", Read16(mem, pc + 1));
        return 4;
    case 0x67:
        snprintf(out, out_len, "RRD");
        return 2;
    case 0x68:
        snprintf(out, out_len, "IN L,(C)");
        return 2;
    case 0x69:
        snprintf(out, out_len, "OUT (C),L");
        return 2;
    case 0x6A:
        snprintf(out, out_len, "ADC HL,HL");
        return 2;
    case 0x6B:
        snprintf(out, out_len, "LD HL,($%04X)", Read16(mem, pc + 1));
        return 4;
    case 0x6F:
        snprintf(out, out_len, "RLD");
        return 2;
    case 0x70:
        snprintf(out, out_len, "IN (C)");
        return 2;
    case 0x71:
        snprintf(out, out_len, "OUT (C),0");
        return 2;
    case 0x72:
        snprintf(out, out_len, "SBC HL,SP");
        return 2;
    case 0x73:
        snprintf(out, out_len, "LD ($%04X),SP", Read16(mem, pc + 1));
        return 4;
    case 0x78:
        snprintf(out, out_len, "IN A,(C)");
        return 2;
    case 0x79:
        snprintf(out, out_len, "OUT (C),A");
        return 2;
    case 0x7A:
        snprintf(out, out_len, "ADC HL,SP");
        return 2;
    case 0x7B:
        snprintf(out, out_len, "LD SP,($%04X)", Read16(mem, pc + 1));
        return 4;
    case 0xA0:
        snprintf(out, out_len, "LDI");
        return 2;
    case 0xA1:
        snprintf(out, out_len, "CPI");
        return 2;
    case 0xA2:
        snprintf(out, out_len, "INI");
        return 2;
    case 0xA3:
        snprintf(out, out_len, "OUTI");
        return 2;
    case 0xA8:
        snprintf(out, out_len, "LDD");
        return 2;
    case 0xA9:
        snprintf(out, out_len, "CPD");
        return 2;
    case 0xAA:
        snprintf(out, out_len, "IND");
        return 2;
    case 0xAB:
        snprintf(out, out_len, "OUTD");
        return 2;
    case 0xB0:
        snprintf(out, out_len, "LDIR");
        return 2;
    case 0xB1:
        snprintf(out, out_len, "CPIR");
        return 2;
    case 0xB2:
        snprintf(out, out_len, "INIR");
        return 2;
    case 0xB3:
        snprintf(out, out_len, "OTIR");
        return 2;
    case 0xB8:
        snprintf(out, out_len, "LDDR");
        return 2;
    case 0xB9:
        snprintf(out, out_len, "CPDR");
        return 2;
    case 0xBA:
        snprintf(out, out_len, "INDR");
        return 2;
    case 0xBB:
        snprintf(out, out_len, "OTDR");
        return 2;
    default:
        snprintf(out, out_len, "NOP");
        return 2;
    }
}

static int DisassembleBase(const UINT8 *mem, int pc, char *out, size_t out_len, IndexMode index) {
    UINT8 op = mem[pc & 0xFFFF];
    char memref[32];
    int indexed_mem = index != INDEX_NONE;

    switch (op) {
    case 0x00:
        snprintf(out, out_len, "NOP");
        return 1;
    case 0x01:
        snprintf(out, out_len, "LD BC,$%04X", Read16(mem, pc));
        return 3;
    case 0x02:
        snprintf(out, out_len, "LD (BC),A");
        return 1;
    case 0x03:
        snprintf(out, out_len, "INC BC");
        return 1;
    case 0x07:
        snprintf(out, out_len, "RLCA");
        return 1;
    case 0x08:
        snprintf(out, out_len, "EX AF,AF'");
        return 1;
    case 0x09:
        snprintf(out, out_len, "ADD %s,BC", indexed_mem ? IndexPairName(index) : "HL");
        return 1;
    case 0x0A:
        snprintf(out, out_len, "LD A,(BC)");
        return 1;
    case 0x0B:
        snprintf(out, out_len, "DEC BC");
        return 1;
    case 0x0F:
        snprintf(out, out_len, "RRCA");
        return 1;
    case 0x10:
        snprintf(out, out_len, "DJNZ $%04X", RelativeTarget(pc, mem[(pc + 1) & 0xFFFF], 2));
        return 2;
    case 0x11:
        snprintf(out, out_len, "LD DE,$%04X", Read16(mem, pc));
        return 3;
    case 0x12:
        snprintf(out, out_len, "LD (DE),A");
        return 1;
    case 0x13:
        snprintf(out, out_len, "INC DE");
        return 1;
    case 0x17:
        snprintf(out, out_len, "RLA");
        return 1;
    case 0x18:
        snprintf(out, out_len, "JR $%04X", RelativeTarget(pc, mem[(pc + 1) & 0xFFFF], 2));
        return 2;
    case 0x19:
        snprintf(out, out_len, "ADD %s,DE", indexed_mem ? IndexPairName(index) : "HL");
        return 1;
    case 0x1A:
        snprintf(out, out_len, "LD A,(DE)");
        return 1;
    case 0x1B:
        snprintf(out, out_len, "DEC DE");
        return 1;
    case 0x1F:
        snprintf(out, out_len, "RRA");
        return 1;
    case 0x20:
    case 0x28:
    case 0x30:
    case 0x38:
        snprintf(out, out_len, "JR %s,$%04X",
                 (op == 0x20) ? "NZ" : (op == 0x28) ? "Z" : (op == 0x30) ? "NC" : "C",
                 RelativeTarget(pc, mem[(pc + 1) & 0xFFFF], 2));
        return 2;
    case 0x21:
        snprintf(out, out_len, "LD %s,$%04X", indexed_mem ? IndexPairName(index) : "HL", Read16(mem, pc));
        return 3;
    case 0x22:
        snprintf(out, out_len, "LD ($%04X),%s", Read16(mem, pc), indexed_mem ? IndexPairName(index) : "HL");
        return 3;
    case 0x23:
        snprintf(out, out_len, "INC %s", indexed_mem ? IndexPairName(index) : "HL");
        return 1;
    case 0x27:
        snprintf(out, out_len, "DAA");
        return 1;
    case 0x29:
        snprintf(out, out_len, "ADD %s,%s", indexed_mem ? IndexPairName(index) : "HL",
                 indexed_mem ? IndexPairName(index) : "HL");
        return 1;
    case 0x2A:
        snprintf(out, out_len, "LD %s,($%04X)", indexed_mem ? IndexPairName(index) : "HL", Read16(mem, pc));
        return 3;
    case 0x2B:
        snprintf(out, out_len, "DEC %s", indexed_mem ? IndexPairName(index) : "HL");
        return 1;
    case 0x2F:
        snprintf(out, out_len, "CPL");
        return 1;
    case 0x31:
        snprintf(out, out_len, "LD SP,$%04X", Read16(mem, pc));
        return 3;
    case 0x32:
        snprintf(out, out_len, "LD ($%04X),A", Read16(mem, pc));
        return 3;
    case 0x33:
        snprintf(out, out_len, "INC SP");
        return 1;
    case 0x37:
        snprintf(out, out_len, "SCF");
        return 1;
    case 0x39:
        snprintf(out, out_len, "ADD %s,SP", indexed_mem ? IndexPairName(index) : "HL");
        return 1;
    case 0x3A:
        snprintf(out, out_len, "LD A,($%04X)", Read16(mem, pc));
        return 3;
    case 0x3B:
        snprintf(out, out_len, "DEC SP");
        return 1;
    case 0x3F:
        snprintf(out, out_len, "CCF");
        return 1;
    case 0x76:
        snprintf(out, out_len, "HALT");
        return 1;
    case 0xC0:
        snprintf(out, out_len, "RET NZ");
        return 1;
    case 0xC1:
        snprintf(out, out_len, "POP BC");
        return 1;
    case 0xC2:
        snprintf(out, out_len, "JP NZ,$%04X", Read16(mem, pc));
        return 3;
    case 0xC3:
        snprintf(out, out_len, "JP $%04X", Read16(mem, pc));
        return 3;
    case 0xC4:
        snprintf(out, out_len, "CALL NZ,$%04X", Read16(mem, pc));
        return 3;
    case 0xC5:
        snprintf(out, out_len, "PUSH BC");
        return 1;
    case 0xC6:
        snprintf(out, out_len, "ADD A,$%02X", mem[(pc + 1) & 0xFFFF]);
        return 2;
    case 0xC7:
    case 0xCF:
    case 0xD7:
    case 0xDF:
    case 0xE7:
    case 0xEF:
    case 0xF7:
    case 0xFF:
        snprintf(out, out_len, "RST $%02X", op & 0x38);
        return 1;
    case 0xC8:
        snprintf(out, out_len, "RET Z");
        return 1;
    case 0xC9:
        snprintf(out, out_len, "RET");
        return 1;
    case 0xCA:
        snprintf(out, out_len, "JP Z,$%04X", Read16(mem, pc));
        return 3;
    case 0xCC:
        snprintf(out, out_len, "CALL Z,$%04X", Read16(mem, pc));
        return 3;
    case 0xCD:
        snprintf(out, out_len, "CALL $%04X", Read16(mem, pc));
        return 3;
    case 0xCE:
        snprintf(out, out_len, "ADC A,$%02X", mem[(pc + 1) & 0xFFFF]);
        return 2;
    case 0xD0:
        snprintf(out, out_len, "RET NC");
        return 1;
    case 0xD1:
        snprintf(out, out_len, "POP DE");
        return 1;
    case 0xD2:
        snprintf(out, out_len, "JP NC,$%04X", Read16(mem, pc));
        return 3;
    case 0xD3:
        snprintf(out, out_len, "OUT ($%02X),A", mem[(pc + 1) & 0xFFFF]);
        return 2;
    case 0xD4:
        snprintf(out, out_len, "CALL NC,$%04X", Read16(mem, pc));
        return 3;
    case 0xD5:
        snprintf(out, out_len, "PUSH DE");
        return 1;
    case 0xD6:
        snprintf(out, out_len, "SUB $%02X", mem[(pc + 1) & 0xFFFF]);
        return 2;
    case 0xD8:
        snprintf(out, out_len, "RET C");
        return 1;
    case 0xD9:
        snprintf(out, out_len, "EXX");
        return 1;
    case 0xDA:
        snprintf(out, out_len, "JP C,$%04X", Read16(mem, pc));
        return 3;
    case 0xDB:
        snprintf(out, out_len, "IN A,($%02X)", mem[(pc + 1) & 0xFFFF]);
        return 2;
    case 0xDC:
        snprintf(out, out_len, "CALL C,$%04X", Read16(mem, pc));
        return 3;
    case 0xDE:
        snprintf(out, out_len, "SBC A,$%02X", mem[(pc + 1) & 0xFFFF]);
        return 2;
    case 0xE0:
        snprintf(out, out_len, "RET PO");
        return 1;
    case 0xE1:
        snprintf(out, out_len, "POP %s", indexed_mem ? IndexPairName(index) : "HL");
        return 1;
    case 0xE2:
        snprintf(out, out_len, "JP PO,$%04X", Read16(mem, pc));
        return 3;
    case 0xE3:
        snprintf(out, out_len, "EX (SP),%s", indexed_mem ? IndexPairName(index) : "HL");
        return 1;
    case 0xE4:
        snprintf(out, out_len, "CALL PO,$%04X", Read16(mem, pc));
        return 3;
    case 0xE5:
        snprintf(out, out_len, "PUSH %s", indexed_mem ? IndexPairName(index) : "HL");
        return 1;
    case 0xE6:
        snprintf(out, out_len, "AND $%02X", mem[(pc + 1) & 0xFFFF]);
        return 2;
    case 0xE8:
        snprintf(out, out_len, "RET PE");
        return 1;
    case 0xE9:
        snprintf(out, out_len, "JP (%s)", indexed_mem ? IndexPairName(index) : "HL");
        return 1;
    case 0xEA:
        snprintf(out, out_len, "JP PE,$%04X", Read16(mem, pc));
        return 3;
    case 0xEB:
        snprintf(out, out_len, "EX DE,HL");
        return 1;
    case 0xEC:
        snprintf(out, out_len, "CALL PE,$%04X", Read16(mem, pc));
        return 3;
    case 0xEE:
        snprintf(out, out_len, "XOR $%02X", mem[(pc + 1) & 0xFFFF]);
        return 2;
    case 0xF0:
        snprintf(out, out_len, "RET P");
        return 1;
    case 0xF1:
        snprintf(out, out_len, "POP AF");
        return 1;
    case 0xF2:
        snprintf(out, out_len, "JP P,$%04X", Read16(mem, pc));
        return 3;
    case 0xF3:
        snprintf(out, out_len, "DI");
        return 1;
    case 0xF4:
        snprintf(out, out_len, "CALL P,$%04X", Read16(mem, pc));
        return 3;
    case 0xF5:
        snprintf(out, out_len, "PUSH AF");
        return 1;
    case 0xF6:
        snprintf(out, out_len, "OR $%02X", mem[(pc + 1) & 0xFFFF]);
        return 2;
    case 0xF8:
        snprintf(out, out_len, "RET M");
        return 1;
    case 0xF9:
        snprintf(out, out_len, "LD SP,%s", indexed_mem ? IndexPairName(index) : "HL");
        return 1;
    case 0xFA:
        snprintf(out, out_len, "JP M,$%04X", Read16(mem, pc));
        return 3;
    case 0xFB:
        snprintf(out, out_len, "EI");
        return 1;
    case 0xFC:
        snprintf(out, out_len, "CALL M,$%04X", Read16(mem, pc));
        return 3;
    case 0xFE:
        snprintf(out, out_len, "CP $%02X", mem[(pc + 1) & 0xFFFF]);
        return 2;
    default:
        break;
    }

    if ((op & 0xC7) == 0x04) {
        int reg = (op >> 3) & 7;
        if (reg == 6) {
            if (indexed_mem) {
                FormatIndexedRef(memref, sizeof(memref), index, mem[(pc + 1) & 0xFFFF]);
                snprintf(out, out_len, "INC %s", memref);
                return 2;
            }
            snprintf(out, out_len, "INC (HL)");
            return 1;
        }
        snprintf(out, out_len, "INC %s", IndexRegName(reg, index));
        return 1;
    }

    if ((op & 0xC7) == 0x05) {
        int reg = (op >> 3) & 7;
        if (reg == 6) {
            if (indexed_mem) {
                FormatIndexedRef(memref, sizeof(memref), index, mem[(pc + 1) & 0xFFFF]);
                snprintf(out, out_len, "DEC %s", memref);
                return 2;
            }
            snprintf(out, out_len, "DEC (HL)");
            return 1;
        }
        snprintf(out, out_len, "DEC %s", IndexRegName(reg, index));
        return 1;
    }

    if ((op & 0xC7) == 0x06) {
        int reg = (op >> 3) & 7;
        if (reg == 6) {
            if (indexed_mem) {
                FormatIndexedRef(memref, sizeof(memref), index, mem[(pc + 1) & 0xFFFF]);
                snprintf(out, out_len, "LD %s,$%02X", memref, mem[(pc + 2) & 0xFFFF]);
                return 3;
            }
            snprintf(out, out_len, "LD (HL),$%02X", mem[(pc + 1) & 0xFFFF]);
            return 2;
        }
        snprintf(out, out_len, "LD %s,$%02X", IndexRegName(reg, index), mem[(pc + 1) & 0xFFFF]);
        return 2;
    }

    if ((op & 0xC0) == 0x40 && op != 0x76) {
        int dst = (op >> 3) & 7;
        int src = op & 7;
        if (indexed_mem && (dst == 6 || src == 6)) {
            FormatIndexedRef(memref, sizeof(memref), index, mem[(pc + 1) & 0xFFFF]);
            snprintf(out, out_len, "LD %s,%s", (dst == 6) ? memref : BaseRegName(dst),
                     (src == 6) ? memref : IndexRegName(src, index));
            return 2;
        }
        snprintf(out, out_len, "LD %s,%s", IndexRegName(dst, index), IndexRegName(src, index));
        return 1;
    }

    if ((op & 0xF8) == 0x80 || (op & 0xF8) == 0x88 || (op & 0xF8) == 0x90 || (op & 0xF8) == 0x98 ||
        (op & 0xF8) == 0xA0 || (op & 0xF8) == 0xA8 || (op & 0xF8) == 0xB0 || (op & 0xF8) == 0xB8) {
        static const char *kMnemonics[8] = {"ADD A", "ADC A", "SUB", "SBC A", "AND", "XOR", "OR", "CP"};
        int src = op & 7;
        if (indexed_mem && src == 6) {
            FormatIndexedRef(memref, sizeof(memref), index, mem[(pc + 1) & 0xFFFF]);
            snprintf(out, out_len, "%s,%s", kMnemonics[(op >> 3) & 7], memref);
            return 2;
        }
        snprintf(out, out_len, "%s,%s", kMnemonics[(op >> 3) & 7], IndexRegName(src, index));
        return 1;
    }

    snprintf(out, out_len, "DB $%02X", op);
    return 1;
}

int DisassembleZ80Op(const UINT8 *codebuffer, int pc, char *out, size_t out_len) {
    UINT8 op;
    IndexMode index = INDEX_NONE;
    int cursor = pc & 0xFFFF;

    if (!out || out_len == 0) {
        return 1;
    }

    op = codebuffer[cursor];
    while (op == 0xDD || op == 0xFD) {
        index = (op == 0xDD) ? INDEX_IX : INDEX_IY;
        cursor = (cursor + 1) & 0xFFFF;
        op = codebuffer[cursor];
    }

    if (op == 0xCB) {
        return DisassembleCB(codebuffer, cursor, out, out_len, index, index != INDEX_NONE) + (cursor - pc);
    }
    if (op == 0xED) {
        return DisassembleED(codebuffer, cursor, out, out_len) + (cursor - pc);
    }
    return DisassembleBase(codebuffer, cursor, out, out_len, index) + (cursor - pc);
}
