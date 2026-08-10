#include "reader.h"
#include "runtime.h"
#include "framebuffer.h"
#include "eval.h"

/** 数値/シンボルトークンや文字列リテラルを組み立てる際の作業バッファの上限 */
#define READER_TOKEN_MAX 128

/**
 * read_expr以下の内部関数が読み取り元の種別(process/stream)を意識せずに
 * 済むようにするための、文字ソースの抽象化。非破壊的なpeek()を前提とする。
 */
typedef struct {
    char (*peek)(void *ctx);
    char (*advance)(void *ctx);
    int (*has_more)(void *ctx);
    void *ctx;
} reader_source_t;

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

static char process_source_peek(void *ctx) {
    process_t *proc = (process_t *)ctx;
    return (char)proc->stdin_buf[proc->read_pos];
}

static char process_source_advance(void *ctx) {
    process_t *proc = (process_t *)ctx;
    return (char)proc->stdin_buf[proc->read_pos++];
}

static int process_source_has_more(void *ctx) {
    process_t *proc = (process_t *)ctx;
    ensure_data(proc);
    return proc->read_pos < proc->stdin_len;
}

/** 1バイトの先読みキャッシュを持つ、stream用のreader_sourceコンテキスト。
    os_stream_read_charは「読んだら進む」だけのAPIなので、readerが要求する
    非破壊的なpeek()を実現するためにここでキャッシュする。 */
typedef struct {
    os_stream_t *stream;
    char lookahead;
    int has_lookahead;
} stream_source_ctx_t;

/** まだ先読みしていなければstream_read_charで1文字取得してlookaheadに積む */
static int stream_source_fill(stream_source_ctx_t *ctx) {
    if (ctx->has_lookahead) {
        return 1;
    }
    if (os_stream_read_char(ctx->stream, &ctx->lookahead)) {
        ctx->has_lookahead = 1;
        return 1;
    }
    return 0;
}

static int stream_source_has_more(void *raw_ctx) {
    stream_source_ctx_t *ctx = (stream_source_ctx_t *)raw_ctx;
    return stream_source_fill(ctx);
}

static char stream_source_peek(void *raw_ctx) {
    stream_source_ctx_t *ctx = (stream_source_ctx_t *)raw_ctx;
    stream_source_fill(ctx);
    return ctx->lookahead;
}

static char stream_source_advance(void *raw_ctx) {
    stream_source_ctx_t *ctx = (stream_source_ctx_t *)raw_ctx;
    stream_source_fill(ctx);
    ctx->has_lookahead = 0;
    return ctx->lookahead;
}

/** C文字列バッファ(NUL終端不要、lenで長さを持つ)に対するreader_sourceコンテキスト。os_parse_numberが使う。 */
typedef struct {
    const char *buf;
    UINT64 len;
    UINT64 pos;
} string_source_ctx_t;

static char string_source_peek(void *raw_ctx) {
    string_source_ctx_t *ctx = (string_source_ctx_t *)raw_ctx;
    return ctx->buf[ctx->pos];
}

static char string_source_advance(void *raw_ctx) {
    string_source_ctx_t *ctx = (string_source_ctx_t *)raw_ctx;
    return ctx->buf[ctx->pos++];
}

static int string_source_has_more(void *raw_ctx) {
    string_source_ctx_t *ctx = (string_source_ctx_t *)raw_ctx;
    return ctx->pos < ctx->len;
}

/**
 * srcにまだ読める文字が残っているかどうかを返す。無ければ次の入力を待つ(process用)か、
 * これ以上取得できないことを確認する(stream用)。
 * @param src 判定対象の文字ソース
 * @return 読める文字が残っていれば非0、そうでなければ0
 */
static int has_more(reader_source_t *src) {
    return src->has_more(src->ctx);
}

/**
 * srcの読取カーソル位置の文字を、読み進めずに返す。
 * @param src 対象の文字ソース
 * @return カーソル位置の文字
 */
static char peek(reader_source_t *src) {
    return src->peek(src->ctx);
}

/**
 * srcの読取カーソル位置の文字を返し、カーソルを1つ進める。
 * @param src 対象の文字ソース
 * @return カーソル位置の文字
 */
