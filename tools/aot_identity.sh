#!/bin/bash
# [同一性] AOT 生成物が「1 文字も変わっていない」ことを、**生成しなおして
# byte 比較する**ことで確かめる。
#
# なぜ要るか。$(LISP_COMPILED)(src/c/lisp_compiled.c)と
# $(LISP_COMPILED_FIXTURE)(test/c/lisp_compiled_fixture.c)は .gitignore 済みの
# **ビルド生成物**で、git には 1 バイトも入っていない。だから
# 「C 側を変えたが AOT 生成物は 1 文字も変わっていない」を言おうとしても
# git diff には何も出ない — **出ないことが根拠にならない**
# (documents/pitfalls.md 原則6 の形。黙って変わっても誰も気づかない)。
# 比較するには「基準を退避 → 生成しなおす → byte 比較」を自分でやるしかない。
#
# 使い方:
#   make aot-identity-save      変更前に基準を退避する
#   (ここで C 側を変更する)
#   make aot-identity-check     生成しなおして基準と比べる
#   make aot-identity-positive-control
#                               **本物の生成器**で陽性対照を取る(規則 8)
#   make aot-identity-self-test 検出器の自己試験(docker も QEMU も要らない)
#
# 終了コード:
#   0  同一(1 バイトも違わない)
#   1  相違(**これが検出。**生成物が実際に変わったなら mtime も新しいまま残るので
#      カーネルは作り直される — 中身が変わったのならそれが正しい)
#   2  比較が成立しない(基準が無い / ビルドの印が違う / 生成に失敗)
#
# [罠1] save も check も、生成物を**消してから**生成しなおす。$(LISP_COMPILED) は
#   $(TRANSPILE_LISP_SRC) へのファイル依存なので、入力の Lisp を触っていなければ
#   make transpile は何もしない。消さずに比べると「生成器を動かしていないのに
#   同一」という無意味な 0 が返る。
#   **save で生成しなおすのは、ディスクに残っていた生成物を基準にしないため。**
#   別のブランチで生成されたものが残っていても mtime が入力より新しければ
#   make は作り直さないので、そのまま退避すると**別の入力から出た生成物を
#   基準にしてしまい、check が理由なく相違を報告する。**
#
# [罠2] 生成しなおしたあと**mtime を元に戻す**。戻さないとカーネル(と
#   BOOT_FAT32_IMG)がまるごと作り直される。比較のためだけに 10 分以上を払う。
#   戻す判定は「**ディスクにあったものと同じ内容が再生成されたか**」で行う。
#   基準と同じかどうかでは決めない — make が作り直すかを決めるのは
#   ディスクの内容の変化だけで、基準は判定(同一/相違)の側の話である。
#   生成物が実際に変わったときは新しい mtime を残す(作り直されるのが正しい)。
#
# [罠3] ビルドの印($(BUILD_FLAG_SIGNATURE))が違う基準と比べてはならない。
#   Makefile:91 付近の GC_DEBUG_STAMP と同じ理由で、印は退避時に記録して
#   check で突き合わせる。違えば終了コード 2 で止める(黙って比べない)。
#   トランスパイラ自体はこれらの C 側フラグを読まないが、Makefile が
#   「新しいビルドフラグは必ず署名に入れる」を例外なしの規則にしているのと
#   同じ理由で、例外を作らない。
#
# 環境変数(既定で十分。自己試験が差し替えるためにある):
#   AOT_IDENTITY_DIR        基準の置き場(既定 tmp/identity/aot)。
#                           **tmp/ は make clean で消える。** clean を挟む予定が
#                           あるなら別の場所を指すこと
#   AOT_IDENTITY_FILES      比較する生成物(空白区切り)
#   AOT_IDENTITY_GENERATE   生成コマンド(既定 make transpile)
#   AOT_IDENTITY_INPUTS     生成器の入力一覧を出すコマンド。空なら記録しない
#   AOT_IDENTITY_SIGNATURE  ビルドの印。未設定なら make から取る
set -eu

