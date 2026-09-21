/**
 * 逆アセンブラのLispからの入口。
 *
 * デコーダ本体(src/c/disasm.c)はランタイムに依存しない純粋な変換なので、
 * 関数オブジェクトからコード範囲を取り出す部分と、結果をLisp値へ組み立てる部分だけを
 * ここに分けてある(stream.c と stream_lisp.c の関係と同じ)。
 * Lisp側のラッパーは src/lisp/disassemble.lisp。
 */

#include "disasm_lisp.h"
#include "disasm_symtab.h"
#include "disasm.h"
#include "lisp.h"
#include "runtime.h"

/** %%DISASM-ITEMが返す整形済み1行の最大長 */
#define OS_DISASM_LINE_SIZE 192

/**
 * insnが絶対アドレスを指しているなら、その所属領域を注釈として埋める。
 * デコーダ(disasm.c)はランタイムの境界を知らないのでここで埋める
 * (disasm.h の os_disasm_insn_t.comment のコメント参照)。
 * どの領域にも当たらない場合は空文字列のままにする — 「引けなかった」ことを
 * 示す表示は出さない(全行に無意味な注釈が付くだけになるため)。
 */
/** comment へ 16 進を追記する(桁数ぶんだけ。0 は "0") */
static UINT64 d_comment_hex(char *buf, UINT64 n, UINT64 cap, UINT64 v) {
    char tmp[17];
    UINT64 t = 0;
    if (v == 0) {
        tmp[t++] = '0';
    }
    while (v != 0 && t < 16) {
        UINT64 nib = v & 0xF;
        tmp[t++] = (char)(nib < 10 ? ('0' + nib) : ('a' + nib - 10));
        v >>= 4;
    }
    while (t > 0 && n + 1 < cap) {
        buf[n++] = tmp[--t];
    }
    return n;
}

/* ===== JIT 関数のシンボル逆引き(documents/disasm-symbols.md §4-5)=====
 *
 * Immobilized Space 上のアドレスから、それを持つ関数の名前を引く。
 * カーネル .text と違いビルド時には決まらないので、**実行時に環境を走査**する。
 *
 * **グローバル環境を起点にする。** 呼び出し文脈の環境から辿ると、同じコードを
 * 逆アセンブルしても呼ぶ場所で結果が変わってしまう。
 *
 * **範囲マッチ**にする。code_base <= addr < code_base + code_len なら、
 * その関数の内側を指しているので `NAME+0x1c` の形で出せる。
 *
 * [引けないのが正しい場合がある]
 *  - **再定義された関数の古いコード。** Immobilized Space には残るが、
 *    どの環境からも参照されないので引けない
 *  - **別名**(同じ関数オブジェクトを 2 つの名前で登録した場合)。
 *    最初に見つかったものを返す
 */
static lisp_val_t d_env_functions_alist(lisp_val_t env) {
    /* 環境は (name . x) (variables . y) (functions . z) ... の alist。
       3 番目が functions スロットである(os_make_environment_raw 参照)。
       スロットの並びに依存せず、**シンボル名で引く**ほうが壊れにくい */
    for (lisp_val_t cur = env; cur != nil && (cur & TAG_MASK) == TAG_CONS; cur = cc_cdr(cur)) {
        lisp_val_t slot = cc_car(cur);
        if ((slot & TAG_MASK) != TAG_CONS) {
            continue;
        }
        if (cc_car(slot) == os_make_symbol("FUNCTIONS")) {
            return cc_cdr(slot);
        }
    }
    return nil;
}

/**
 * JIT コンパイル済み関数のコード範囲に addr が入っていれば、その名前を返す。
 * @param addr 実行時アドレス
 * @param out_offset コード先頭からのバイト数(引けたときだけ書く)
 * @return 名前のシンボル。引けなければ nil
 */
