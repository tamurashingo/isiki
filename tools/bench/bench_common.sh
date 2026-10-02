# [性能測定] ベンチドライバの共通処理。**source して使う(実行はしない)。**
#
# run_jit_bench.sh と run_construct_bench.sh の両方が使う。
# 2 箇所に同じものを書けば、いつか片方だけ直される
# (PR #115 §3 で本体の S 式について同じことを決めた)。
#
# 提供するもの:
#   bench_init_eval_range <nmファイル> <ウォームアップ用milestoneを書く関数>
#       os_eval の実行時アドレス範囲を決める。**ウォームアップも兼ねる**
#       (ビルド直後の初回実行は命令数が約 118M 多い。捨てないと最初に測る
#        カテゴリの傾きだけが系統的に狂う)
#   bench_plugin_flags
#       QEMU へ渡す -plugin 引数(範囲が決まっていれば start=/end= 付き)
#   bench_run <milestoneパス> <説明>
#       1 ブート測って "total_insns range_insns" を返す。
#       **アンカーが動いていたら止まる**(start=/end= が os_eval を
#        指さなくなるため)。1 回だけ再試行し、[FLAKE] 行を残す
#   bench_flake_count / bench_report_flakes

BENCH_PLUGIN="${BENCH_PLUGIN:-tools/plugins/isiki_instcount.so}"
BENCH_FLAKE_LOG="${BENCH_FLAKE_LOG:-tmp/bench_flakes.txt}"

_bench_eval_start=""
_bench_eval_end=""
_bench_eval_anchor=""

bench_plugin_flags() {
    if [ -n "$_bench_eval_start" ]; then
        echo "-plugin file=$BENCH_PLUGIN,start=$_bench_eval_start,end=$_bench_eval_end"
    else
        echo "-plugin file=$BENCH_PLUGIN"
    fi
}

# $1: milestone のパス  $2: 説明(エラー表示用)
# -> "total_insns range_insns" を標準出力へ。失敗したら exit 1
bench_run() {
    local ms="$1" what="$2"
    local out line insns range anchor attempt
    attempt=1
    while : ; do
        # make 側が " 0 failed" を grep するので、**Lisp 側のゲートが落ちれば
        # ここで失敗する**(JIT に乗っていないカテゴリの数字は 1 つも出ない)
        if out=$(make test-qemu-milestone QEMU_EXTRA_FLAGS="$(bench_plugin_flags)" \
                     MILESTONE="$ms" 2>&1); then
            line=$(echo "$out" | sed -n 's/.*\[isiki_instcount\] \(total_insns=.*\)/\1/p' | tail -1)
            insns=$(echo "$line" | sed -n 's/.*total_insns=\([0-9]*\).*/\1/p')
            range=$(echo "$line" | sed -n 's/.*range_insns=\([0-9]*\).*/\1/p')
            anchor=$(echo "$out" | sed -n 's/^#anchor \([0-9]*\)$/\1/p' | tail -1)
            if [ -n "$insns" ]; then
                # **アンカーが動いていたら start=/end= は別の場所を指している。**
                # 黙って続けると os_eval を見ていないのに「傾き 0」になる
                if [ -n "$_bench_eval_anchor" ] && [ -n "$anchor" ] \
                   && [ "$anchor" != "$_bench_eval_anchor" ]; then
                    echo "ERROR: イメージのロードアドレスが動きました ($what)" >&2
                    echo "       範囲決定時 $_bench_eval_anchor / このブート $anchor" >&2
                    echo "       start=/end= が os_eval を指していません。中止します" >&2
                    exit 1
                fi
                echo "$insns ${range:-0}"
                return 0
            fi
        fi
        if [ "$attempt" -ge 2 ]; then
            echo "ERROR: 測定が 2 回失敗しました ($what)。中止します" >&2
            echo "$out" | tail -25 >&2
            exit 1
        fi
        # **黙って再試行しない。**「たまに落ちる」という事実を消さないため、
        # 1 行残して最後に件数を出す(2026-10-02 に aot/let の N=3,000,000 が
        # 素のブートより少ない total_insns で落ちた例が 1 件。再現しなかった)
        echo "$what" >> "$BENCH_FLAKE_LOG"
        echo "[FLAKE] $what: 1 回目が失敗したのでやり直します" >&2
        echo "$out" | sed -n 's/.*\(total_insns=[0-9]*\).*/        \1/p' | tail -1 >&2
        attempt=$((attempt + 1))
    done
}

# $1: nm ファイル  $2: ウォームアップに使う milestone のパス(呼び出し側が用意済み)
bench_init_eval_range() {
    local nm="$1" ms="$2"
    : > "$BENCH_FLAKE_LOG"
    if [ ! -f "$nm" ]; then
        echo "ERROR: $nm が無い。make build を先に実行すること" >&2
        exit 1
    fi
    echo "--- ウォームアップ兼アンカー取得(測定値は捨てる) ---"
    bench_run "$ms" "warmup" > /dev/null
    _bench_eval_anchor=$(sed -n 's/^#anchor \([0-9]*\)$/\1/p' test-results.txt | tail -1)
    if [ -z "$_bench_eval_anchor" ]; then
        echo "ERROR: %%DIAG-IMAGE-ANCHOR-PUB を取得できませんでした。" >&2
        echo "       milestone に (format ... \"#anchor ~D~%\" (%%diag-image-anchor-pub)) が要る" >&2
        exit 1
    fi
    read -r _bench_eval_start _bench_eval_end <<EOF
$(python3 tools/bench/os_eval_range.py "$nm" "$_bench_eval_anchor")
EOF
    echo "os_eval 区間: $_bench_eval_start .. $_bench_eval_end (実行時アンカー $_bench_eval_anchor)"
}

bench_report_flakes() {
    local flakes
    flakes=$(wc -l < "$BENCH_FLAKE_LOG")
    if [ "$flakes" -gt 0 ]; then
        echo "*** 注意: ゲストの起動が $flakes 回失敗し、やり直しています:"
        sed 's/^/***   /' "$BENCH_FLAKE_LOG"
        echo "***       測定条件ではなく処理系側の問題の可能性がある。"
        echo
    fi
}