IDENT_DIR=${AOT_IDENTITY_DIR:-tmp/identity/aot}
FILES=${AOT_IDENTITY_FILES:-src/c/lisp_compiled.c test/c/lisp_compiled_fixture.c}
GENERATE=${AOT_IDENTITY_GENERATE:-make --no-print-directory transpile}
INPUTS_CMD=${AOT_IDENTITY_INPUTS-make --no-print-directory print-transpile-src}
GC_DEBUG_STAMP=${GC_DEBUG_STAMP:-tmp/.gc-debug-flags}

META="$IDENT_DIR/meta.txt"
SAVED="$IDENT_DIR/files"
SELF=$(cd "$(dirname "$0")" && pwd)/$(basename "$0")

now() { date +%s.%N; }
since() { awk -v a="$1" -v b="$2" 'BEGIN{printf "%.2f", b-a}'; }
hash_of() { sha256sum "$1" | cut -d' ' -f1; }

# $(BUILD_FLAG_SIGNATURE) は空のフラグ変数のぶんだけ空白が残る(既定ビルドは
# "    "、GC_DEBUG=1 なら "-DISIKIOS_GC_DEBUG   " のような形)。
# **空白をそのまま持ち回ると引数に渡したときに分割される**ので詰めてから使う
normalize_sig() {
    printf '%s' "$1" | tr -s ' ' ' ' | sed -e 's/^ *//' -e 's/ *$//'
}

# ビルドの印。未設定のときだけ make へ聞く(= Makefile の BUILD_FLAG_SIGNATURE が
# 唯一の真実源。この口を通すので、呼び出し側が手で書き写して間違える余地が無い)
signature() {
    if [ "${AOT_IDENTITY_SIGNATURE+set}" = set ]; then
        normalize_sig "$AOT_IDENTITY_SIGNATURE"
    else
        normalize_sig "$(make -s print-build-flag-signature)"
    fi
}

run_generator() {
    local t0 t1
    t0=$(now)
    if ! sh -c "$GENERATE" > /dev/null; then
        echo "ERROR: 生成コマンドが失敗した: $GENERATE" >&2
        return 2
    fi
    # [陽性対照] 生成の直後に差を入れる口。positive-control だけが使う。
    # **生成器を本当に動かしたうえで「1 文字違うものが出てきた」状態を作る**
    if [ -n "${AOT_IDENTITY_INJECT_AFTER_GENERATE:-}" ]; then
        sh -c "$AOT_IDENTITY_INJECT_AFTER_GENERATE"
    fi
    t1=$(now)
    GEN_SECONDS=$(since "$t0" "$t1")
    return 0
}

# ファイルの先頭 1 バイトだけを別の文字に書き換える。
# **「1 文字でも違えば落ちる」を本物の生成物で示すために使う**
inject_one_byte() {
    local f=$1 cur new
    cur=$(dd if="$f" bs=1 count=1 2>/dev/null | od -An -c | tr -d ' \n')
    if [ "$cur" = "A" ]; then new=B; else new=A; fi
    printf '%s' "$new" | dd of="$f" bs=1 seek=0 conv=notrunc status=none
    echo "    $f の先頭 1 バイトを [$cur] -> [$new] に書き換えた"
}

