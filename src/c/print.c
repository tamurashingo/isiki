#include "print.h"
#include "lisp.h"
#include "framebuffer.h"

static void print_value(os_char_sink_t *sink, lisp_val_t val, int escaped);

static void sink_write_char(os_char_sink_t *sink, UINT8 c) {
    sink->write_char(sink->ctx, c);
}

static void sink_write_string(os_char_sink_t *sink, const char *s) {
    while (*s != '\0') {
        sink_write_char(sink, (UINT8)*s);
        s++;
    }
}

/**
 * UINT64を10進数の数字列にして出力する(標準ライブラリのitoa等は使えないため自前実装)。
 * @param sink 出力先のシンク
 * @param value 出力する値
 */
static void print_fixnum(os_char_sink_t *sink, UINT64 value) {
    if (value == 0) {
        sink_write_char(sink, '0');
        return;
    }
    char digits[20]; // UINT64の最大10進桁数
    int len = 0;
    while (value > 0) {
        digits[len++] = '0' + (char)(value % 10);
        value /= 10;
    }
    while (len > 0) {
        sink_write_char(sink, (UINT8)digits[--len]);
    }
}

/**
 * bignum(MAGIC_BIGNUM)のlimb配列を10進数で出力する。limb配列はmag_divmod_smallで
 * 破壊的に10で割り続けるため、出力前に別バッファへコピーしておく。
 * @param sink 出力先のシンク
 * @param val 出力するbignum(TAG_INSTANCE、word0==MAGIC_BIGNUM)
 */
static void print_bignum(os_char_sink_t *sink, lisp_val_t val) {
    UINT64 *obj = (UINT64 *)(val & ~TAG_MASK);
    UINT64 sign = obj[1];
    UINT64 count = obj[2];
    UINT64 *src = (UINT64 *)obj[3];

    // [GC安全性] workとdigitsを別々にos_alloc_rawすると、2回目の確保がGCを誘発した
    // 時点で1回目のworkが無効になる。os_alloc_rawで取る生バッファはGCのコピー対象
    // ではないため、GC後も旧From空間に残ったまま「上書きされるまで」生き延びている
    // だけであり、from-spaceが再利用された時点で内容が壊れる
    // (documents/pitfalls.md 原則7)。1回の確保にまとめ、確保後は一切確保を
    // 伴わない形にすることで解消する(sink_write_charは関数ポインタ経由の出力のみで
    // 確保を伴わない)。
    // 1limb(基数2^32)あたり最大10進10桁(log10(2^32) < 9.63)なので10*countで十分
    UINT8 *scratch = (UINT8 *)os_alloc_raw(8 * count + 10 * count);
    UINT64 *work = (UINT64 *)scratch;
    char *digits = (char *)(scratch + 8 * count);
    for (UINT64 i = 0; i < count; i++) {
        work[i] = src[i];
    }

    if (sign) {
        sink_write_char(sink, '-');
    }

    int len = 0;
    while (count > 1 || work[0] != 0) {
        UINT64 rem;
        count = mag_divmod_small(work, count, 10, &rem);
        digits[len++] = '0' + (char)rem;
    }
    while (len > 0) {
        sink_write_char(sink, (UINT8)digits[--len]);
    }
}

/** doubleを可逆に10進へ落とせる有効桁数。printする桁数の上限でもある */
#define FLOAT_SIG_DIGITS_MAX 17

/** binary32(single-float)を可逆に10進へ落とせる有効桁数(=探索の上限)。
 *  doubleと同じ17桁で出すとbinary32の丸め誤差が見えてしまうので、single側はこれを使う。
 *  実際に出す桁数は「同じ値に読み戻せる最短」を1〜9で探して決める
 *  (print_double_with_marker の shortest_single)。 */
#define SINGLE_FLOAT_SIG_DIGITS 9

/**
 * 10進の桁列と10進指数から値を組み立てる。
 * **reader.c の parse_float_token とまったく同じ手順**(仮数を逐次累積してから
 * 10のべきを掛け/割る)にしてある。ここが食い違うと、printした文字列を
 * 読み直したときに別の値になる。
 * @param digits 上位から並んだ10進の桁('0'〜'9')
 * @param count 使う桁数
 * @param exp10 digits[0]の位(1<=|x|/10^exp10<10 となる指数)
 * @return 組み立てたdouble値(符号なし)
 */
