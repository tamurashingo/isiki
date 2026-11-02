#!/bin/bash
# [同一性] JIT が出力した機械語が「1 バイトも変わっていない」ことを、
# JIT_DUMP ビルドのシリアル出力で確かめるドライバ。
#
# なぜ要るか。JIT_DUMP=1(-DISIKIOS_JIT_DUMP)でビルドすると、コンパイルした
# 関数ごとに [JITDUMP] size=… maskedbytes=… hash=… がシリアルへ出る
# (src/c/za.c)。**出す側はあったが、比べる側が無かった。**
# documents/jit-tag-constants.md §5.2 の「1038 関数、完全一致」は人が目で
# 突き合わせた結果で、手順として残っていない。
# フレームの配置を並べ替える類の変更は「全関数でバイト長もバイト列も変わらない」
# ことが合否そのものなので、1 コマンドで判定が出る形にしておく。
#
# 使い方:
#   make jit-identity-save       変更前に基準を取る(**QEMU を 1 回起動する**)
#   (ここで C 側を変更する)
#   make jit-identity-check      同じ条件で流して基準と比べる(QEMU を 1 回起動する)
#   make jit-identity-show       何が走るかだけを見る空運転(QEMU を起動しない)
#   make jit-identity-self-test  検出器の自己試験(ビルドも QEMU も要らない)
#
# 終了コード:
#   0  同一   1  相違(これが検出)   2  比較が成立しない
#
# [罠1] JIT_DUMP ビルドは**別ビルド**である。BUILD_FLAG_SIGNATURE が変わるので
#   GC_DEBUG_STAMP 経由でカーネルが作り直される(Makefile:91 付近)。
#   つまり save と check は**どちらも JIT_DUMP=1 で**揃えないと意味が無い。
#   このスクリプトは make へ JIT_DUMP=1 を渡し、さらに印に
#   -DISIKIOS_JIT_DUMP が入っていることを**先に確かめてから**動く。
#   入っていなければダンプは 0 行になり、0 行同士が「一致」になってしまう。
#
# [罠2] ダンプ行は**関数名を持たない**。突き合わせは出現順で行うので、
#   比べる条件(MILESTONE・QEMU_MEM・ビルドの印)が同じでなければならない。
#   基準に記録して check で突き合わせ、違えば 2 で止める。
#
# 変数:
#   MILESTONE          流す boot スクリプト(既定 test/lisp/qemu_boot_test.lisp。
#                      make test-qemu が既定で読むものと同じ)
#   QEMU_MEM           ゲストのメモリ(既定は Makefile の既定 = 256M)
#   JIT_IDENTITY_DIR   基準の置き場(既定 tmp/identity/jit)。**make clean で消える**
#   JIT_IDENTITY_LOG   既に取ってあるシリアルログを使う(**QEMU を起動しない**)。
#                      ログだけ手元にある場合と、自己試験が使う
#   JIT_IDENTITY_SIGNATURE       ビルドの印。未設定なら make から取る
#   JIT_IDENTITY_IGNORE_MAKE_RC  1 なら試験が落ちても比較へ進む(既定は止まる)
set -eu

IDENT_DIR=${JIT_IDENTITY_DIR:-tmp/identity/jit}
MILESTONE=${MILESTONE:-test/lisp/qemu_boot_test.lisp}
QEMU_MEM=${QEMU_MEM:-256M}
GC_DEBUG_STAMP=${GC_DEBUG_STAMP:-tmp/.gc-debug-flags}
CHECKER=tools/check_jit_dump_identity.py
SELF=$(cd "$(dirname "$0")" && pwd)/$(basename "$0")

now() { date +%s.%N; }
since() { awk -v a="$1" -v b="$2" 'BEGIN{printf "%.1f", b-a}'; }

# $(BUILD_FLAG_SIGNATURE) は空のフラグ変数のぶんだけ空白が残るので、
# 既定ビルドでは "    "、JIT_DUMP=1 では "  -DISIKIOS_JIT_DUMP  " のような形で
# 出てくる。**空白をそのまま持ち回ると引数に渡したときに分割される**ので、
# 詰めた形に正規化してから使う(比較は正規化後の文字列どうしで行う)
normalize_sig() {
    printf '%s' "$1" | tr -s ' ' ' ' | sed -e 's/^ *//' -e 's/ *$//'
}

signature() {
    if [ "${JIT_IDENTITY_SIGNATURE+set}" = set ]; then
        normalize_sig "$JIT_IDENTITY_SIGNATURE"
    else
        normalize_sig "$(make -s print-build-flag-signature JIT_DUMP=1)"
    fi
}

# [罠1] ダンプが出ないビルドで比べると「0 行 vs 0 行 = 一致」になる。
# 計器が働く設定であることを**先に**確かめる
require_jit_dump() {
    case "$1" in
        *-DISIKIOS_JIT_DUMP*) return 0 ;;
    esac
    echo "ERROR: ビルドの印に -DISIKIOS_JIT_DUMP が入っていない: [$1]" >&2
    echo "       JIT_DUMP=1 でないと [JITDUMP] 行は 1 行も出ず、**0 行同士が" >&2
    echo "       「一致」になってしまう**。Makefile の JIT_DUMP_FLAGS を確かめること" >&2
    return 2
}