# [罠1][罠2] 生成物を消して生成しなおし、**ディスクにあったものと byte 一致した
# ものだけ mtime を戻す**。save と check で同じ手順を踏むので、
# 「save のときだけ生成器が動いていなかった」が起きない
regenerate_preserving_mtime() {
    local f b keep
    keep="$IDENT_DIR/before"
    rm -rf "$keep"
    mkdir -p "$keep"
    for f in $FILES; do
        if [ -f "$f" ]; then
            cp -p "$f" "$keep/$(basename "$f")"
        fi
    done

    echo '--- 生成物を消して生成しなおす(消さないと make transpile は何もしない)'
    rm -f $FILES
    run_generator || return 2
    for f in $FILES; do
        if [ ! -f "$f" ]; then
            echo "ERROR: 生成しても $f が出てこない" >&2
            return 2
        fi
    done

    MTIME_RESTORED=0
    MTIME_KEPT_NEW=0
    MTIME_LINES=""
    for f in $FILES; do
        b="$keep/$(basename "$f")"
        if [ -f "$b" ] && cmp -s "$b" "$f"; then
            MTIME_LINES="$MTIME_LINES
    $f : $(stat -c '%y' "$f" | cut -c1-23) -> $(stat -c '%y' "$b" | cut -c1-23) (戻した)"
            touch -r "$b" "$f"
            MTIME_RESTORED=$((MTIME_RESTORED + 1))
        elif [ -f "$b" ]; then
            MTIME_LINES="$MTIME_LINES
    $f : 内容が変わったので新しい mtime を残す(**作り直される**)"
            MTIME_KEPT_NEW=$((MTIME_KEPT_NEW + 1))
        else
            MTIME_LINES="$MTIME_LINES
    $f : 比較前に無かったので戻す先が無い(新しい mtime のまま)"
            MTIME_KEPT_NEW=$((MTIME_KEPT_NEW + 1))
        fi
    done
    rm -rf "$keep"
    return 0
}

# 相違が出たときの切り分け。**入力の Lisp が変わっていれば生成物が変わるのは
# 当たり前**で、それは検出したいものではない。逆に入力が 1 件も変わっていない
# のに生成物が変わったなら、原因は C 側の変更かトランスパイラの非決定性である
report_input_changes() {
    local f was nowh changed=0 total=0
    if [ -z "$INPUTS_CMD" ] || ! grep -q '^input' "$META"; then
        return 0
    fi
    echo "--- 生成器の入力の変化(差の原因の切り分け)"
    for f in $(sh -c "$INPUTS_CMD"); do
        [ -f "$f" ] || continue
        total=$((total + 1))
        was=$(sed -n "s|^input\t$f\t||p" "$META")
        nowh=$(hash_of "$f")
        if [ -n "$was" ] && [ "$was" != "$nowh" ]; then
            echo "    変わった入力: $f"
            changed=$((changed + 1))
        elif [ -z "$was" ]; then
            echo "    基準に記録の無い入力: $f (顔ぶれが増えている)"
            changed=$((changed + 1))
        fi
    done
    if [ "$changed" -eq 0 ]; then
        echo "    入力 $total 件は**すべて基準と同じ**。"
        echo "    => 生成物が変わった原因は入力ではない。C 側の変更か、"
        echo "       トランスパイラが決定的でないかのどちらかである"
    else
        echo "    入力が $changed/$total 件変わっている。"
        echo "    => 生成物が変わるのは当たり前。**この比較では何も言えない。**"
        echo "       入力を戻すか、make aot-identity-save を取り直すこと"
    fi
}

cmd_save() {
    local t0 sig f n=0
    t0=$(now)
    sig=$(signature)

    mkdir -p "$IDENT_DIR"
    regenerate_preserving_mtime || return 2

    rm -rf "$IDENT_DIR"
    mkdir -p "$SAVED"
    {
        echo "# AOT 生成物の同一性比較の基準"
        printf 'saved-at\t%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
        printf 'git-head\t%s\n' "$(git rev-parse --short HEAD 2>/dev/null || echo unknown)"
        printf 'signature\t%s\n' "$sig"
        if [ -f "$GC_DEBUG_STAMP" ]; then
            printf 'gc-debug-stamp\t%s\n' "$(cat "$GC_DEBUG_STAMP")"
        else
            printf 'gc-debug-stamp\t%s\n' "(無し)"
        fi
        printf 'files\t%s\n' "$FILES"
        for f in $FILES; do
            printf 'artifact\t%s\t%s\t%s\n' "$f" "$(wc -c < "$f")" "$(hash_of "$f")"
        done
        if [ -n "$INPUTS_CMD" ]; then
            for f in $(sh -c "$INPUTS_CMD"); do
                if [ -f "$f" ]; then
                    printf 'input\t%s\t%s\n' "$f" "$(hash_of "$f")"
                fi
            done
        fi
    } > "$META"
    for f in $FILES; do
        cp -p "$f" "$SAVED/$(basename "$f")"
        n=$((n + 1))
    done

    echo "=== AOT 生成物の基準を退避した ==="
    echo "    置き場     : $IDENT_DIR  (**make clean で消える**)"
    echo "    ビルドの印 : [$sig]"
    grep '^artifact' "$META" | while IFS="$(printf '\t')" read -r _ path size h; do
        printf '    %-34s %9s byte  sha256=%s\n' "$path" "$size" "$(printf '%s' "$h" | cut -c1-16)…"
    done
    echo "    入力の記録 : $(grep -c '^input' "$META" || true) 件"
    echo "    退避件数   : $n"
    echo "    mtime      : 戻した $MTIME_RESTORED 件 / 新しいまま $MTIME_KEPT_NEW 件$MTIME_LINES"
    echo "    所要       : $(since "$t0" "$(now)") 秒${GEN_SECONDS:+ (うち生成 $GEN_SECONDS 秒)}"
    echo
    echo "次: C 側を変更したあと  make aot-identity-check"
    return 0
}