static double digits_to_double(const char *digits, int count, int exp10) {
    double m = 0.0;
    for (int i = 0; i < count; i++) {
        m = m * 10.0 + (double)(digits[i] - '0');
    }
    int e = exp10 - (count - 1);
    if (e > 0) {
        for (int i = 0; i < e; i++) { m *= 10.0; }
    } else {
        for (int i = 0; i < -e; i++) { m /= 10.0; }
    }
    return m;
}

/**
 * doubleをISLisp §19.2相当の10進表記でsinkへ出力する(strtod/printf系が無い前提の
 * 手書き実装)。符号・0を特別扱いした後、1<=|x|/10^e<10となる10進指数eを求め、
 * 仮数から有効sig_digits桁を上位から順に取り出し、末尾の0を(小数点以下最低1桁を
 * 残して)取り除く。指数の大小で固定小数点表記("123.456")かE表記("1.23456E20")かを
 * 切り替える。
 *
 * 指数マーカーの文字と「指数部を必ず出すか」を引数に取るのは、
 * *read-default-float-format* と型が一致しないfloatへ接尾辞を付けるためである
 * (os_print_float_to_sink)。
 * exp_marker='E' / force_exponent=0 / sig_digits=FLOAT_SIG_DIGITS_MAX /
 * shortest_single=0 が、single-float導入前とまったく同じ出力になる組み合わせ。
 * @param sink 出力先のシンク
 * @param value 出力するdouble値
 * @param exp_marker 指数部の前に置く文字('E' / 'f' / 'd')
 * @param force_exponent 非0なら固定小数点表記のときも "<marker>0" を末尾に補う
 * @param sig_digits 取り出す有効桁数(固定小数点表記とE表記の切り替え閾値も兼ねる)
 * @param shortest_single 非0なら「同じfloatに読み戻せる最短の桁数」まで切り詰める
 */
static void print_double_with_marker(os_char_sink_t *sink, double value,
                                     char exp_marker, int force_exponent, int sig_digits,
                                     int shortest_single) {
    if (value != value) {
        sink_write_string(sink, "NAN");
        return;
    }

    int negative = 0;
    if (value < 0.0) {
        negative = 1;
        value = -value;
    }
    if (negative) {
        sink_write_char(sink, '-');
    }

    if (value == 0.0) {
        sink_write_string(sink, "0.0");
        return;
    }

    // value == +infになるケース(±1.7976931348623157E308を超える等)はinfとして扱う
    double check_inf = value * 10.0;
    if (check_inf == value && value > 1.0) {
        sink_write_string(sink, "INF");
        return;
    }

    /* 正規化で value を書き換えるので、往復判定に使う元の絶対値を控えておく */
    const double abs_value = value;

    int exp10 = 0;
    while (value >= 10.0) {
        value /= 10.0;
        exp10++;
    }
    while (value < 1.0) {
        value *= 10.0;
        exp10--;
    }

    char digits[FLOAT_SIG_DIGITS_MAX];
    double m = value;
    for (int i = 0; i < sig_digits; i++) {
        int digit = (int)m;
        if (digit > 9) {
            digit = 9; // 浮動小数点誤差で10になるのを防ぐ
        }
        digits[i] = (char)('0' + digit);
        m = (m - digit) * 10.0;
    }

    int ndigits = sig_digits;
    while (ndigits > 1 && digits[ndigits - 1] == '0') {
        ndigits--;
    }

    /* [single-float] 末尾の0を落とすだけでは足りない。binary32の刻みは粗いので、
       可逆に必要な9桁を出すと "1.5E20" が "1.50000002E20" になってしまう。
       そこで**同じ値に読み戻せる最短の桁数**を探す。判定はdigits_to_double
       (reader.cと同じ組み立て)を通すので、「printした文字列をこのリーダで
       読み直すと元のfloatに戻る」ことがそのまま保証される。
       doubleには適用しない — 既存の出力を変えないため。 */
    if (shortest_single) {
        const float target = (float)abs_value;
        for (int k = 1; k <= sig_digits; k++) {
            if ((float)digits_to_double(digits, k, exp10) == target) {
                ndigits = k;
                break;
            }
        }
    }

    if (exp10 >= -3 && exp10 < sig_digits) {
        // 固定小数点表記
        if (exp10 >= 0) {
            for (int i = 0; i <= exp10; i++) {
                sink_write_char(sink, (UINT8)((i < ndigits) ? digits[i] : '0'));
            }
            sink_write_char(sink, '.');
            if (exp10 + 1 >= ndigits) {
                sink_write_char(sink, '0');
            } else {
                for (int i = exp10 + 1; i < ndigits; i++) {
                    sink_write_char(sink, (UINT8)digits[i]);
                }
            }
        } else {
            sink_write_string(sink, "0.");
            for (int i = 0; i < -exp10 - 1; i++) {
                sink_write_char(sink, '0');
            }
            for (int i = 0; i < ndigits; i++) {
                sink_write_char(sink, (UINT8)digits[i]);
            }
        }
        if (force_exponent) {
            /* 固定小数点表記には指数部が無いので、型を表すために0指数を補う
               ("1.5" → "1.5d0")。これが無いと読み直したときに型が変わる */
            sink_write_char(sink, (UINT8)exp_marker);
            sink_write_char(sink, '0');
        }
    } else {
        // E表記
        sink_write_char(sink, (UINT8)digits[0]);
        sink_write_char(sink, '.');
        if (ndigits == 1) {
            sink_write_char(sink, '0');
        } else {
            for (int i = 1; i < ndigits; i++) {
                sink_write_char(sink, (UINT8)digits[i]);
            }
        }
        sink_write_char(sink, (UINT8)exp_marker);
        if (exp10 < 0) {
            sink_write_char(sink, '-');
            exp10 = -exp10;
        }
        print_fixnum(sink, (UINT64)exp10);
    }
}