static lisp_val_t d_lookup_jit_symbol(UINT64 addr, UINT64 *out_offset) {
    lisp_val_t fns = d_env_functions_alist(global_environment);
    for (lisp_val_t cur = fns; cur != nil && (cur & TAG_MASK) == TAG_CONS; cur = cc_cdr(cur)) {
        lisp_val_t pair = cc_car(cur);
        if ((pair & TAG_MASK) != TAG_CONS) {
            continue;
        }
        lisp_val_t fn = cc_cdr(pair);
        if ((fn & TAG_MASK) != TAG_INSTANCE) {
            continue;
        }
        UINT64 *obj = (UINT64 *)(fn & ~TAG_MASK);
        if (obj[0] != MAGIC_FUNCTION_NATIVE || obj[1] == 0) {
            continue;
        }
        za_fn_meta_t *meta = (za_fn_meta_t *)obj[1];
        if (meta->code_len == 0) {
            continue;   /* 組み込み primitive / AOT は機械語ブロックを持たない */
        }
        if (addr >= meta->code_base && addr < meta->code_base + meta->code_len) {
            if (out_offset != 0) {
                *out_offset = addr - meta->code_base;
            }
            return cc_car(pair);
        }
    }
    return nil;
}

static void d_fill_region_comment(os_disasm_insn_t *insn) {
    insn->comment[0] = 0;
    if (!insn->has_target_addr) {
        return;
    }
    os_addr_region_t region = os_classify_addr((lisp_addr_t)insn->target_addr);

    /* [シンボル解決] カーネル .text なら**名前**を出す。
       領域名("<kernel>")より、どの関数を呼んでいるかのほうが役に立つ
       (documents/disasm-symbols.md)。引けなければ領域名へ落ちる。 */
    if (region == OS_ADDR_KERNEL_TEXT) {
        UINT64 off = 0;
        const char *sym = os_disasm_lookup_kernel_symbol(insn->target_addr, &off);
        if (sym != 0) {
            UINT64 n = 0;
            while (*sym != 0 && n + 1 < sizeof(insn->comment)) {
                insn->comment[n++] = *sym++;
            }
            if (off != 0 && n + 3 < sizeof(insn->comment)) {
                insn->comment[n++] = '+';
                insn->comment[n++] = '0';
                insn->comment[n++] = 'x';
                n = d_comment_hex(insn->comment, n, sizeof(insn->comment), off);
            }
            insn->comment[n] = 0;
            return;
        }
    }

    /* [シンボル解決] Immobilized Space なら JIT 関数の名前を環境から引く */
    if (region == OS_ADDR_IMMOBILIZED) {
        UINT64 off = 0;
        lisp_val_t sym = d_lookup_jit_symbol(insn->target_addr, &off);
        if (sym != nil && (sym & TAG_MASK) == TAG_SYMBOL) {
            lisp_val_t str = ((lisp_val_t *)(sym & ~TAG_MASK))[0];
            if ((str & TAG_MASK) == TAG_STRING) {
                lisp_addr_t sa = str & ~TAG_MASK;
                UINT64 slen = ((UINT64 *)sa)[0];
                const char *sp = (const char *)(sa + 8);
                UINT64 n = 0;
                for (UINT64 i = 0; i < slen && n + 1 < sizeof(insn->comment); i++) {
                    insn->comment[n++] = sp[i];
                }
                if (off != 0 && n + 3 < sizeof(insn->comment)) {
                    insn->comment[n++] = '+';
                    insn->comment[n++] = '0';
                    insn->comment[n++] = 'x';
                    n = d_comment_hex(insn->comment, n, sizeof(insn->comment), off);
                }
                insn->comment[n] = 0;
                return;
            }
        }
    }

    const char *name = os_addr_region_name(region);
    if (name == 0) {
        return;
    }
    UINT64 n = 0;
    insn->comment[n++] = '<';
    while (*name != 0 && n + 2 < sizeof(insn->comment)) {
        insn->comment[n++] = *name++;
    }
    insn->comment[n++] = '>';
    insn->comment[n] = 0;
}

/* ============================== 関数オブジェクト → コード範囲 ============================== */

/**
 * 関数オブジェクト(またはその名前のsymbol)からza_fn_meta_tを取り出す。
 * symbolを受け付けるのは、ISLispの(function name)が構文上リテラルな名前しか
 * 取れず、`(disassemble 'fib)`のように**値として**渡された名前を実行時に解決する
 * 手段がLisp側に無いため。
 * @param val 関数オブジェクト、またはその名前のsymbol
 * @param env symbolを解決する環境
 * @return meta。関数でない/未定義ならば0
 */
static za_fn_meta_t *d_fn_meta(lisp_val_t val, lisp_val_t env) {
    if ((val & TAG_MASK) == TAG_SYMBOL) {
        val = os_get_function(val, env);
    }
    if ((val & TAG_MASK) != TAG_INSTANCE) {
        return 0;
    }
    UINT64 *obj = (UINT64 *)(val & ~TAG_MASK);
    if (obj[0] != MAGIC_FUNCTION_NATIVE) {
        return 0;
    }
    return (za_fn_meta_t *)obj[1];
}