cmd_check() {
    local t0 sig base_sig f b rc=0 n=0 diffs=0
    t0=$(now)
    sig=$(signature)

    if [ ! -f "$META" ]; then
        echo "ERROR: 基準が無い($META)。先に make aot-identity-save を実行すること。" >&2
        echo "       (tmp/ は make clean で消えるので、clean を挟んだなら取り直し)" >&2
        return 2
    fi
    base_sig=$(sed -n 's/^signature\t//p' "$META")
    if [ "$sig" != "$base_sig" ]; then
        echo "ERROR: ビルドの印が基準と違う。**違う設定で取った基準と比べてはならない**" >&2
        echo "       基準: [$base_sig]" >&2
        echo "       いま: [$sig]" >&2
        echo "       同じ変数で呼び直すか、make aot-identity-save を取り直すこと" >&2
        return 2
    fi
    for f in $FILES; do
        if [ ! -f "$SAVED/$(basename "$f")" ]; then
            echo "ERROR: 基準に $(basename "$f") が入っていない(比較する顔ぶれが違う)" >&2
            return 2
        fi
    done

    regenerate_preserving_mtime || return 2

    echo
    echo "=== AOT 生成物の同一性比較 ==="
    echo "    ビルドの印 : [$sig]  (基準と一致)"
    echo "    基準       : $(sed -n 's/^saved-at\t//p' "$META") / git $(sed -n 's/^git-head\t//p' "$META")"
    echo
    printf '    | %-34s | %9s | %9s | %s\n' "生成物" "基準byte" "今回byte" "判定"
    printf '    |-%-34s-|-%9s-|-%9s-|------\n' "----------------------------------" "---------" "---------"
    for f in $FILES; do
        n=$((n + 1))
        b="$SAVED/$(basename "$f")"
        if [ ! -f "$f" ]; then
            printf '    | %-34s | %9s | %9s | %s\n' "$f" "$(wc -c < "$b")" "-" "**生成されなかった**"
            diffs=$((diffs + 1))
            continue
        fi
        if cmp -s "$b" "$f"; then
            printf '    | %-34s | %9s | %9s | %s\n' "$f" "$(wc -c < "$b")" "$(wc -c < "$f")" "同一"
        else
            printf '    | %-34s | %9s | %9s | %s\n' "$f" "$(wc -c < "$b")" "$(wc -c < "$f")" "**相違**"
            diffs=$((diffs + 1))
        fi
    done
    echo

    echo "--- mtime の扱い(戻さないとカーネルと BOOT_FAT32_IMG が作り直される)"
    echo "    戻した $MTIME_RESTORED 件 / 新しいまま $MTIME_KEPT_NEW 件$MTIME_LINES"
    echo

    if [ "$diffs" -eq 0 ]; then
        echo "OK: $n 件すべて **1 バイトも違わない**(生成しなおして byte 比較)"
        rc=0
    else
        for f in $FILES; do
            b="$SAVED/$(basename "$f")"
            [ -f "$f" ] || continue
            cmp -s "$b" "$f" && continue
            echo "--- $f の相違"
            cmp "$b" "$f" 2>&1 | sed 's/^/    /' || true
            echo "    違う byte 数: $(cmp -l "$b" "$f" 2>/dev/null | wc -l)"
            echo "    違う行数    : $(diff "$b" "$f" | grep -c '^[<>]' || true)"
            diff -u "$b" "$f" | head -20 | sed 's/^/    /' || true
        done
        report_input_changes
        echo
        echo "NG: $diffs/$n 件が相違。**AOT 生成物が変わっている**"
        rc=1
    fi
    echo "    所要: $(since "$t0" "$(now)") 秒 (うち生成 $GEN_SECONDS 秒)"
    return $rc
}