void os_print_double_to_sink(os_char_sink_t *sink, double value) {
    /* 型情報を持たない生のdoubleなので、接尾辞は付けようがない。
       single-float導入前とまったく同じ出力になる */
    print_double_with_marker(sink, value, 'E', 0, FLOAT_SIG_DIGITS_MAX, 0);
}

void os_print_float_to_sink(os_char_sink_t *sink, lisp_val_t val) {
    int is_single = os_is_single_float(val);
    int default_is_single = os_read_default_float_format_is_single();
    /* [桁数] single-floatをdoubleと同じ17桁で出すと、binary32の丸め誤差が
       そのまま見える("1.5E20" が "1.50000002024002560E20" になる)。
       binary32を可逆に表せるのは10進9桁なので、single側は9桁で打ち切る。 */
    int sig = is_single ? SINGLE_FLOAT_SIG_DIGITS : FLOAT_SIG_DIGITS_MAX;
    if (is_single == default_is_single) {
        /* 既定の型と一致 → 接尾辞なし(従来の出力) */
        print_double_with_marker(sink, os_float_value(val), 'E', 0, sig, is_single);
        return;
    }
    /* 既定と違う型 → 指数マーカーで型を明示する。小文字にするのは
       ISLispのE表記(大文字)と「型を表す接尾辞」を見た目で区別するため */
    print_double_with_marker(sink, os_float_value(val), is_single ? 'f' : 'd', 1, sig, is_single);
}

/**
 * floatを *read-default-float-format* に応じた接尾辞付きで出力する。
 * @param sink 出力先のシンク
 * @param val 出力するfloat(TAG_SINGLE_FLOATの即値、またはword0==MAGIC_FLOATのINSTANCE)
 */
static void print_float(os_char_sink_t *sink, lisp_val_t val) {
    os_print_float_to_sink(sink, val);
}

/**
 * STRINGオブジェクトのレイアウト([len(8byte)][chars...])に従ってバイト列を出力する。
 * @param sink 出力先のシンク
 * @param str_addr STRINGオブジェクト本体(タグを除いた先頭)のアドレス
 */
static void print_bytes(os_char_sink_t *sink, lisp_addr_t str_addr) {
    UINT64 len = ((UINT64 *)str_addr)[0];
    const char *bytes = (const char *)(str_addr + 8);
    for (UINT64 i = 0; i < len; i++) {
        sink_write_char(sink, (UINT8)bytes[i]);
    }
}

/**
 * SYMBOLの名前を出力する。
 * @param sink 出力先のシンク
 * @param val 出力するSYMBOL
 */
