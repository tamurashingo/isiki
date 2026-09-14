/**
 * x86-64 逆アセンブラ(JIT生成コード用)のデコーダ本体。
 *
 * 設計の背景と対応範囲は documents/disasm-backend-decision.md、
 * メタデータの調査結果は documents/jit-metadata-investigation.md を参照。
 *
 * このファイルはランタイム(runtime.c)に一切依存せず、バイト列を入れて文字列を
 * 得るだけの純粋な変換である(stream.c と stream_lisp.c の関係と同じ分け方)。
 * Lispからの入口は src/c/disasm_lisp.c にある。
 * test/c/disasm_test.c がここを単体で検証する。
 */

#include "disasm.h"

/* ============================== 文字列の組み立て ============================== */

/* libcが無いので、境界チェック付きの最小限のビルダーを持つ。
 * オーバーフローは切り捨てで、NUL終端だけは必ず維持する。 */
typedef struct {
    char *buf;
    UINT64 size;
    UINT64 len;
} d_sb_t;

static void sb_init(d_sb_t *sb, char *buf, UINT64 size) {
    sb->buf = buf;
    sb->size = size;
    sb->len = 0;
    if (size > 0) {
        buf[0] = 0;
    }
}

static void sb_char(d_sb_t *sb, char c) {
    if (sb->size == 0 || sb->len + 1 >= sb->size) {
        return;
    }
    sb->buf[sb->len++] = c;
    sb->buf[sb->len] = 0;
}

static void sb_str(d_sb_t *sb, const char *s) {
    while (*s) {
        sb_char(sb, *s++);
    }
}

/** 0埋め無しの16進(例: 0x1f)。value==0は"0x0" */
static void sb_hex(d_sb_t *sb, UINT64 value) {
    char tmp[17];
    int n = 0;
    sb_str(sb, "0x");
    if (value == 0) {
        sb_char(sb, '0');
        return;
    }
    while (value != 0 && n < 16) {
        UINT8 digit = (UINT8)(value & 0xF);
        tmp[n++] = (char)(digit < 10 ? '0' + digit : 'a' + (digit - 10));
        value >>= 4;
    }
    while (n > 0) {
        sb_char(sb, tmp[--n]);
    }
}

/**
 * 最小桁数を指定した16進(先頭0埋め、prefix無し)。valueがdigits桁に収まらない場合は
 * 桁を増やして**必ず全体を出す**(切り詰めて嘘の値を見せない)。
 */
static void sb_hex_pad(d_sb_t *sb, UINT64 value, int digits) {
    int needed = 1;
    UINT64 v = value;
    while (v > 0xF) {
        v >>= 4;
        needed++;
    }
    int i = needed > digits ? needed : digits;
    while (i > 0) {
        i--;
        UINT8 nibble = (UINT8)((value >> (i * 4)) & 0xF);
        sb_char(sb, (char)(nibble < 10 ? '0' + nibble : 'a' + (nibble - 10)));
    }
}

/** 符号付きの16進(例: -0x8)。dispや符号拡張imm8の表示に使う */
static void sb_shex(d_sb_t *sb, INT64 value) {
    if (value < 0) {
        sb_char(sb, '-');
        /* INT64_MINでも破綻しないよう、符号反転はUINT64側で行う */
        sb_hex(sb, (UINT64)(-(value + 1)) + 1);
    } else {
        sb_hex(sb, (UINT64)value);
    }
}

/** dispを "+0x8" / "-0x8" の形で足す(0なら何も出さない) */
static void sb_disp(d_sb_t *sb, INT64 value) {
    if (value == 0) {
        return;
    }
    if (value < 0) {
        sb_char(sb, '-');
        sb_hex(sb, (UINT64)(-(value + 1)) + 1);
    } else {
        sb_char(sb, '+');
        sb_hex(sb, (UINT64)value);
    }
}

static void d_strcpy(char *dst, UINT64 dst_size, const char *src) {
    d_sb_t sb;
    sb_init(&sb, dst, dst_size);
    sb_str(&sb, src);
}

/* ============================== レジスタ名 ============================== */