# ---- 陽性対照(本物の生成器を使う) ----------------------------------------
# 規則 8。**自己試験は偽の生成器で経路を通すだけなので、本物の生成器に対して
# 検出器が働くことは別に示さないといけない。**
#
#   1. 基準を取る(無ければ)
#   2. **生成器を本当に動かしたうえで**、出てきた生成物の 1 バイトを書き換えて
#      check を走らせる -> 相違(1)として落ちなければならない
#   3. もう一度ふつうに check を走らせる -> 同一(0)に戻らなければならない
#      (戻らない検出器は「いつも落ちる」だけで何も見ていない)
#
# 3 回生成するので、本物の生成器なら実測 0.86 秒 x 数回で終わる。
cmd_positive_control() {
    local rc fails=0 f first
    first=$(set -- $FILES; echo "$1")

    if [ ! -f "$META" ]; then
        echo "--- 基準が無いので先に取る"
        cmd_save > /dev/null || return 2
    fi

    echo "=== 陽性対照: 生成物を 1 バイト変えたら落ちるか(本物の生成器を使う) ==="
    rc=0
    AOT_IDENTITY_INJECT_AFTER_GENERATE="$SELF __inject $first" cmd_check > "$IDENT_DIR/pc.txt" 2>&1 || rc=$?
    sed 's/^/    | /' "$IDENT_DIR/pc.txt"
    if [ "$rc" -eq 1 ]; then
        echo "OK(陽性対照): 1 バイト変えると 相違(rc=1)として落ちた"
    else
        echo "FAIL(陽性対照): 1 バイト変えたのに rc=$rc。**検出器が効いていない**" >&2
        fails=$((fails + 1))
    fi

    echo
    echo "=== 陰性対照: そのまま生成しなおせば 同一 に戻るか ==="
    rc=0
    cmd_check > "$IDENT_DIR/nc.txt" 2>&1 || rc=$?
    sed 's/^/    | /' "$IDENT_DIR/nc.txt"
    if [ "$rc" -eq 0 ]; then
        echo "OK(陰性対照): 同一(rc=0)に戻った"
    else
        echo "FAIL(陰性対照): rc=$rc。落ちたままなら「いつも落ちる」だけで何も見ていない" >&2
        fails=$((fails + 1))
    fi

    echo
    if [ "$fails" -eq 0 ]; then
        echo "OK: AOT 同一性比較は、本物の生成物の 1 バイトの差を捕まえ、戻せば通る"
        return 0
    fi
    echo "NG: $fails 件が期待どおりに動かない" >&2
    return 1
}

