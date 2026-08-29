#include <stdint.h>

#include "subprimitive.h"
#include "runtime.h"
#include "lisp.h"
#include "interrupt.h"

/**
 * ハードウェア層の組み込み関数%%IN-8。第一引数のポート番号から1バイト読み込む。
 * @param args (port) portはFIXNUM
 * @param env 呼び出し時の環境(未使用)
 * @return 読み込んだ値のFIXNUM
 */
lisp_val_t cc_in_8(lisp_val_t args, lisp_val_t env) {
    (void)env;
    uint16_t port = (uint16_t)(cc_car(args) >> 3);
    return os_make_fixnum(inb(port));
}

/**
 * ハードウェア層の組み込み関数%%OUT-8。第一引数のポート番号へ第二引数の1バイトを出力する。
 * @param args (port value) port/valueはFIXNUM
 * @param env 呼び出し時の環境(未使用)
 * @return 出力した値のFIXNUM
 */
lisp_val_t cc_out_8(lisp_val_t args, lisp_val_t env) {
    (void)env;
    uint16_t port = (uint16_t)(cc_car(args) >> 3);
    uint8_t value = (uint8_t)(cc_car(cc_cdr(args)) >> 3);
    outb(port, value);
    return os_make_fixnum(value);
}

/**
 * ハードウェア層の組み込み関数%%PEEK。第一引数のアドレスから1バイト読み込む。
 * @param args (addr) addrはFIXNUM
 * @param env 呼び出し時の環境(未使用)
 * @return 読み込んだ値のFIXNUM
 */
lisp_val_t cc_peek(lisp_val_t args, lisp_val_t env) {
    (void)env;
    lisp_addr_t addr = (lisp_addr_t)(cc_car(args) >> 3);
    UINT8 value = *(volatile UINT8 *)addr;
    return os_make_fixnum(value);
}

/**
 * ハードウェア層の組み込み関数%%POKE。第一引数のアドレスへ第二引数の1バイトを書き込む。
 * @param args (addr value) addr/valueはFIXNUM
 * @param env 呼び出し時の環境(未使用)
 * @return 書き込んだ値のFIXNUM
 */
lisp_val_t cc_poke(lisp_val_t args, lisp_val_t env) {
    (void)env;
    lisp_addr_t addr = (lisp_addr_t)(cc_car(args) >> 3);
    UINT8 value = (UINT8)(cc_car(cc_cdr(args)) >> 3);
    *(volatile UINT8 *)addr = value;
    return os_make_fixnum(value);
}

/**
 * ハードウェア層の組み込み関数%%IN-16。第一引数のポート番号から2バイト読み込む。
 * @param args (port) portはFIXNUM
 * @param env 呼び出し時の環境(未使用)
 * @return 読み込んだ値のFIXNUM
 */
lisp_val_t cc_in_16(lisp_val_t args, lisp_val_t env) {
    (void)env;
    uint16_t port = (uint16_t)(cc_car(args) >> 3);
    return os_make_fixnum(inw(port));
}

/**
 * ハードウェア層の組み込み関数%%OUT-16。第一引数のポート番号へ第二引数の2バイトを出力する。
 * @param args (port value) port/valueはFIXNUM
 * @param env 呼び出し時の環境(未使用)
 * @return 出力した値のFIXNUM
 */
lisp_val_t cc_out_16(lisp_val_t args, lisp_val_t env) {
    (void)env;
    uint16_t port = (uint16_t)(cc_car(args) >> 3);
    uint16_t value = (uint16_t)(cc_car(cc_cdr(args)) >> 3);
    outw(port, value);
    return os_make_fixnum(value);
}

/**
 * ビット演算の組み込み関数%%LOGAND。第一引数と第二引数のビットごとのANDを取る。
 * @param args (a b) a/bは非負のFIXNUM
 * @param env 呼び出し時の環境(未使用)
 * @return a AND bのFIXNUM
 */
lisp_val_t cc_logand(lisp_val_t args, lisp_val_t env) {
    (void)env;
    uint64_t a = (uint64_t)(cc_car(args) >> 3);
    uint64_t b = (uint64_t)(cc_car(cc_cdr(args)) >> 3);
    return os_make_fixnum(a & b);
}

/**
 * ビット演算の組み込み関数%%LOGIOR。第一引数と第二引数のビットごとのORを取る。
 * @param args (a b) a/bは非負のFIXNUM
 * @param env 呼び出し時の環境(未使用)
 * @return a OR bのFIXNUM
 */
lisp_val_t cc_logior(lisp_val_t args, lisp_val_t env) {
    (void)env;
    uint64_t a = (uint64_t)(cc_car(args) >> 3);
    uint64_t b = (uint64_t)(cc_car(cc_cdr(args)) >> 3);
    return os_make_fixnum(a | b);
}

