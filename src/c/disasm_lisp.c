/**
 * 逆アセンブラのLispからの入口。
 *
 * デコーダ本体(src/c/disasm.c)はランタイムに依存しない純粋な変換なので、
 * 関数オブジェクトからコード範囲を取り出す部分と、結果をLisp値へ組み立てる部分だけを
 * ここに分けてある(stream.c と stream_lisp.c の関係と同じ)。
 * Lisp側のラッパーは src/lisp/disassemble.lisp。
 */

#include "disasm_lisp.h"
#include "disasm.h"
#include "lisp.h"
#include "runtime.h"

/** %%DISASM-ITEMが返す整形済み1行の最大長 */
#define OS_DISASM_LINE_SIZE 192

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
 * 戻り値のリストは (長さ 種別 ニモニック オペランド バイト列 整形済み1行) で、
 * 種別は 0=命令 / 1=埋め込み文字列 / 2=デコード失敗。
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

void os_register_disasm(void) {
    os_set_function(os_make_symbol("%%DISASM-CODE-BASE"),
                    os_make_native_function((lisp_addr_t)(void *)primitive_disasm_code_base), global_environment);
    os_set_function(os_make_symbol("%%DISASM-CODE-LEN"),
                    os_make_native_function((lisp_addr_t)(void *)primitive_disasm_code_len), global_environment);
    os_set_function(os_make_symbol("%%DISASM-ENTRY-OFFSET"),
                    os_make_native_function((lisp_addr_t)(void *)primitive_disasm_entry_offset), global_environment);
    os_set_function(os_make_symbol("%%DISASM-ITEM"),
                    os_make_native_function((lisp_addr_t)(void *)primitive_disasm_item), global_environment);
}