# 実際に打つ make を**1 箇所で**組む。show が同じものを表示できるようにして
# あるので、「表示したものと走るものが違う」が起きない。
# test-qemu-milestone に $(QEMU_EXTRA_FLAGS) があるので -serial を差し込める
# (tools/bench/run_paint_audit.sh が同じ口を使っている)
build_make_cmd() {
    MAKE_CMD=(make test-qemu-milestone
        JIT_DUMP=1
        MILESTONE="$MILESTONE"
        QEMU_MEM="$QEMU_MEM"
        QEMU_EXTRA_FLAGS="-serial file:$(pwd)/$1")
}

# シリアルログを用意する。JIT_IDENTITY_LOG が指定されていればそれを使い、
# QEMU は起動しない
obtain_log() {
    local log t0
    if [ -n "${JIT_IDENTITY_LOG:-}" ]; then
        if [ ! -f "$JIT_IDENTITY_LOG" ]; then
            echo "ERROR: JIT_IDENTITY_LOG=$JIT_IDENTITY_LOG が無い" >&2
            return 2
        fi
        LOG_PATH=$JIT_IDENTITY_LOG
        RUN_SECONDS=0
        echo "--- 既にあるシリアルログを使う(QEMU は起動しない): $LOG_PATH"
        return 0
    fi
    mkdir -p "$IDENT_DIR"
    log="$IDENT_DIR/$1.serial.txt"
    : > "$log"
    build_make_cmd "$log"
    echo "--- JIT_DUMP=1 でビルドして QEMU を 1 回起動する"
    echo "    シリアル -> $log"
    echo "    $(printf '%q ' "${MAKE_CMD[@]}")"
    t0=$(now)
    set +e
    "${MAKE_CMD[@]}" > "$IDENT_DIR/$1.make.log" 2>&1
    MAKE_RC=$?
    set -e
    RUN_SECONDS=$(since "$t0" "$(now)")
    echo "    make の終了コード: $MAKE_RC  / 所要 ${RUN_SECONDS} 秒"
    if [ "$MAKE_RC" -ne 0 ] && [ "${JIT_IDENTITY_IGNORE_MAKE_RC:-0}" != "1" ]; then
        echo "ERROR: 試験が通らなかった(make rc=$MAKE_RC)。**比較の土台が崩れている**" >&2
        echo "       ゲストが途中で止まるとダンプも途中で切れるので、比較しても" >&2
        echo "       意味が無い。ログ: $IDENT_DIR/$1.make.log" >&2
        echo "       どうしても比べるなら JIT_IDENTITY_IGNORE_MAKE_RC=1" >&2
        return 2
    fi
    LOG_PATH=$log
    return 0
}

# [罠] 印には空白が含まれうるので、**配列で渡す**。
# $(meta_args) のように文字列で渡すと "signature=" と "-DISIKIOS_JIT_DUMP" に
# 分割され、比較器が知らないオプションとして死ぬ
build_meta_args() {
    META_ARGS=(--meta "signature=$1"
               --meta "milestone=$MILESTONE"
               --meta "qemu-mem=$QEMU_MEM")
}

cmd_run() {   # $1 = save|check
    local sig t0 rc
    t0=$(now)
    sig=$(signature)
    require_jit_dump "$sig" || return 2

    echo "=== JIT 生成コードの同一性: $1 ==="
    echo "    ビルドの印   : [$sig]"
    if [ -f "$GC_DEBUG_STAMP" ]; then
        echo "    直前のビルド : [$(cat "$GC_DEBUG_STAMP")]  (印と違えば make が作り直す)"
    else
        echo "    直前のビルド : (スタンプ無し = まだビルドしていない)"
    fi
    obtain_log "$1" || return 2

    rc=0
    build_meta_args "$sig"
    python3 "$CHECKER" "$1" "$LOG_PATH" --dir "$IDENT_DIR" "${META_ARGS[@]}" || rc=$?
    echo "    所要(合計): $(since "$t0" "$(now)") 秒 (うち QEMU ${RUN_SECONDS} 秒)"
    if [ "$1" = save ] && [ "$rc" -eq 0 ]; then
        echo
        echo "次: C 側を変更したあと  make jit-identity-check"
    fi
    return $rc
}