static char advance(reader_source_t *src) {
    return src->advance(src->ctx);
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
 * srcの読取カーソルを、空白文字でも';'コメント(その行末まで)でもない文字が来るまで進める。
 * @param src 対象の文字ソース
 */
static void skip_whitespace(reader_source_t *src) {
    while (has_more(src)) {
        if (is_whitespace(peek(src))) {
            advance(src);
            continue;
        }
        if (peek(src) == ';') {
            while (has_more(src) && peek(src) != '\n') {
                advance(src);
            }
            continue;
        }
        break;
    }
}

static lisp_val_t read_expr(reader_source_t *src);

/**
 * '(' は呼び出し元で消費済みの前提で、閉じ括弧までのS式を読みリストとして組み立てる。
 * @param src 読み取り対象の文字ソース
 * @return 読み取ったリスト。構文エラーの場合はg_sym_read_error
 */
static lisp_val_t read_list(reader_source_t *src) {
    skip_whitespace(src);
    if (!has_more(src)) {
        return g_sym_read_error; // 閉じ括弧が無いまま入力が終端した
    }
    if (peek(src) == ')') {
        advance(src);
        return nil;
    }

    lisp_val_t car = read_expr(src);
    if (car == g_sym_read_error) {
        return g_sym_read_error;
    }
    lisp_val_t cdr = read_list(src);
    if (cdr == g_sym_read_error) {
        return g_sym_read_error;
    }
    return os_make_cons(car, cdr);
}

/**
 * '"' は呼び出し元で消費済みの前提で、閉じクォートまでの文字列を読む。エスケープシーケンスは扱わない。
 * @param src 読み取り対象の文字ソース
 * @return 読み取ったSTRING。閉じクォートが無いまま入力が終端した場合はg_sym_read_error
 */
static lisp_val_t read_string(reader_source_t *src) {
    char token[READER_TOKEN_MAX];
    UINT32 len = 0;

    while (has_more(src) && peek(src) != '"') {
        char c = advance(src);
        if (len < READER_TOKEN_MAX - 1) {
            token[len++] = c;
        }
    }

    if (!has_more(src)) {
        return g_sym_read_error; // 閉じクォートが無いまま入力が終端した
    }
    advance(src); // 閉じの '"' を消費

    token[len] = '\0';
    return os_make_string(token);
}

/**
 * READER_TOKEN_MAX(128、符号込みなので数字部は最大127桁)のトークンを10進・2進・8進・
 * 16進のいずれで解釈しても表現しきれるのに十分なlimb数。最も桁効率の良い16進でも
 * 127桁 = 508bit < 16*32bit = 512bitに収まる(10進なら127桁 < 422bitでさらに余裕がある)。
 */
#define READER_INT_LIMBS 16

/**
 * c が浮動小数点リテラルの構成要素として許される文字('0'-'9', '.', 'e'/'E', '+'/'-')かどうかを返す。
 * read_atomがトークン全体を整数/float/symbolのいずれとして扱うか判定する粗いフィルタに使う。
 * @param c 判定する文字
 * @return floatリテラルの構成要素として許される文字なら非0、そうでなければ0
 */
static int is_float_token_char(char c) {
    return is_digit(c) || c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-';
}

/**
 * token[digit_start..len)をISLisp §19.2のfloat構文([s]dd...d.dd...d、
 * [s]dd...d.dd...dE[s]dd...d、[s]dd...dE[s]dd...dのいずれか)として解析する。
 * token[0..digit_start)の符号は解析済み(negativeへ反映済み)の前提。strtodが無い環境のため、
 * 桁を読みながらmantissaへ逐次累積し、小数部桁数と指数部を最後にまとめて10のべきとして
 * 掛け/割りする手書きの変換を行う。
 * @param token トークン全体(NUL終端)
 * @param len tokenの長さ
 * @param digit_start 数字部の開始位置(符号が無ければ0、あれば1)
 * @param negative tokenの符号が'-'だったかどうか
 * @return 解析したfloat値。ISLisp §19.2の構文に一致しない場合はg_sym_read_error
 */
static lisp_val_t parse_float_token(const char *token, UINT32 len, UINT32 digit_start, int negative) {
    UINT32 i = digit_start;

    UINT32 int_start = i;
    while (i < len && is_digit(token[i])) {
        i++;
    }
    UINT32 int_end = i;
    UINT32 int_len = int_end - int_start;

    double mantissa = 0.0;
    int frac_len = 0;
    int has_dot = 0;

    if (i < len && token[i] == '.') {
        has_dot = 1;
        i++;
        UINT32 frac_start = i;
        while (i < len && is_digit(token[i])) {
            i++;
        }
        UINT32 frac_end = i;
        frac_len = (int)(frac_end - frac_start);
        if (int_len == 0 || frac_len == 0) {
            return g_sym_read_error; // 小数点の前後どちらかに桁が無い
        }
        for (UINT32 j = int_start; j < int_end; j++) {
            mantissa = mantissa * 10.0 + (double)(token[j] - '0');
        }
        for (UINT32 j = frac_start; j < frac_end; j++) {
            mantissa = mantissa * 10.0 + (double)(token[j] - '0');
        }
    } else {
        if (int_len == 0) {
            return g_sym_read_error; // 数字が1つも無い
        }
        for (UINT32 j = int_start; j < int_end; j++) {
            mantissa = mantissa * 10.0 + (double)(token[j] - '0');
        }
    }

    int has_exp = 0;
    int exp_negative = 0;
    int exponent = 0;

    if (i < len && (token[i] == 'e' || token[i] == 'E')) {
        has_exp = 1;
        i++;
        if (i < len && (token[i] == '+' || token[i] == '-')) {
            exp_negative = (token[i] == '-');
            i++;
        }
        UINT32 exp_start = i;
        while (i < len && is_digit(token[i])) {
            i++;
        }
        if (i == exp_start) {
            return g_sym_read_error; // 指数部の桁が無い
        }
        for (UINT32 j = exp_start; j < i; j++) {
            exponent = exponent * 10 + (token[j] - '0');
        }
    }

    if (i != len || (!has_dot && !has_exp)) {
        return g_sym_read_error; // 末尾に余分な文字がある、または小数点も指数部も無い
    }

    int total_exp = (has_exp ? (exp_negative ? -exponent : exponent) : 0) - frac_len;
    double value = mantissa;
    if (total_exp > 0) {
        for (int k = 0; k < total_exp; k++) {
            value *= 10.0;
        }
    } else {
        for (int k = 0; k < -total_exp; k++) {
            value /= 10.0;
        }
    }

    return os_make_float(negative ? -value : value);
}

/**
 * 区切り文字までのトークンを読む。数字だけ(先頭の'+'/'-'による符号も可)のトークンは整数
 * (FIXNUMまたはbignum)、ISLisp §19.2のfloat構文に一致するトークンはfloatとして読む
 * (構文に近いが一致しない場合はg_sym_read_error)。それ以外はsymbolとして読む
 * ('+'/'-'/'1+'/'1-'はISLisp仕様上括弧無しで書けるシンボル名としてsymbolのまま残る)。
 * @param src 読み取り対象の文字ソース
 * @return 読み取った整数(FIXNUMまたはbignum)、float、SYMBOL、または構文エラーの場合はg_sym_read_error
 */
static lisp_val_t read_atom(reader_source_t *src) {
    char token[READER_TOKEN_MAX];
    UINT32 len = 0;

    while (has_more(src) && !is_delimiter(peek(src))) {
        char c = advance(src);
        if (len < READER_TOKEN_MAX - 1) {
            token[len++] = c;
        }
    }
    token[len] = '\0';

    int negative = (len > 0 && token[0] == '-');
    int has_sign = negative || (len > 0 && token[0] == '+');
    UINT32 digit_start = has_sign ? 1 : 0;
    int all_digits = (len > digit_start);
    for (UINT32 i = digit_start; i < len; i++) {
        if (!is_digit(token[i])) {
            all_digits = 0;
            break;
        }
    }

    if (all_digits) {
        // 桁を読みながらlimb配列に逐次累積する(60bit以内に収まる値はos_make_integerが
        // 自動的にFIXNUMへ降格するため、追加のヒープ確保は発生しない)
        UINT64 limbs[READER_INT_LIMBS];
        UINT64 count = 1;
        limbs[0] = 0;
        for (UINT32 i = digit_start; i < len; i++) {
            count = mag_mul_small_add_small(limbs, count, 10, (UINT64)(token[i] - '0'));
        }
        return os_make_integer(negative, limbs, count);
    }

    if (len == 2 && token[0] == '1' && (token[1] == '+' || token[1] == '-')) {
        // ISLisp仕様(§7)で括弧無しに書ける名前として明示的に許可されているシンボル
        return os_make_symbol(token);
    }

    int has_digit = 0;
    int float_chars = (len > digit_start);
    for (UINT32 i = digit_start; i < len; i++) {
        char c = token[i];
        if (!is_float_token_char(c)) {
            float_chars = 0;
            break;
        }
        if (is_digit(c)) {
            has_digit = 1;
        }
    }
    if (float_chars && has_digit) {
        return parse_float_token(token, len, digit_start, negative);
    }

    return os_make_symbol(token);
}

/**
 * cをradix進数の数字として解釈した値(0〜radix-1)を返す。0〜9はそのまま、
 * A〜F/a〜fは10〜15として扱う。radixで表現できない数字(例: radix=2に対する'2')や
 * 数字以外の文字が来た場合は-1を返す。
 * @param c 判定する文字
 * @param radix 基数(2/8/16)
 * @return 0〜radix-1の数字の値。無効な文字なら-1
 */
static int digit_value_in_radix(char c, UINT64 radix) {
    int v;
    if (c >= '0' && c <= '9') {
        v = c - '0';
    } else if (c >= 'A' && c <= 'F') {
        v = 10 + (c - 'A');
    } else if (c >= 'a' && c <= 'f') {
        v = 10 + (c - 'a');
    } else {
        return -1;
    }
    return (v < (int)radix) ? v : -1;
}

/**
 * "#b"/"#o"/"#x"(呼び出し元で"#"とradix文字を消費済み)の残りの符号+数字列を読み、
 * radix進数の整数(FIXNUMまたはbignum)として組み立てる。符号のみ・数字なし・
 * radixで表現できない数字を含む場合はg_sym_read_errorを返す。
 * @param src 読み取り対象の文字ソース
 * @param radix 基数(2/8/16)
 * @return 読み取った整数(FIXNUMまたはbignum)。構文エラーの場合はg_sym_read_error
 */
static lisp_val_t read_radix_integer(reader_source_t *src, UINT64 radix) {
    char token[READER_TOKEN_MAX];
    UINT32 len = 0;

    while (has_more(src) && !is_delimiter(peek(src))) {
        char c = advance(src);
        if (len < READER_TOKEN_MAX - 1) {
            token[len++] = c;
        }
    }
    token[len] = '\0';

    int negative = (len > 0 && token[0] == '-');
    int has_sign = negative || (len > 0 && token[0] == '+');
    UINT32 digit_start = has_sign ? 1 : 0;

    if (len <= digit_start) {
        return g_sym_read_error; // 符号のみ、または数字が1つも無い
    }

    UINT64 limbs[READER_INT_LIMBS];
    UINT64 count = 1;
    limbs[0] = 0;
    for (UINT32 i = digit_start; i < len; i++) {
        int v = digit_value_in_radix(token[i], radix);
        if (v < 0) {
            return g_sym_read_error;
        }
        count = mag_mul_small_add_small(limbs, count, radix, (UINT64)v);
    }
    return os_make_integer(negative, limbs, count);
}

/** 文字名(SPACE/NEWLINE/TAB)の最大長。この長さを超える名前は既知の名前と一致しない */
#define CHAR_NAME_MAX 16

/**
 * name(すべて大文字で渡す既知の文字名)とbuf[0..len)を大文字小文字を無視して比較する。
 * os_make_symbol経由のinterningでは大文字化されてしまい元の大小文字が失われるため、
 * runtime.cのstrncmpignorecase(static、非公開)の代わりにreader.c内で同じ手法を使う。
 * @param buf 比較対象のバッファ(大小文字混在のまま)
 * @param len bufの長さ
 * @param name 既知の文字名(すべて大文字のNUL終端文字列)
 * @return bufとnameが大文字小文字を無視して一致すればnon-zero
 */
static int char_name_matches(const char *buf, UINT32 len, const char *name) {
    UINT32 i = 0;
    for (; name[i] != '\0'; i++) {
        if (i >= len) {
            return 0;
        }
        char c = buf[i];
        char upper = (c >= 'a' && c <= 'z') ? (char)(c - 0x20) : c;
        if (upper != name[i]) {
            return 0;
        }
    }
    return i == len;
}

/**
 * '#\' は呼び出し元で消費済みの前提で、文字リテラル(#\a や #\Space など)の残りを読む。
 * 1文字目は無条件に読む(delimiterかどうかは見ないため、#\( や #\) のような記号も1文字として扱える)。
 * 続けてdelimiterでない文字がある限り読み進め、複数文字ならSPACE/NEWLINE/TABの名前として
 * 大文字小文字を無視して照合する。os_make_symbolは大文字化してしまうため経由しない。
 * @param src 読み取り対象の文字ソース
 * @return 読み取ったCHAR。入力が終端していた、または未知の名前の場合はg_sym_read_error
 */
static lisp_val_t read_char_literal(reader_source_t *src) {
    if (!has_more(src)) {
        return g_sym_read_error;
    }
    char buf[CHAR_NAME_MAX];
    UINT32 len = 0;
    buf[len++] = advance(src);

    while (len < CHAR_NAME_MAX - 1 && has_more(src) && !is_delimiter(peek(src))) {
        buf[len++] = advance(src);
    }

    if (len == 1) {
        return os_make_char(buf[0]);
    }
    if (char_name_matches(buf, len, "SPACE")) {
        return os_make_char(' ');
    }
    if (char_name_matches(buf, len, "NEWLINE")) {
        return os_make_char('\n');
    }
    if (char_name_matches(buf, len, "TAB")) {
        return os_make_char('\t');
    }
    return g_sym_read_error;
}

/**
 * srcの読取カーソル位置から1つのS式(リスト・文字列・quote・quasiquote・unquote・アトム)を読む。
 * @param src 読み取り対象の文字ソース
 * @return 読み取ったS式。構文エラーの場合はg_sym_read_error
 */
static lisp_val_t read_expr(reader_source_t *src) {
    char c = peek(src);

    if (c == '(') {
        advance(src);
        return read_list(src);
    }
    if (c == ')') {
        // 先頭に余分な閉じ括弧がある。次回呼び出しで同じ文字に留まらないよう読み進めておく
        advance(src);
        return g_sym_read_error;
    }
    if (c == '"') {
        advance(src);
        return read_string(src);
    }
    if (c == '\'') {
        advance(src);
        skip_whitespace(src);
        if (!has_more(src)) {
            return g_sym_read_error;
        }
        lisp_val_t quoted = read_expr(src);
        if (quoted == g_sym_read_error) {
            return g_sym_read_error;
        }
        return os_make_cons(g_sym_quote, os_make_cons(quoted, nil));
    }
    if (c == '`') {
        advance(src);
        skip_whitespace(src);
        if (!has_more(src)) {
            return g_sym_read_error;
        }
        lisp_val_t quoted = read_expr(src);
        if (quoted == g_sym_read_error) {
            return g_sym_read_error;
        }
        return os_make_cons(g_sym_quasiquote, os_make_cons(quoted, nil));
    }
    if (c == ',') {
        advance(src);
        lisp_val_t sym = g_sym_unquote;
        if (has_more(src) && peek(src) == '@') {
            advance(src);
            sym = g_sym_unquote_splicing;
        }
        skip_whitespace(src);
        if (!has_more(src)) {
            return g_sym_read_error;
        }
        lisp_val_t quoted = read_expr(src);
        if (quoted == g_sym_read_error) {
            return g_sym_read_error;
        }
        return os_make_cons(sym, os_make_cons(quoted, nil));
    }
    if (c == '#') {
        advance(src);
        if (!has_more(src)) {
            return g_sym_read_error;
        }
        char c2 = peek(src);
        if (c2 == '\'') {
            advance(src);
            skip_whitespace(src);
            if (!has_more(src)) {
                return g_sym_read_error;
            }
            lisp_val_t form = read_expr(src);
            if (form == g_sym_read_error) {
                return g_sym_read_error;
            }
            return os_make_cons(g_sym_function, os_make_cons(form, nil));
        }
        if (c2 == '\\') {
            advance(src);
            return read_char_literal(src);
        }
        if (c2 == 'b' || c2 == 'B') {
            advance(src);
            return read_radix_integer(src, 2);
        }
        if (c2 == 'o' || c2 == 'O') {
            advance(src);
            return read_radix_integer(src, 8);
        }
        if (c2 == 'x' || c2 == 'X') {
            advance(src);
            return read_radix_integer(src, 16);
        }
        if (c2 == '(') {
            advance(src);
            lisp_val_t list = read_list(src);
            if (list == g_sym_read_error) {
                return g_sym_read_error;
            }
            return os_make_vector_from_list(list);
        }
        return g_sym_read_error;
    }

    return read_atom(src);
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

    reader_source_t src;
    src.peek = process_source_peek;
    src.advance = process_source_advance;
    src.has_more = process_source_has_more;
    src.ctx = proc;

    skip_whitespace(&src);

    if (!has_more(&src)) {
        // 読めるものが無い: バッファをクリアして次の行の入力を待つ
        reset_line(proc);
        return nil;
    }

    return read_expr(&src);
}

/**
 * streamから1つの完全なS式を読み取る。os_readと同じ規約に従う:
 * 読み取れるものが無い(クリーンEOF)場合はnil、構文エラーの場合はg_sym_read_errorを返す。
 * @param stream 読み取り対象のストリーム
 * @return 読み取ったS式。読めるものが無ければnil、構文エラーならg_sym_read_error
 */
lisp_val_t os_read_stream(os_stream_t *stream) {
    stream_source_ctx_t stream_ctx;
    stream_ctx.stream = stream;
    stream_ctx.has_lookahead = 0;

    reader_source_t src;
    src.peek = stream_source_peek;
    src.advance = stream_source_advance;
    src.has_more = stream_source_has_more;
    src.ctx = &stream_ctx;

    skip_whitespace(&src);

    if (!has_more(&src)) {
        return nil;
    }

    return read_expr(&src);
}

/**
 * valが数値(fixnum/bignum/float)かどうかを、runtime.hが公開するタグ/MAGIC定数を使って判定する。
 * is_bignum/is_floatはruntime.cのstaticヘルパーで参照できないため、ここで同等の判定を行う。
 * @param val 判定対象の値
 * @return 数値なら非0、そうでなければ0
 */
static int is_number_result(lisp_val_t val) {
    if ((val & TAG_MASK) == TAG_FIXNUM) {
        return 1;
    }
    if ((val & TAG_MASK) != TAG_INSTANCE) {
        return 0;
    }
    UINT64 magic = ((UINT64 *)(val & ~TAG_MASK))[0];
    return magic == MAGIC_BIGNUM || magic == MAGIC_FLOAT;
}

lisp_val_t os_parse_number(lisp_val_t str, lisp_val_t env) {
    char buf[READER_TOKEN_MAX];
    os_string_to_cstr(str, buf, sizeof(buf));
    UINT64 len = 0;
    while (buf[len]) {
        len++;
    }

    string_source_ctx_t str_ctx;
    str_ctx.buf = buf;
    str_ctx.len = len;
    str_ctx.pos = 0;

    reader_source_t src;
    src.peek = string_source_peek;
    src.advance = string_source_advance;
    src.has_more = string_source_has_more;
    src.ctx = &str_ctx;

    lisp_val_t result = has_more(&src) ? read_expr(&src) : g_sym_read_error;

    if (result != g_sym_read_error && !has_more(&src) && is_number_result(result)) {
        return result;
    }

    lisp_val_t number_class = os_resolve_class(g_sym_class_number, env);
    if (number_class == g_sym_eval_error || os_is_control_transfer(number_class)) {
        return number_class;
    }
    lisp_val_t initargs = os_make_cons(g_sym_kw_string, os_make_cons(str,
        os_make_cons(g_sym_kw_expected_class, os_make_cons(number_class, nil))));
    return os_signal_condition(g_sym_class_parse_error, initargs, env);
}