static void print_symbol(os_char_sink_t *sink, lisp_val_t val) {
    lisp_val_t name_str = ((lisp_val_t *)(val & ~TAG_MASK))[0];
    print_bytes(sink, name_str & ~TAG_MASK);
}

/**
 * STRINGを出力する。escapedが真ならダブルクオートで囲む(prin1相当)、
 * 偽なら内容のみをそのまま出力する(princ相当)。
 * @param sink 出力先のシンク
 * @param val 出力するSTRING
 * @param escaped ダブルクオートで囲むかどうか
 */
static void print_string(os_char_sink_t *sink, lisp_val_t val, int escaped) {
    if (!escaped) {
        print_bytes(sink, val & ~TAG_MASK);
        return;
    }
    // prin1相当: ISLisp仕様§24の通り、要素の " と \ はバックスラッシュでエスケープする
    lisp_addr_t addr = val & ~TAG_MASK;
    UINT64 len = ((UINT64 *)addr)[0];
    const UINT8 *bytes = (const UINT8 *)(addr + 8);
    sink_write_char(sink, '"');
    for (UINT64 i = 0; i < len; i++) {
        if (bytes[i] == '"' || bytes[i] == '\\') {
            sink_write_char(sink, '\\');
        }
        sink_write_char(sink, bytes[i]);
    }
    sink_write_char(sink, '"');
}

/**
 * CONSのリストを"(a b c)"形式で出力する。nilで終端しない場合はドット対記法で表示する。
 * @param sink 出力先のシンク
 * @param val 出力するCONS
 * @param escaped 要素の出力にprin1/princどちらの規則を使うか
 */
static void print_list(os_char_sink_t *sink, lisp_val_t val, int escaped) {
    sink_write_char(sink, '(');
    lisp_val_t current = val;
    int first = 1;
    while (current != nil && (current & TAG_MASK) == TAG_CONS) {
        if (!first) {
            sink_write_char(sink, ' ');
        }
        first = 0;
        print_value(sink, cc_car(current), escaped);
        current = cc_cdr(current);
    }
    if (current != nil) {
        // 末尾がnilで終わらないconsはドット対記法で表示する
        sink_write_string(sink, " . ");
        print_value(sink, current, escaped);
    }
    sink_write_char(sink, ')');
}

/**
 * VECTORを"#(a b c)"形式で出力する。多次元の場合も次元の区切りは付けず、
 * 要素本体をrow-major順にフラットに並べて表示する。
 * @param sink 出力先のシンク
 * @param val 出力するVECTOR
 * @param escaped 要素の出力にprin1/princどちらの規則を使うか
 */
/** rank次元配列のlevel次元目以降をネストした括弧で出力する(print_vectorの再帰本体)。
 * dataは行優先の要素データ、*posは次に出力する要素の位置 */
static void print_array_level(os_char_sink_t *sink, const lisp_val_t *header, UINT64 rank, UINT64 level,
                              const lisp_val_t *data, UINT64 *pos, int escaped) {
    if (level == rank) {
        print_value(sink, data[(*pos)++], escaped);
        return;
    }
    UINT64 n = header[1 + level];
    sink_write_char(sink, '(');
    for (UINT64 i = 0; i < n; i++) {
        if (i != 0) {
            sink_write_char(sink, ' ');
        }
        print_array_level(sink, header, rank, level + 1, data, pos, escaped);
    }
    sink_write_char(sink, ')');
}

static void print_vector(os_char_sink_t *sink, lisp_val_t val, int escaped) {
    lisp_val_t *header = os_vector_header(val);
    UINT64 rank = header[0];
    lisp_val_t *data = header + 1 + rank;
    UINT64 pos = 0;

    if (rank == 1) {
        // general-vectorはISLisp仕様§23の #(x1 x2 ...) 表記
        sink_write_char(sink, '#');
        print_array_level(sink, header, rank, 0, data, &pos, escaped);
        return;
    }
    // それ以外はISLisp仕様§22の #nA(...) 表記(rank 0は #0A obj)
    sink_write_char(sink, '#');
    char digits[24];
    int nd = 0;
    UINT64 r = rank;
    do {
        digits[nd++] = (char)('0' + (r % 10));
        r /= 10;
    } while (r != 0);
    while (nd > 0) {
        sink_write_char(sink, (UINT8)digits[--nd]);
    }
    sink_write_char(sink, 'A');
    if (rank == 0) {
        sink_write_char(sink, ' ');
    }
    print_array_level(sink, header, rank, 0, data, &pos, escaped);
}