# ---- 自己試験(規則 8) -----------------------------------------------------
# **QEMU もビルドも使わない。** 合成したシリアルログを JIT_IDENTITY_LOG で
# 食わせて、ドライバの判定経路そのものを通す。
#
# 確かめること:
#   比較器      : tools/check_jit_dump_identity.py --self-test(バイト列の感度)
#   陽性対照1   : ビルドの印に -DISIKIOS_JIT_DUMP が無ければ 2 で止まる
#                 (**ここを通すと 0 行同士が「一致」になる**)
#   陰性対照    : 同じログなら save -> check が 同一(0)
#   陽性対照2   : ログの 1 関数の hash を変えると 相違(1)
#   陽性対照3   : 条件(QEMU_MEM)が違う基準とは比べずに 2 で止まる
cmd_self_test() {
    local box log rc fails=0
    echo "--- 比較器の自己試験 ---"
    if ! python3 "$CHECKER" --self-test; then
        echo "FAIL: 比較器の自己試験が落ちた" >&2
        return 1
    fi
    echo
    echo "--- ドライバの自己試験(QEMU もビルドも使わない) ---"
    SELFTEST_BOX=$(mktemp -d)
    trap 'rm -rf "$SELFTEST_BOX"' EXIT
    box=$SELFTEST_BOX
    log="$box/serial.txt"
    {
        echo "isiki-test-load src/lisp/init.lisp"
        echo "[JITDUMP] size=6 maskedbytes=0 hash=0123456789abcdef"
        echo "[JITDUMP] size=40 maskedbytes=16 hash=fedcba9876543210"
        echo "[JITDUMP] size=128 maskedbytes=24 hash=00112233445566aa"
        echo "3269 passed, 0 failed"
    } > "$log"

    run() {   # $1 = save|check, $2 = 印, $3 = QEMU_MEM
        env JIT_IDENTITY_DIR="$box/base" \
            JIT_IDENTITY_LOG="$log" \
            JIT_IDENTITY_SIGNATURE="$2" \
            QEMU_MEM="${3:-256M}" \
            MILESTONE="test/lisp/qemu_boot_test.lisp" \
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

    rc=0; run save "-DISIKIOS_GC_DEBUG" || rc=$?
    say "陽性対照1" "$rc" 2 "印に -DISIKIOS_JIT_DUMP が無ければ止まる(0 行同士の「一致」を防ぐ)"

    rc=0; run save "-DISIKIOS_JIT_DUMP" || rc=$?
    say "準備" "$rc" 0 "基準を退避できた"

    rc=0; run check "-DISIKIOS_JIT_DUMP" || rc=$?
    say "陰性対照" "$rc" 0 "同じログ・同じ条件なら 同一"

    rc=0; run check "-DISIKIOS_JIT_DUMP" "96M" || rc=$?
    say "陽性対照3" "$rc" 2 "QEMU_MEM が違う基準とは比べずに止まる"

    sed -i 's/hash=fedcba9876543210/hash=fedcba9876543211/' "$log"
    rc=0; run check "-DISIKIOS_JIT_DUMP" || rc=$?
    say "陽性対照2" "$rc" 1 "1 関数のバイト列が変わると 相違として落ちる"

    echo "[JITDUMP] size=16 maskedbytes=0 hash=aaaabbbbccccdddd" >> "$log"
    rc=0; run check "-DISIKIOS_JIT_DUMP" || rc=$?
    say "陽性対照4" "$rc" 1 "関数が 1 つ増えると 相違として落ちる"

    # [罠] $(BUILD_FLAG_SIGNATURE) は空白を含む("  -DISIKIOS_JIT_DUMP  ")。
    # **引数として素で渡すと分割されて比較器が死ぬ**(実際に踏んだ)。
    # 空白付きの印で save -> check が通ることで、分割されないことと
    # 正規化が効いていることの両方を見る
    rc=0; run save "  -DISIKIOS_JIT_DUMP  " || rc=$?
    say "罠(空白)1" "$rc" 0 "印に空白があっても引数が分割されない"
    rc=0; run check "-DISIKIOS_JIT_DUMP" || rc=$?
    say "罠(空白)2" "$rc" 0 "空白を詰めた印として突き合わせられる"

    echo
    if [ "$fails" -eq 0 ]; then
        echo "OK: JIT 同一性ドライバの自己試験はすべて通った(QEMU もビルドも使っていない)"
        return 0
    fi
    echo "NG: $fails 件が期待どおりに動かない。**検出器が効いていない**" >&2
    return 1
}

# QEMU を起動せずに「何が走るか」だけを見せる。**1 回 10 分近い起動を
# 払う前に、印と条件が意図どおりかを確かめるための口。**
# make -n ではここまで追えない(src/c/disasm_symtab.c が生成物で、-n からは
# 作られないため make が先に止まる。これは既存の挙動)
cmd_show() {
    local sig
    sig=$(signature)
    echo "=== JIT 同一性比較で走るもの(空運転。QEMU は起動しない) ==="
    echo "    ビルドの印 : [$sig]"
    if require_jit_dump "$sig"; then
        echo "    -> -DISIKIOS_JIT_DUMP あり。[JITDUMP] 行が出るビルドである"
    else
        return 2
    fi
    build_make_cmd "$IDENT_DIR/save.serial.txt"
    echo "    打つコマンド:"
    echo "      $(printf '%q ' "${MAKE_CMD[@]}")"
    echo "    基準の置き場 : $IDENT_DIR  (**make clean で消える**)"
    echo "    基準に記録して check で突き合わせる条件:"
    echo "      signature=$sig"
    echo "      milestone=$MILESTONE"
    echo "      qemu-mem=$QEMU_MEM"
    return 0
}

case "${1:-}" in
    save|check) cmd_run "$1" ;;
    show)       cmd_show ;;
    self-test)  cmd_self_test ;;
    *)
        sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'
        exit 2
        ;;
esac
