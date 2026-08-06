#include "reader.h"
#include "runtime.h"
#include "framebuffer.h"

/** 数値/シンボルトークンや文字列リテラルを組み立てる際の作業バッファの上限 */
#define READER_TOKEN_MAX 128

/**
 * proc の入力バッファ(stdin_buf/read_pos/ready)を1行分リセットする。
 * @param proc リセット対象のプロセス
 */
static void reset_line(process_t *proc) {
    proc->stdin_len = 0;
    proc->read_pos = 0;
    proc->ready = 0;
}

/**
 * 1回の os_read 呼び出し内で "> " を表示済みかどうか。
 * 複数行にわたる式(文字列やリストの途中改行)の継続行では表示しない。
 */
static int prompt_shown = 0;

/**
 * proc のバッファを使い切った際、次の行の入力(Enterによるready確定)を待つ。
 * @param proc 入力待ちするプロセス
 */
static void ensure_data(process_t *proc) {
    if (proc->read_pos < proc->stdin_len) {
        return;
    }
    reset_line(proc);
    if (!prompt_shown) {
        proc->stdout_buffer->write_string(proc->stdout_buffer, "> ");
        prompt_shown = 1;
    }
    os_wait_for_more_input(proc);
}

/**
 * proc にまだ読める文字が残っているかどうかを返す。無ければ次の行の入力を待つ。
 * @param proc 判定対象のプロセス
 * @return 読める文字が残っていれば非0、そうでなければ0
 */
static int has_more(process_t *proc) {
    ensure_data(proc);
    return proc->read_pos < proc->stdin_len;
}

/**
 * proc の読取カーソル位置の文字を、読み進めずに返す。
 * @param proc 対象のプロセス
 * @return カーソル位置の文字
 */
static char peek(process_t *proc) {
    return (char)proc->stdin_buf[proc->read_pos];
}

/**
 * proc の読取カーソル位置の文字を返し、カーソルを1つ進める。
 * @param proc 対象のプロセス
 * @return カーソル位置の文字
 */
static char advance(process_t *proc) {
    return (char)proc->stdin_buf[proc->read_pos++];
}

/**
 * c が空白文字(スペース・タブ・改行・CR)かどうかを返す。
 * @param c 判定する文字
 * @return 空白文字なら非0、そうでなければ0
 */
static int is_whitespace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/**
 * c がトークンの区切り文字(空白・括弧・ダブルクオート・クオート・quasiquote・unquote・コメント開始)かどうかを返す。
 * @param c 判定する文字
 * @return 区切り文字なら非0、そうでなければ0
 */
static int is_delimiter(char c) {
    return is_whitespace(c) || c == '(' || c == ')' || c == '"' || c == '\''
        || c == '`' || c == ',' || c == ';';
}

/**
 * c が数字('0'〜'9')かどうかを返す。
 * @param c 判定する文字
 * @return 数字なら非0、そうでなければ0
 */
static int is_digit(char c) {
    return c >= '0' && c <= '9';
}

/**
 * proc の読取カーソルを、空白文字でも';'コメント(その行末まで)でもない文字が来るまで進める。
 * @param proc 対象のプロセス
 */
static void skip_whitespace(process_t *proc) {
    while (has_more(proc)) {
        if (is_whitespace(peek(proc))) {
            advance(proc);
            continue;
        }
        if (peek(proc) == ';') {
            while (has_more(proc) && peek(proc) != '\n') {
                advance(proc);
            }
            continue;
        }
        break;
    }
}

static lisp_val_t read_expr(process_t *proc);

/**
 * '(' は呼び出し元で消費済みの前提で、閉じ括弧までのS式を読みリストとして組み立てる。
 * @param proc 読み取り対象のプロセス
 * @return 読み取ったリスト。構文エラーの場合はg_sym_read_error
 */
static lisp_val_t read_list(process_t *proc) {
    skip_whitespace(proc);
    if (!has_more(proc)) {
        return g_sym_read_error; // 閉じ括弧が無いまま入力が終端した
    }
    if (peek(proc) == ')') {
        advance(proc);
        return nil;
    }

    lisp_val_t car = read_expr(proc);
    if (car == g_sym_read_error) {
        return g_sym_read_error;
    }
    lisp_val_t cdr = read_list(proc);
    if (cdr == g_sym_read_error) {
        return g_sym_read_error;
    }
    return os_make_cons(car, cdr);
}

/**
 * '"' は呼び出し元で消費済みの前提で、閉じクォートまでの文字列を読む。エスケープシーケンスは扱わない。
 * @param proc 読み取り対象のプロセス
 * @return 読み取ったSTRING。閉じクォートが無いまま入力が終端した場合はg_sym_read_error
 */