/**
 * ビット演算の組み込み関数%%LOGXOR。第一引数と第二引数のビットごとのXORを取る。
 * @param args (a b) a/bは非負のFIXNUM
 * @param env 呼び出し時の環境(未使用)
 * @return a XOR bのFIXNUM
 */
lisp_val_t cc_logxor(lisp_val_t args, lisp_val_t env) {
    (void)env;
    uint64_t a = (uint64_t)(cc_car(args) >> 3);
    uint64_t b = (uint64_t)(cc_car(cc_cdr(args)) >> 3);
    return os_make_fixnum(a ^ b);
}

/**
 * ビット演算の組み込み関数%%ASH。第一引数を第二引数の分だけシフトする。
 * countが非負(符号ビット0)なら左シフト、負(符号ビット1)なら絶対値分の右シフトを行う。
 * @param args (a count) aは非負のFIXNUM、countは符号付きFIXNUM
 * @param env 呼び出し時の環境(未使用)
 * @return シフト後の値のFIXNUM
 */
lisp_val_t cc_ash(lisp_val_t args, lisp_val_t env) {
    (void)env;
    uint64_t value = (uint64_t)(cc_car(args) >> 3);
    lisp_val_t count_val = cc_car(cc_cdr(args));
    UINT64 magnitude = os_fixnum_magnitude(count_val);
    uint64_t result = os_fixnum_is_negative(count_val) ? (value >> magnitude) : (value << magnitude);
    return os_make_fixnum(result);
}

/**
 * 文字コード変換の組み込み関数%%CHAR-CODE。文字をタグ(TAG_CHAR)を外した文字コードの
 * FIXNUMにする(文字はFIXNUMと同じ即値表現で下位3bitのタグのみ異なるため、タグの
 * 付け替えだけで変換できる)。
 * @param args (char) charはCHARACTER
 * @param env 呼び出し時の環境(未使用)
 * @return 文字コードのFIXNUM
 */
lisp_val_t cc_char_code(lisp_val_t args, lisp_val_t env) {
    (void)env;
    uint64_t code = (uint64_t)(cc_car(args) >> 3);
    return os_make_fixnum(code);
}

/**
 * 文字コード変換の組み込み関数%%CODE-CHAR。文字コードのFIXNUMをCHARACTER(TAG_CHAR)にする。
 * @param args (code) codeはFIXNUM(文字コード)
 * @param env 呼び出し時の環境(未使用)
 * @return 対応するCHARACTER
 */
lisp_val_t cc_code_char(lisp_val_t args, lisp_val_t env) {
    (void)env;
    uint64_t code = (uint64_t)(cc_car(args) >> 3);
    return ((lisp_val_t)code << 3) | TAG_CHAR;
}

/** %%IN-8/%%OUT-8/%%PEEK/%%POKE/%%IN-16/%%OUT-16/%%LOGAND/%%LOGIOR/%%LOGXOR/%%ASH/%%CHAR-CODE/%%CODE-CHARをglobal_environmentに関数として登録する */
void os_register_subprimitives(void) {
    os_set_function(os_make_symbol("%%IN-8"), os_make_native_function((lisp_addr_t)(void *)cc_in_8), global_environment);
    os_set_function(os_make_symbol("%%OUT-8"), os_make_native_function((lisp_addr_t)(void *)cc_out_8), global_environment);
    os_set_function(os_make_symbol("%%PEEK"), os_make_native_function((lisp_addr_t)(void *)cc_peek), global_environment);
    os_set_function(os_make_symbol("%%POKE"), os_make_native_function((lisp_addr_t)(void *)cc_poke), global_environment);
    os_set_function(os_make_symbol("%%IN-16"), os_make_native_function((lisp_addr_t)(void *)cc_in_16), global_environment);
    os_set_function(os_make_symbol("%%OUT-16"), os_make_native_function((lisp_addr_t)(void *)cc_out_16), global_environment);
    os_set_function(os_make_symbol("%%LOGAND"), os_make_native_function((lisp_addr_t)(void *)cc_logand), global_environment);
    os_set_function(os_make_symbol("%%LOGIOR"), os_make_native_function((lisp_addr_t)(void *)cc_logior), global_environment);
    os_set_function(os_make_symbol("%%LOGXOR"), os_make_native_function((lisp_addr_t)(void *)cc_logxor), global_environment);
    os_set_function(os_make_symbol("%%ASH"), os_make_native_function((lisp_addr_t)(void *)cc_ash), global_environment);
    os_set_function(os_make_symbol("%%CHAR-CODE"), os_make_native_function((lisp_addr_t)(void *)cc_char_code), global_environment);
    os_set_function(os_make_symbol("%%CODE-CHAR"), os_make_native_function((lisp_addr_t)(void *)cc_code_char), global_environment);
}