static const char *const g_reg64[16] = {
    "rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
    "r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15"
};

static const char *const g_reg32[16] = {
    "eax",  "ecx",  "edx",  "ebx",  "esp",  "ebp",  "esi",  "edi",
    "r8d",  "r9d",  "r10d", "r11d", "r12d", "r13d", "r14d", "r15d"
};

/** @param wide REX.Wが立っていれば1(64bitレジスタ名)、そうでなければ0(32bit) */
static const char *d_regname(UINT8 reg, int wide) {
    return wide ? g_reg64[reg & 15] : g_reg32[reg & 15];
}

/* ============================== 命令表 ============================== */

/** 0x83/0x81のグループ1(/digitでニモニックが決まる) */
static const char *const g_group1[8] = { "add", "or", "adc", "sbb", "and", "sub", "xor", "cmp" };

/** 0x70-0x7F(rel8)および0x0F 0x80-0x8F(rel32)の条件付きジャンプ */
static const char *const g_jcc[16] = {
    "jo", "jno", "jb", "jae", "je", "jne", "jbe", "ja",
    "js", "jns", "jp", "jnp", "jl", "jge", "jle", "jg"
};

/** opcodeの下位3bitを落とした形でニモニックが決まる算術命令(r/m,r と r,r/m の両方向) */
static const char *d_alu_name(UINT8 op) {
    switch (op & 0x38) {
        case 0x00: return "add";
        case 0x08: return "or";
        case 0x10: return "adc";
        case 0x18: return "sbb";
        case 0x20: return "and";
        case 0x28: return "sub";
        case 0x30: return "xor";
        default:   return "cmp";
    }
}

/* ============================== ModRM / SIB ============================== */

/* INT8 が types.h に無いので、UINT8 からの符号拡張をマクロで書く */
#define INT8_SIGN(b) ((INT64)(INT32)(signed char)(b))

/** 1命令のデコード中に持ち回る状態 */
typedef struct {
    const UINT8 *code;
    UINT64 code_len;
    UINT64 pos;      /* 次に読むバイトの位置 */
    int truncated;   /* code_lenを踏み越えて読もうとしたら1 */
} d_cursor_t;

static UINT8 d_u8(d_cursor_t *c) {
    if (c->pos >= c->code_len) {
        c->truncated = 1;
        return 0;
    }
    return c->code[c->pos++];
}

static UINT32 d_u32(d_cursor_t *c) {
    UINT32 v = 0;
    for (int i = 0; i < 4; i++) {
        v |= ((UINT32)d_u8(c)) << (i * 8);
    }
    return v;
}

static UINT64 d_u64(d_cursor_t *c) {
    UINT64 v = 0;
    for (int i = 0; i < 8; i++) {
        v |= ((UINT64)d_u8(c)) << (i * 8);
    }
    return v;
}


/**
 * ModRMバイト(と必要ならSIB・disp)を読み、r/mオペランドの表記を rm_text へ書く。
 * @param wide REX.Wが立っていれば1(レジスタ名を64bitで出す)
 * @param out_reg ModRM.reg(REX.Rを含む4bit)の書き込み先
 * @return なし(読み進みは cursor に反映される)
 */