/**
 * valをTAGに応じて出力する(os_print_to_sinkの実処理本体)。
 * @param sink 出力先のシンク
 * @param val 出力するLisp値
 * @param escaped STRINGをprin1相当(ダブルクオート付き)で出力するかprinc相当で出力するか
 */
static void print_value(os_char_sink_t *sink, lisp_val_t val, int escaped) {
    if (val == nil) {
        // NILはcar/cdrが自分自身を指す循環consのため、専用処理せず
        // TAG_CONSの分岐に入ると無限再帰するので先に判定する
        sink_write_string(sink, "NIL");
        return;
    }

    switch (val & TAG_MASK) {
        case TAG_FIXNUM:
            if (os_fixnum_is_negative(val)) {
                sink_write_char(sink, '-');
            }
            print_fixnum(sink, os_fixnum_magnitude(val));
            return;
        case TAG_SYMBOL:
            print_symbol(sink, val);
            return;
        case TAG_STRING:
            print_string(sink, val, escaped);
            return;
        case TAG_CHAR: {
            // prin1相当(escaped)ではISLisp仕様§20の文字リテラル表記 #\x で出力する。
            // 名前を持つ文字(space/newline/tab)は名前で出す
            UINT8 ch = (UINT8)(val >> CHAR_VALUE_SHIFT);
            if (!escaped) {
                sink_write_char(sink, ch);
                return;
            }
            sink_write_string(sink, "#\\");
            if (ch == ' ') {
                sink_write_string(sink, "space");
            } else if (ch == '\n') {
                sink_write_string(sink, "newline");
            } else if (ch == '\t') {
                sink_write_string(sink, "tab");
            } else {
                sink_write_char(sink, ch);
            }
            return;
        }
        case TAG_SINGLE_FLOAT:
            print_float(sink, val);
            return;
        case TAG_CONS:
            print_list(sink, val, escaped);
            return;
        case TAG_INSTANCE: {
            UINT64 *obj = (UINT64 *)(val & ~TAG_MASK);
            UINT64 magic = obj[0];
            if (magic == MAGIC_PROCESS) {
                sink_write_string(sink, "#<PROCESS>");
            } else if (magic == MAGIC_BIGNUM) {
                print_bignum(sink, val);
            } else if (magic == MAGIC_VECTOR) {
                print_vector(sink, val, escaped);
            } else if (magic == MAGIC_FLOAT) {
                print_float(sink, val);
            } else if (magic == MAGIC_STREAM) {
                sink_write_string(sink, "#<STREAM>");
            } else if (magic == MAGIC_BUILTIN_CLASS || magic == MAGIC_STANDARD_CLASS) {
                sink_write_string(sink, "#<CLASS ");
                print_value(sink, obj[1], escaped);
                sink_write_char(sink, '>');
            } else if (magic == MAGIC_CLASS_INSTANCE) {
                UINT64 *cls = (UINT64 *)(obj[1] & ~TAG_MASK);
                sink_write_string(sink, "#<INSTANCE-OF ");
                print_value(sink, cls[1], escaped);
                sink_write_char(sink, '>');
            } else if (magic == MAGIC_FUNCTION_NATIVE) {
                sink_write_string(sink, obj[2] != nil ? "#<FUNCTION-COMPILED>" : "#<FUNCTION-BUILTIN>");
            } else if (magic == MAGIC_FUNCTION_INTERPRETED) {
                sink_write_string(sink, "#<FUNCTION-INTERPRETED>");
            } else {
                sink_write_string(sink, "#<FUNCTION>");
            }
            return;
        }
    }
}

void os_print_to_sink(lisp_val_t val, os_char_sink_t *sink, int escaped) {
    print_value(sink, val, escaped);
}

static void frame_buffer_sink_write_char(void *ctx, UINT8 c) {
    frame_buffer *fb = (frame_buffer *)ctx;
    fb->write_char(fb, c);
}

lisp_val_t os_print(lisp_val_t val, frame_buffer *fb) {
    os_char_sink_t sink = { .ctx = fb, .write_char = frame_buffer_sink_write_char };
    print_value(&sink, val, 1);
    return val;
}