/**
 * 組み込み関数%%DISASM-CODE-BASE。JITコンパイル済み関数の機械語ブロックの
 * 先頭アドレスをfixnumで返す。逆アセンブルできる機械語を持たない場合
 * (組み込みprimitive、AOTのlifted closure、インタプリタ実行の関数)はnil。
 * @param args (function)
 * @param env 呼び出し時の環境(未使用)
 * @return コード先頭アドレスのfixnum、またはnil
 */
static lisp_val_t primitive_disasm_code_base(lisp_val_t args, lisp_val_t env) {
    za_fn_meta_t *meta = d_fn_meta(cc_car(args), env);
    if (meta == 0 || meta->code_len == 0) {
        return nil;
    }
    return os_make_fixnum(meta->code_base);
}

/**
 * 組み込み関数%%DISASM-CODE-LEN。JITコンパイル済み関数の機械語ブロックのバイト長。
 * @param args (function)
 * @param env 呼び出し時の環境(未使用)
 * @return バイト長のfixnum、またはnil
 */
static lisp_val_t primitive_disasm_code_len(lisp_val_t args, lisp_val_t env) {
    za_fn_meta_t *meta = d_fn_meta(cc_car(args), env);
    if (meta == 0 || meta->code_len == 0) {
        return nil;
    }
    return os_make_fixnum(meta->code_len);
}

/**
 * 組み込み関数%%DISASM-ENTRY-OFFSET。エントリポイントがコードブロックの先頭から
 * 何バイト目にあるかを返す。ABI-M5の固定引数エントリを持つ関数では、
 * fixed_entry(=0)とcons_entryの2つが別の位置にある。
 * @param args (function kind) kind: 0=consリストABI、1=固定引数レジスタ渡し
 * @param env 呼び出し時の環境(未使用)
 * @return オフセットのfixnum。該当エントリが無ければnil
 */
static lisp_val_t primitive_disasm_entry_offset(lisp_val_t args, lisp_val_t env) {
    za_fn_meta_t *meta = d_fn_meta(cc_car(args), env);
    if (meta == 0 || meta->code_len == 0) {
        return nil;
    }
    UINT64 kind = os_fixnum_magnitude(cc_car(cc_cdr(args)));
    UINT64 entry = (kind == 0) ? meta->cons_entry : meta->fixed_entry;
    if (entry == 0 || entry < meta->code_base || entry >= meta->code_base + meta->code_len) {
        return nil;
    }
    return os_make_fixnum(entry - meta->code_base);
}

/**
 * 組み込み関数%%DISASM-ITEM。addrから始まるcode-lenバイトのコードのうち、
 * offsetの位置にある1項目(命令、またはコードに埋め込まれた文字列)をデコードする。
 *
 * 戻り値のリストは (長さ 種別 ニモニック オペランド バイト列 整形済み1行 注釈) で、
 * 種別は 0=命令 / 1=埋め込み文字列 / 2=デコード失敗。
 * 注釈はこの命令が指す絶対アドレスの所属領域("<kernel>"等)で、
 * 絶対アドレスを持たない命令、および領域が引けなかった場合は空文字列。
 * offsetが範囲外ならnilを返すので、呼び出し側はこれをループの終端判定に使える。
 *
 * @param args (addr code-len offset)
 * @param env 呼び出し時の環境(未使用)
 * @return 上記6要素のリスト、またはnil
 */