static void d_decode_modrm(d_cursor_t *c, UINT8 rex, int wide,
                           char *rm_text, UINT64 rm_size, UINT8 *out_reg) {
    d_sb_t sb;
    sb_init(&sb, rm_text, rm_size);

    UINT8 modrm = d_u8(c);
    UINT8 mod = (UINT8)(modrm >> 6);
    UINT8 reg = (UINT8)(((modrm >> 3) & 7) | (((rex >> 2) & 1) << 3));
    UINT8 rm = (UINT8)(modrm & 7);
    *out_reg = reg;

    if (mod == 3) {
        sb_str(&sb, d_regname((UINT8)(rm | (((rex >> 0) & 1) << 3)), wide));
        return;
    }

    /* メモリオペランド。アドレス計算は常に64bitレジスタで行う(アドレスサイズ
       プレフィックス0x67は未対応 = za.cは出力しない) */
    const char *base_name = 0;
    const char *index_name = 0;
    UINT8 scale = 0;
    INT64 disp = 0;
    int rip_relative = 0;

    if (rm == 4) {
        UINT8 sib = d_u8(c);
        scale = (UINT8)(1 << (sib >> 6));
        UINT8 index = (UINT8)(((sib >> 3) & 7) | (((rex >> 1) & 1) << 3));
        UINT8 base = (UINT8)((sib & 7) | ((rex & 1) << 3));
        /* index==4(rsp)だけは「indexなし」を意味する。REX.Xが立った12(r12)は有効 */
        if (index != 4) {
            index_name = g_reg64[index];
        }
        if ((sib & 7) == 5 && mod == 0) {
            disp = (INT64)(INT32)d_u32(c); /* baseなしのdisp32 */
        } else {
            base_name = g_reg64[base];
        }
    } else if (rm == 5 && mod == 0) {
        rip_relative = 1;
        disp = (INT64)(INT32)d_u32(c);
    } else {
        base_name = g_reg64[rm | ((rex & 1) << 3)];
    }

    if (mod == 1) {
        disp = INT8_SIGN(d_u8(c));
    } else if (mod == 2) {
        disp = (INT64)(INT32)d_u32(c);
    }

    sb_char(&sb, '[');
    if (rip_relative) {
        sb_str(&sb, "rip");
        sb_disp(&sb, disp);
    } else {
        int wrote = 0;
        if (base_name) {
            sb_str(&sb, base_name);
            wrote = 1;
        }
        if (index_name) {
            if (wrote) {
                sb_char(&sb, '+');
            }
            sb_str(&sb, index_name);
            sb_char(&sb, '*');
            sb_hex(&sb, scale);
            wrote = 1;
        }
        if (wrote) {
            sb_disp(&sb, disp);
        } else {
            sb_hex(&sb, (UINT64)disp);
        }
    }
    sb_char(&sb, ']');
}

/* ============================== 分岐先の表記 ============================== */

/**
 * 相対分岐の飛び先を operands へ書く。飛び先がコード範囲内ならコード先頭からの
 * オフセットを、範囲外(共有トランポリンへの末尾呼び出し等)なら相対変位を出す。
 * @param next_offset 分岐命令の次の命令のオフセット(rel加算の基準)
 */
static void d_branch_target(d_sb_t *sb, UINT64 code_len, UINT64 next_offset, INT64 rel) {
    INT64 target = (INT64)next_offset + rel;
    if (target >= 0 && (UINT64)target <= code_len) {
        sb_hex(sb, (UINT64)target);
    } else {
        /* 共有トランポリンのようにコードブロックの外へ飛ぶ場合。オフセットとして
           出すと範囲外の値になって嘘になるので、その場からの相対変位で示す */
        sb_str(sb, ".");
        sb_disp(sb, rel);
        sb_str(sb, " ; outside");
    }
}

/* ============================== 埋め込み文字列の検出 ============================== */

/**
 * offsetが za_emit_symbol_name の埋め込み文字列の先頭かどうかを判定する。
 * 判定は完全にステートレス(直前5バイトを見るだけ)。条件は
 * documents/disasm-backend-decision.md「埋め込み文字列の扱い」参照。
 * @return 文字列のバイト数(NUL込み)。文字列でなければ0
 */
static UINT64 d_embedded_string_len(const UINT8 *code, UINT64 code_len, UINT64 offset) {
    if (offset < 5) {
        return 0;
    }
    if (code[offset - 5] != 0xE9) {
        return 0; /* 直前が jmp rel32 でない */
    }
    UINT32 rel = 0;
    for (int i = 0; i < 4; i++) {
        rel |= ((UINT32)code[offset - 4 + i]) << (i * 8);
    }
    INT64 len = (INT64)(INT32)rel;
    if (len < 1 || len > OS_DISASM_MAX_DATA_BYTES) {
        return 0;
    }
    if (offset + (UINT64)len > code_len) {
        return 0;
    }
    if (code[offset + (UINT64)len - 1] != 0) {
        return 0; /* NUL終端でない */
    }
    for (INT64 i = 0; i < len - 1; i++) {
        UINT8 ch = code[offset + (UINT64)i];
        if (ch < 0x20 || ch > 0x7E) {
            return 0; /* 表示可能ASCIIでない */
        }
    }
    return (UINT64)len;
}