static lisp_val_t read_string(process_t *proc) {
    char token[READER_TOKEN_MAX];
    UINT32 len = 0;

    while (has_more(proc) && peek(proc) != '"') {
        char c = advance(proc);
        if (len < READER_TOKEN_MAX - 1) {
            token[len++] = c;
        }
    }

    if (!has_more(proc)) {
        return g_sym_read_error; // 閉じクォートが無いまま入力が終端した
    }
    advance(proc); // 閉じの '"' を消費

    token[len] = '\0';
    return os_make_string(token);
}

/**
 * 区切り文字までのトークンを読む。数字だけのトークンはfixnum、それ以外はsymbolとして読む。
 * @param proc 読み取り対象のプロセス
 * @return 読み取ったFIXNUMまたはSYMBOL
 */
static lisp_val_t read_atom(process_t *proc) {
    char token[READER_TOKEN_MAX];
    UINT32 len = 0;

    while (has_more(proc) && !is_delimiter(peek(proc))) {
        char c = advance(proc);
        if (len < READER_TOKEN_MAX - 1) {
            token[len++] = c;
        }
    }
    token[len] = '\0';

    int all_digits = (len > 0);
    for (UINT32 i = 0; i < len; i++) {
        if (!is_digit(token[i])) {
            all_digits = 0;
            break;
        }
    }

    if (all_digits) {
        UINT64 value = 0;
        for (UINT32 i = 0; i < len; i++) {
            value = value * 10 + (UINT64)(token[i] - '0');
        }
        return os_make_fixnum(value);
    }

    return os_make_symbol(token);
}

/**
 * proc の読取カーソル位置から1つのS式(リスト・文字列・quote・quasiquote・unquote・アトム)を読む。
 * @param proc 読み取り対象のプロセス
 * @return 読み取ったS式。構文エラーの場合はg_sym_read_error
 */
static lisp_val_t read_expr(process_t *proc) {
    char c = peek(proc);

    if (c == '(') {
        advance(proc);
        return read_list(proc);
    }
    if (c == ')') {
        // 先頭に余分な閉じ括弧がある。次回呼び出しで同じ文字に留まらないよう読み進めておく
        advance(proc);
        return g_sym_read_error;
    }
    if (c == '"') {
        advance(proc);
        return read_string(proc);
    }
    if (c == '\'') {
        advance(proc);
        skip_whitespace(proc);
        if (!has_more(proc)) {
            return g_sym_read_error;
        }
        lisp_val_t quoted = read_expr(proc);
        if (quoted == g_sym_read_error) {
            return g_sym_read_error;
        }
        return os_make_cons(g_sym_quote, os_make_cons(quoted, nil));
    }
    if (c == '`') {
        advance(proc);
        skip_whitespace(proc);
        if (!has_more(proc)) {
            return g_sym_read_error;
        }
        lisp_val_t quoted = read_expr(proc);
        if (quoted == g_sym_read_error) {
            return g_sym_read_error;
        }
        return os_make_cons(g_sym_quasiquote, os_make_cons(quoted, nil));
    }
    if (c == ',') {
        advance(proc);
        lisp_val_t sym = g_sym_unquote;
        if (has_more(proc) && peek(proc) == '@') {
            advance(proc);
            sym = g_sym_unquote_splicing;
        }
        skip_whitespace(proc);
        if (!has_more(proc)) {
            return g_sym_read_error;
        }
        lisp_val_t quoted = read_expr(proc);
        if (quoted == g_sym_read_error) {
            return g_sym_read_error;
        }
        return os_make_cons(sym, os_make_cons(quoted, nil));
    }

    return read_atom(proc);
}

/**
 * proc の標準入力バッファ(stdin_buf)から1つの完全なS式を読み取り、読取カーソル(read_pos)を進める。
 * 読み取れるものが無い場合は nil を返し、バッファをクリアして次の行の入力を待つ。
 * 構文エラー(閉じカッコ不足・文字列リテラル未終端・先頭の余分な ')' など)は g_sym_read_error を返す。
 * @param proc 読み取り対象のプロセス
 * @return 読み取ったS式。読めるものが無ければnil、構文エラーならg_sym_read_error
 */
lisp_val_t os_read(process_t *proc) {
    prompt_shown = 0;
    skip_whitespace(proc);

    if (!has_more(proc)) {
        // 読めるものが無い: バッファをクリアして次の行の入力を待つ
        reset_line(proc);
        return nil;
    }

    return read_expr(proc);
}