static lisp_val_t primitive_disasm_item(lisp_val_t args, lisp_val_t env) {
    (void)env;
    UINT64 addr = os_fixnum_magnitude(cc_car(args));
    UINT64 code_len = os_fixnum_magnitude(cc_car(cc_cdr(args)));
    UINT64 offset = os_fixnum_magnitude(cc_car(cc_cdr(cc_cdr(args))));

    /* デコード結果は生データのみ(lisp_val_tを一切含まない)のCローカルなので、
       以降のアロケーションでGCが走っても影響を受けない */
    os_disasm_insn_t insn;
    if (addr == 0 || os_disasm_item((const UINT8 *)(lisp_addr_t)addr, code_len, offset, &insn) == 0) {
        return nil;
    }

    d_fill_region_comment(&insn);

    char line[OS_DISASM_LINE_SIZE];
    os_disasm_format_line(&insn, line, sizeof(line));

    char bytes_text[OS_DISASM_MAX_INSN_BYTES * 3 + 4];
    os_disasm_format_bytes(&insn, bytes_text, sizeof(bytes_text));

    /* [GC安全性] documents/pitfalls.md 原則4。os_make_string/os_make_consは確保を
       伴うため、リストを後ろから組み立てる途中の値は必ず保護済みの**変数**に置き、
       次の確保を跨いでCの一時値のまま持ち回らない */
    lisp_val_t acc = nil;
    lisp_val_t tmp = nil;
    GC_PROTECT(acc);
    GC_PROTECT(tmp);

    tmp = os_make_string(insn.comment);
    acc = os_make_cons(tmp, acc);
    tmp = os_make_string(line);
    acc = os_make_cons(tmp, acc);
    tmp = os_make_string(bytes_text);
    acc = os_make_cons(tmp, acc);
    tmp = os_make_string(insn.operands);
    acc = os_make_cons(tmp, acc);
    tmp = os_make_string(insn.mnemonic);
    acc = os_make_cons(tmp, acc);
    tmp = os_make_fixnum((UINT64)insn.kind);
    acc = os_make_cons(tmp, acc);
    tmp = os_make_fixnum(insn.length);
    acc = os_make_cons(tmp, acc);
    return acc;
}

/**
 * 組み込み関数%%DISASM-CLASSIFY-ADDR。アドレスの所属領域を返す。
 * 戻り値は os_addr_region_t の値そのもの(0=不明 1=kernel 2=immobilized 3=gc-heap)。
 * @param args (addr)
 * @param env 呼び出し時の環境(未使用)
 * @return 領域番号のfixnum
 */
static lisp_val_t primitive_disasm_classify_addr(lisp_val_t args, lisp_val_t env) {
    (void)env;
    lisp_addr_t addr = (lisp_addr_t)os_fixnum_magnitude(cc_car(args));
    return os_make_fixnum((UINT64)os_classify_addr(addr));
}

/**
 * 組み込み関数%%DISASM-REGION-BOUNDS。領域の実行時の境界を(start . end)で返す。
 * endは**範囲外**(半開区間の上端)。境界が未確定ならnil。
 *
 * .textの範囲はPE/COFFヘッダを辿って求めており、ヘッダが期待した形でなければ
 * 黙って未確定のままになる。それが起きたことを外から確認できるようにするために
 * 公開する(documents/pitfalls.md 原則6)。
 * @param args (region) regionは os_addr_region_t の値
 * @param env 呼び出し時の環境(未使用)
 * @return (start . end) のcons、または境界が未確定ならnil
 */
static lisp_val_t primitive_disasm_region_bounds(lisp_val_t args, lisp_val_t env) {
    (void)env;
    UINT64 region = os_fixnum_magnitude(cc_car(args));
    lisp_addr_t start = 0;
    lisp_addr_t end = 0;
    if (!os_addr_region_bounds((os_addr_region_t)region, &start, &end)) {
        return nil;
    }
    /* fixnum 2つを作ってからconsする。os_make_fixnumは確保しないのでGCは走らない */
    return os_make_cons(os_make_fixnum(start), os_make_fixnum(end));
}

void os_register_disasm(void) {
    os_set_function(os_make_symbol("%%DISASM-CODE-BASE"),
                    os_make_native_function((lisp_addr_t)(void *)primitive_disasm_code_base), global_environment);
    os_set_function(os_make_symbol("%%DISASM-CODE-LEN"),
                    os_make_native_function((lisp_addr_t)(void *)primitive_disasm_code_len), global_environment);
    os_set_function(os_make_symbol("%%DISASM-ENTRY-OFFSET"),
                    os_make_native_function((lisp_addr_t)(void *)primitive_disasm_entry_offset), global_environment);
    os_set_function(os_make_symbol("%%DISASM-ITEM"),
                    os_make_native_function((lisp_addr_t)(void *)primitive_disasm_item), global_environment);
    os_set_function(os_make_symbol("%%DISASM-CLASSIFY-ADDR"),
                    os_make_native_function((lisp_addr_t)(void *)primitive_disasm_classify_addr), global_environment);
    os_set_function(os_make_symbol("%%DISASM-REGION-BOUNDS"),
                    os_make_native_function((lisp_addr_t)(void *)primitive_disasm_region_bounds), global_environment);
}