/* ============================== 本体 ============================== */

static void d_fill_bytes(os_disasm_insn_t *out, const UINT8 *code, UINT64 code_len, UINT64 offset) {
    for (UINT64 i = 0; i < OS_DISASM_MAX_INSN_BYTES; i++) {
        UINT64 at = offset + i;
        out->bytes[i] = (i < out->length && at < code_len) ? code[at] : 0;
    }
}

static UINT64 d_emit_bad(os_disasm_insn_t *out, const UINT8 *code, UINT64 code_len, UINT64 offset) {
    out->kind = OS_DISASM_BAD;
    out->offset = offset;
    out->length = 1;
    d_strcpy(out->mnemonic, sizeof(out->mnemonic), "(bad)");
    d_sb_t sb;
    sb_init(&sb, out->operands, sizeof(out->operands));
    sb_hex(&sb, code[offset]);
    d_fill_bytes(out, code, code_len, offset);
    return 1;
}

UINT64 os_disasm_item(const UINT8 *code, UINT64 code_len, UINT64 offset, os_disasm_insn_t *out) {
    if (code == 0 || out == 0 || offset >= code_len) {
        return 0;
    }

    /* 1. コードストリームに埋め込まれた文字列か */
    UINT64 str_len = d_embedded_string_len(code, code_len, offset);
    if (str_len > 0) {
        out->kind = OS_DISASM_DATA;
        out->offset = offset;
        out->length = str_len;
        d_strcpy(out->mnemonic, sizeof(out->mnemonic), ".asciz");
        d_sb_t sb;
        sb_init(&sb, out->operands, sizeof(out->operands));
        sb_char(&sb, '"');
        for (UINT64 i = 0; i + 1 < str_len; i++) {
            char ch = (char)code[offset + i];
            if (ch == '"' || ch == '\\') {
                sb_char(&sb, '\\');
            }
            sb_char(&sb, ch);
        }
        sb_char(&sb, '"');
        d_fill_bytes(out, code, code_len, offset);
        return str_len;
    }

    /* 2. 命令としてデコードする */
    d_cursor_t c;
    c.code = code;
    c.code_len = code_len;
    c.pos = offset;
    c.truncated = 0;

    UINT8 rex = 0;
    while (c.pos < code_len && code[c.pos] >= 0x40 && code[c.pos] <= 0x4F) {
        rex = code[c.pos++];
    }
    int wide = (rex & 0x08) ? 1 : 0;

    if (c.pos >= code_len) {
        return d_emit_bad(out, code, code_len, offset);
    }

    char mnemonic[16];
    char operands[OS_DISASM_TEXT_SIZE];
    d_sb_t ops;
    sb_init(&ops, operands, sizeof(operands));
    mnemonic[0] = 0;

    char rm_text[48];
    UINT8 reg = 0;
    UINT8 op = d_u8(&c);
    int ok = 1;

    switch (op) {
        /* ---- ALU r/m64, r64 (add/or/adc/sbb/and/sub/xor/cmp) ---- */
        case 0x01: case 0x09: case 0x11: case 0x19:
        case 0x21: case 0x29: case 0x31: case 0x39:
        /* ---- test r/m64, r64 ---- */
        case 0x85:
        /* ---- mov r/m64, r64 ---- */
        case 0x89: {
            d_decode_modrm(&c, rex, wide, rm_text, sizeof(rm_text), &reg);
            d_strcpy(mnemonic, sizeof(mnemonic), op == 0x89 ? "mov" : (op == 0x85 ? "test" : d_alu_name(op)));
            sb_str(&ops, rm_text);
            sb_str(&ops, ", ");
            sb_str(&ops, d_regname(reg, wide));
            break;
        }

        /* ---- ALU r64, r/m64 ---- */
        case 0x03: case 0x0B: case 0x13: case 0x1B:
        case 0x23: case 0x2B: case 0x33: case 0x3B:
        /* ---- mov r64, r/m64 / lea r64, m ---- */
        case 0x8B: case 0x8D: {
            d_decode_modrm(&c, rex, wide, rm_text, sizeof(rm_text), &reg);
            d_strcpy(mnemonic, sizeof(mnemonic), op == 0x8B ? "mov" : (op == 0x8D ? "lea" : d_alu_name(op)));
            sb_str(&ops, d_regname(reg, wide));
            sb_str(&ops, ", ");
            sb_str(&ops, rm_text);
            break;
        }

        /* ---- push/pop r64 ---- */
        case 0x50: case 0x51: case 0x52: case 0x53:
        case 0x54: case 0x55: case 0x56: case 0x57:
            d_strcpy(mnemonic, sizeof(mnemonic), "push");
            sb_str(&ops, g_reg64[(op - 0x50) | ((rex & 1) << 3)]);
            break;
        case 0x58: case 0x59: case 0x5A: case 0x5B:
        case 0x5C: case 0x5D: case 0x5E: case 0x5F:
            d_strcpy(mnemonic, sizeof(mnemonic), "pop");
            sb_str(&ops, g_reg64[(op - 0x58) | ((rex & 1) << 3)]);
            break;

        /* ---- group1 r/m64, imm8(符号拡張) / imm32 ---- */
        case 0x83: case 0x81: {
            UINT8 modrm_peek = (c.pos < code_len) ? code[c.pos] : 0;
            d_decode_modrm(&c, rex, wide, rm_text, sizeof(rm_text), &reg);
            d_strcpy(mnemonic, sizeof(mnemonic), g_group1[(modrm_peek >> 3) & 7]);
            sb_str(&ops, rm_text);
            sb_str(&ops, ", ");
            if (op == 0x83) {
                sb_shex(&ops, INT8_SIGN(d_u8(&c)));
            } else {
                sb_shex(&ops, (INT64)(INT32)d_u32(&c));
            }
            break;
        }

        /* ---- mov r/m64, imm32(符号拡張) ---- */
        case 0xC7: {
            d_decode_modrm(&c, rex, wide, rm_text, sizeof(rm_text), &reg);
            d_strcpy(mnemonic, sizeof(mnemonic), "mov");
            sb_str(&ops, rm_text);
            sb_str(&ops, ", ");
            sb_shex(&ops, (INT64)(INT32)d_u32(&c));
            break;
        }

        /* ---- mov r64, imm64 (movabs) / mov r32, imm32 ---- */
        case 0xB8: case 0xB9: case 0xBA: case 0xBB:
        case 0xBC: case 0xBD: case 0xBE: case 0xBF: {
            UINT8 dst = (UINT8)((op - 0xB8) | ((rex & 1) << 3));
            d_strcpy(mnemonic, sizeof(mnemonic), wide ? "movabs" : "mov");
            sb_str(&ops, d_regname(dst, wide));
            sb_str(&ops, ", ");
            sb_hex(&ops, wide ? d_u64(&c) : (UINT64)d_u32(&c));
            break;
        }

        /* ---- 制御転送 ---- */
        case 0xC3:
            d_strcpy(mnemonic, sizeof(mnemonic), "ret");
            break;
        case 0xC9:
            d_strcpy(mnemonic, sizeof(mnemonic), "leave");
            break;
        case 0x90:
            d_strcpy(mnemonic, sizeof(mnemonic), "nop");
            break;
        case 0xCC:
            d_strcpy(mnemonic, sizeof(mnemonic), "int3");
            break;
        case 0xE8: case 0xE9: {
            INT64 rel = (INT64)(INT32)d_u32(&c);
            d_strcpy(mnemonic, sizeof(mnemonic), op == 0xE8 ? "call" : "jmp");
            d_branch_target(&ops, code_len, c.pos, rel);
            break;
        }
        case 0xEB: {
            INT64 rel = INT8_SIGN(d_u8(&c));
            d_strcpy(mnemonic, sizeof(mnemonic), "jmp");
            d_branch_target(&ops, code_len, c.pos, rel);
            break;
        }
        case 0x70: case 0x71: case 0x72: case 0x73:
        case 0x74: case 0x75: case 0x76: case 0x77:
        case 0x78: case 0x79: case 0x7A: case 0x7B:
        case 0x7C: case 0x7D: case 0x7E: case 0x7F: {
            INT64 rel = INT8_SIGN(d_u8(&c));
            d_strcpy(mnemonic, sizeof(mnemonic), g_jcc[op - 0x70]);
            d_branch_target(&ops, code_len, c.pos, rel);
            break;
        }

        /* ---- group5: inc/dec/call/jmp/push r/m64 ---- */
        case 0xFF: {
            UINT8 modrm_peek = (c.pos < code_len) ? code[c.pos] : 0;
            UINT8 digit = (UINT8)((modrm_peek >> 3) & 7);
            /* call/jmp/pushは64bitがデフォルトなのでREX.Wが無くても64bitレジスタ名 */
            int rm_wide = (digit >= 2) ? 1 : wide;
            d_decode_modrm(&c, rex, rm_wide, rm_text, sizeof(rm_text), &reg);
            switch (digit) {
                case 0: d_strcpy(mnemonic, sizeof(mnemonic), "inc"); break;
                case 1: d_strcpy(mnemonic, sizeof(mnemonic), "dec"); break;
                case 2: d_strcpy(mnemonic, sizeof(mnemonic), "call"); break;
                case 4: d_strcpy(mnemonic, sizeof(mnemonic), "jmp"); break;
                case 6: d_strcpy(mnemonic, sizeof(mnemonic), "push"); break;
                default: ok = 0; break;
            }
            sb_str(&ops, rm_text);
            break;
        }

        /* ---- group3: test/not/neg/mul/imul/div/idiv ---- */
        case 0xF7: {
            UINT8 modrm_peek = (c.pos < code_len) ? code[c.pos] : 0;
            UINT8 digit = (UINT8)((modrm_peek >> 3) & 7);
            d_decode_modrm(&c, rex, wide, rm_text, sizeof(rm_text), &reg);
            switch (digit) {
                case 0: d_strcpy(mnemonic, sizeof(mnemonic), "test"); break;
                case 2: d_strcpy(mnemonic, sizeof(mnemonic), "not"); break;
                case 3: d_strcpy(mnemonic, sizeof(mnemonic), "neg"); break;
                case 4: d_strcpy(mnemonic, sizeof(mnemonic), "mul"); break;
                case 5: d_strcpy(mnemonic, sizeof(mnemonic), "imul"); break;
                case 6: d_strcpy(mnemonic, sizeof(mnemonic), "div"); break;
                default: d_strcpy(mnemonic, sizeof(mnemonic), "idiv"); break;
            }
            sb_str(&ops, rm_text);
            if (digit == 0) {
                sb_str(&ops, ", ");
                sb_shex(&ops, (INT64)(INT32)d_u32(&c));
            }
            break;
        }

        /* ---- 2バイトopcode ---- */
        case 0x0F: {
            UINT8 op2 = d_u8(&c);
            if (op2 >= 0x80 && op2 <= 0x8F) {
                INT64 rel = (INT64)(INT32)d_u32(&c);
                d_strcpy(mnemonic, sizeof(mnemonic), g_jcc[op2 - 0x80]);
                d_branch_target(&ops, code_len, c.pos, rel);
            } else if (op2 >= 0x90 && op2 <= 0x9F) {
                d_decode_modrm(&c, rex, 0, rm_text, sizeof(rm_text), &reg);
                /* setcc: "set" + jccニモニックの条件部分 */
                d_sb_t mn;
                sb_init(&mn, mnemonic, sizeof(mnemonic));
                sb_str(&mn, "set");
                sb_str(&mn, g_jcc[op2 - 0x90] + 1);
                sb_str(&ops, rm_text);
            } else if (op2 == 0x1F) {
                d_decode_modrm(&c, rex, wide, rm_text, sizeof(rm_text), &reg);
                d_strcpy(mnemonic, sizeof(mnemonic), "nop");
                sb_str(&ops, rm_text);
            } else if (op2 == 0xA2) {
                d_strcpy(mnemonic, sizeof(mnemonic), "cpuid");
            } else if (op2 == 0xAF) {
                d_decode_modrm(&c, rex, wide, rm_text, sizeof(rm_text), &reg);
                d_strcpy(mnemonic, sizeof(mnemonic), "imul");
                sb_str(&ops, d_regname(reg, wide));
                sb_str(&ops, ", ");
                sb_str(&ops, rm_text);
            } else if (op2 == 0xB6 || op2 == 0xB7 || op2 == 0xBE || op2 == 0xBF) {
                d_decode_modrm(&c, rex, wide, rm_text, sizeof(rm_text), &reg);
                d_strcpy(mnemonic, sizeof(mnemonic), (op2 == 0xB6 || op2 == 0xB7) ? "movzx" : "movsx");
                sb_str(&ops, d_regname(reg, wide));
                sb_str(&ops, ", ");
                sb_str(&ops, rm_text);
            } else {
                ok = 0;
            }
            break;
        }

        default:
            ok = 0;
            break;
    }

    if (!ok || c.truncated) {
        /* デコードできなかった/コード末尾を踏み越えた。1バイトだけ進めて再開できる
           ようにする(linear sweepが以降の全行を失わないため) */
        return d_emit_bad(out, code, code_len, offset);
    }

    out->kind = OS_DISASM_INSN;
    out->offset = offset;
    out->length = c.pos - offset;
    d_strcpy(out->mnemonic, sizeof(out->mnemonic), mnemonic);
    d_strcpy(out->operands, sizeof(out->operands), operands);
    d_fill_bytes(out, code, code_len, offset);
    return out->length;
}