# ---- 自己試験(規則 8: 試していない検出器は働かない) -----------------------
# docker も QEMU も使わない。本物の生成器の代わりに、中身を制御できる
# 偽の生成器を置いて、save/check の経路そのものを通す。
#
# 確かめること:
#   陰性対照 : 同じものを生成すれば 同一(0)。**そのとき mtime が戻っている**
#              (生成器が実際に走ったことは呼び出し回数カウンタで確かめる。
#               走っていないのに mtime が同じ、では何も確かめていない)
#   陽性対照1: 1 文字だけ変えると 相違(1)
#   陽性対照2: 長さが変わると 相違(1)
#   陽性対照3: ビルドの印が違う基準と比べようとすると 2 で止まる
#   陽性対照4: 基準が無ければ 2 で止まる
cmd_self_test() {
    local f1 f2 counter rc fails=0 mt_before mt_after calls
    # trap は関数を抜けたあとに走るので、箱の場所は local にしてはいけない
    # (local だと trap の時点で未定義になり、set -u で落ちる)
    SELFTEST_BOX=$(mktemp -d)
    box=$SELFTEST_BOX
    trap 'rm -rf "$SELFTEST_BOX"' EXIT
    f1="$box/gen_a.c"
    f2="$box/gen_b.c"
    counter="$box/calls"
    : > "$counter"

    # 偽の生成器を作る。$1 = 埋める値、$2 = 追加する行(空なら足さない)
    mkgen() {
        {
            echo '#!/bin/sh'
            echo "echo x >> '$counter'"
            echo "echo 'static int a = $1;' > '$f1'"
            echo "echo 'static int b = 2;' > '$f2'"
            if [ -n "${2:-}" ]; then
                echo "echo '$2' >> '$f2'"
            fi
        } > "$box/gen.sh"
        chmod +x "$box/gen.sh"
    }
    run() {
        env AOT_IDENTITY_DIR="$box/base" \
            AOT_IDENTITY_FILES="$f1 $f2" \
            AOT_IDENTITY_GENERATE="$box/gen.sh" \
            AOT_IDENTITY_INPUTS="" \
            AOT_IDENTITY_SIGNATURE="${2:--DFAKE}" \
            "$SELF" "$1" > "$box/out.txt" 2>&1
    }
    say() {
        if [ "$2" = "$3" ]; then
            echo "OK($1): $4"
        else
            echo "FAIL($1): $4 — 期待 rc=$3 だが rc=$2" >&2
            sed 's/^/    | /' "$box/out.txt" >&2
            fails=$((fails + 1))
        fi
    }

    mkgen 1 ""
    rc=0; run save || rc=$?
    say "準備" "$rc" 0 "基準を退避できた"

    # 生成物と基準の mtime を古い時刻に揃える。戻していなければ必ず差が出る
    touch -d '@1000000000' "$f1" "$f2"
    cp -p "$f1" "$box/base/files/$(basename "$f1")"
    cp -p "$f2" "$box/base/files/$(basename "$f2")"
    mt_before=$(stat -c '%Y' "$f1")
    calls=$(wc -l < "$counter")

    rc=0; run check || rc=$?
    say "陰性対照" "$rc" 0 "同じものを生成すれば 同一"
    mt_after=$(stat -c '%Y' "$f1")
    if [ "$(wc -l < "$counter")" -le "$calls" ]; then
        echo "FAIL(陰性対照): 生成器が呼ばれていない。**消してから生成しなおす経路が死んでいる**" >&2
        fails=$((fails + 1))
    else
        echo "OK(陰性対照): 生成器は実際に呼ばれた(= 消してから生成しなおしている)"
    fi
    if [ "$mt_before" = "$mt_after" ]; then
        echo "OK(陰性対照): mtime が戻っている($mt_before)。カーネルは作り直されない"
    else
        echo "FAIL(陰性対照): mtime が戻っていない($mt_before -> $mt_after)。**カーネルがまるごと作り直される**" >&2
        fails=$((fails + 1))
    fi

    mkgen 1 ""
    rc=0; run save || rc=$?
    mkgen 2 ""                      # 1 文字だけ違う生成器
    rc=0; run check || rc=$?
    say "陽性対照1" "$rc" 1 "1 文字だけ変えると 相違として落ちる"

    mkgen 1 ""
    rc=0; run save || rc=$?
    mkgen 1 "static int c = 3;"     # 長さが変わる
    rc=0; run check || rc=$?
    say "陽性対照2" "$rc" 1 "長さが変わると 相違として落ちる"

    mkgen 1 ""
    rc=0; run save || rc=$?
    rc=0; run check "-DFAKE -DISIKIOS_GC_DEBUG" || rc=$?
    say "陽性対照3" "$rc" 2 "ビルドの印が違う基準とは比べずに止まる"

    # [罠] 印は空白を含む形で出てくる。空白の有無だけで「違う設定」と
    # 誤判定しないこと(= 正規化が効いていること)
    rc=0; run check "   -DFAKE  " || rc=$?
    say "陰性対照2" "$rc" 0 "空白の有無だけでは「違う設定」と判定しない"

    rm -rf "$box/base"
    rc=0; run check || rc=$?
    say "陽性対照4" "$rc" 2 "基準が無ければ止まる"

    # positive-control(本物の生成器向けの陽性対照)そのものの経路も通しておく。
    # **これを通していないと、親が本物で走らせたときに初めて壊れていると分かる**
    mkgen 1 ""
    rc=0; run positive-control || rc=$?
    say "陽性対照5" "$rc" 0 "positive-control 自体が成立する(1 バイト変えて落ち、戻して通る)"

    # 相違の切り分け(report_input_changes)。**ここを試していないと、
    # 本番で相違が出たときに「入力が変わったせい」なのかが読めない**
    local inp="$box/input.lisp"
    echo '(defun f () 1)' > "$inp"
    { echo '#!/bin/sh'; echo "echo '$inp'"; } > "$box/inputs.sh"
    chmod +x "$box/inputs.sh"
    run2() {
        env AOT_IDENTITY_DIR="$box/base" \
            AOT_IDENTITY_FILES="$f1 $f2" \
            AOT_IDENTITY_GENERATE="$box/gen.sh" \
            AOT_IDENTITY_INPUTS="$box/inputs.sh" \
            AOT_IDENTITY_SIGNATURE="-DFAKE" \
            "$SELF" "$1" > "$box/out.txt" 2>&1
    }

    mkgen 1 ""
    rc=0; run2 save || rc=$?
    say "準備2" "$rc" 0 "入力を記録した基準を退避できた"
    mkgen 2 ""
    rc=0; run2 check || rc=$?
    say "陽性対照6" "$rc" 1 "入力は同じで生成物だけ変わった"
    if grep -q 'すべて基準と同じ' "$box/out.txt"; then
        echo "OK(切り分け): 「入力は変わっていない = C 側かトランスパイラが原因」と報告した"
    else
        echo "FAIL(切り分け): 入力が同じであることを報告していない" >&2
        sed 's/^/    | /' "$box/out.txt" >&2
        fails=$((fails + 1))
    fi

    mkgen 1 ""
    rc=0; run2 save || rc=$?
    echo '(defun f () 2)' > "$inp"      # 入力そのものを変える
    mkgen 3 ""
    rc=0; run2 check || rc=$?
    say "陽性対照7" "$rc" 1 "入力も生成物も変わった"
    if grep -q '変わった入力' "$box/out.txt"; then
        echo "OK(切り分け): 「入力が変わっている = この比較では何も言えない」と報告した"
    else
        echo "FAIL(切り分け): 入力の変化を報告していない" >&2
        sed 's/^/    | /' "$box/out.txt" >&2
        fails=$((fails + 1))
    fi

    echo
    if [ "$fails" -eq 0 ]; then
        echo "OK: AOT 同一性検出器の自己試験はすべて通った(docker も QEMU も使っていない)"
        return 0
    fi
    echo "NG: $fails 件が期待どおりに動かない。**検出器が効いていない**" >&2
    return 1
}

case "${1:-}" in
    save)             cmd_save ;;
    check)            cmd_check ;;
    positive-control) cmd_positive_control ;;
    self-test)        cmd_self_test ;;
    # 内部用。cmd_positive_control が生成の直後に自分を呼び戻すための口
    __inject)         inject_one_byte "$2" ;;
    *)
        sed -n '2,24p' "$0" | sed 's/^# \{0,1\}//'
        exit 2
        ;;
esac