/* ============================== 整形 ============================== */

/** バイト列の表示に使う最大バイト数(movabsの10バイトが収まる幅) */
#define OS_DISASM_SHOWN_BYTES 10

UINT64 os_disasm_format_bytes(const os_disasm_insn_t *insn, char *buf, UINT64 buf_size) {
    d_sb_t sb;
    sb_init(&sb, buf, buf_size);
    if (insn == 0) {
        return 0;
    }
    UINT64 shown = insn->length < OS_DISASM_MAX_INSN_BYTES ? insn->length : OS_DISASM_MAX_INSN_BYTES;
    for (UINT64 i = 0; i < shown; i++) {
        if (i > 0) {
            sb_char(&sb, ' ');
        }
        sb_hex_pad(&sb, insn->bytes[i], 2);
    }
    if (insn->length > shown) {
        sb_str(&sb, " ..");
    }
    return sb.len;
}

UINT64 os_disasm_format_line(const os_disasm_insn_t *insn, char *buf, UINT64 buf_size) {
    d_sb_t sb;
    sb_init(&sb, buf, buf_size);
    if (insn == 0) {
        return 0;
    }

    sb_hex_pad(&sb, insn->offset, 4);
    sb_str(&sb, "  ");

    UINT64 shown = insn->length < OS_DISASM_SHOWN_BYTES ? insn->length : OS_DISASM_SHOWN_BYTES;
    UINT64 column = 0;
    for (UINT64 i = 0; i < shown; i++) {
        sb_hex_pad(&sb, insn->bytes[i], 2);
        sb_char(&sb, ' ');
        column += 3;
    }
    if (insn->length > shown) {
        sb_str(&sb, ".. ");
        column += 3;
    }
    /* バイト列の欄を固定幅(10バイト分 + 省略記号)に揃える */
    while (column < (OS_DISASM_SHOWN_BYTES + 1) * 3) {
        sb_char(&sb, ' ');
        column++;
    }

    sb_str(&sb, insn->mnemonic);
    if (insn->operands[0] != 0) {
        sb_char(&sb, ' ');
        sb_str(&sb, insn->operands);
    }
    return sb.len;
}
