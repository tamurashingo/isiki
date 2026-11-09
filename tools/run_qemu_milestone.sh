#!/bin/sh
# QEMU で 1 つのミルストンを走らせ、結果を判定する。
#
# **「結果が無い」を「テストが失敗した」と区別し、結果が無いときだけ 1 回再試行する。**
#
# ## なぜ要るか(2026-11-01、3 件目でこの仕組みを入れた)
#
# ゲストが `test-results.txt` を書く前に終わると、make 側は `test -f` で落ちる。
# すると **「テストが失敗した」ではなく「結果が無い」** という形で現れ、
# 外から見ると本物の失敗と区別が付かない。
# `documents/records-and-measurement-rules.md`「付録: QEMU 実行の flake 記録」に
# 記録されている 3 件はすべてこの形である:
#
#   2026-10-02 (PR #115) ベンチ測定中にゲストが起動途中で落ちた      再現せず
#   2026-10-29 (PR #121) make test-qemu-all の qemu_boot_partition  再現せず
#   2026-11-01 (PR #150) CI の qemu_boot_partition(gpt_multi)       再現せず
#                        **同じコミットのもう 1 本は pass、再実行も pass**
#
# **2 件目と 3 件目は同じミルストン(qemu_boot_partition / gpt_multi)である。**
#
# 文書は「3 件目が来たら族として扱う。そのときは『test-results.txt が無い』を
# 失敗と区別して再試行する仕組みを、ベンチだけでなく test-qemu-milestone 側にも
# 入れることになる」と予告していた。**これがその実装である。**
#
# ## 何を再試行し、何を再試行しないか
#
#   再試行する  … **結果が無い**(ゲストが test-results.txt を書く前に終わった)
#   再試行しない … **結果があって失敗している**(= 本物の失敗。1 件でも failed なら即座に落とす)
#
# この区別が要点である。**失敗を再試行で隠してはいけない。**
#
# ## 黙って再試行しない
#
# ベンチ側(tools/bench/bench_common.sh)と同じ作法で、`[FLAKE]` を 1 行残し、
# `tmp/qemu_flakes.txt` に追記する。**「たまに落ちる」という事実を消さない。**
#
# ## 使い方(Makefile から呼ばれる)
#
#   tools/run_qemu_milestone.sh <milestone> <qemu コマンドを 1 行で>
#
# 第 2 引数は呼び出し側(Makefile)が組み立てた QEMU のコマンド行。
# **このスクリプトは QEMU のオプションを知らない**(Makefile 側の唯一の真実源を壊さない)。

set -e

# [規則 8] この仕組み自身の自己試験。**4 つの経路すべてを意図的に発火させる。**
#   (1) 結果が無い(2 回とも)      -> 再試行して落ちる + [FLAKE] を残す
#   (2) 結果があって失敗          -> **再試行しない**(失敗を隠さない)。flake 記録は 0 件
#   (3) 1 回目で成功              -> 再試行も記録もしない
#   (4) 1 回目は結果が無く 2 回目で成功 -> 通るが [FLAKE] が残る
# (2) が落ちることを確かめるのが要点である。ここが壊れると**本物の失敗を
# 再試行で隠す**仕組みになってしまう。
if [ "$1" = "--self-test" ]; then
    self_dir=$(mktemp -d)
    trap 'rm -rf "$self_dir"' EXIT
    ok=0
    # $0 は相対の場合があるので、cd する前に絶対パスへ直す
    self_script=$(cd "$(dirname "$0")" && pwd)/$(basename "$0")
    _run() {   # $1=説明 $2=期待rc $3=期待flake件数 $4=qemuコマンド
        # **set -e で落ちないように `|| rc=$?` で受ける**(部分シェルが 1 を返す
        #  のは期待どおりの経路であり、そこで打ち切ってはいけない)
        rc=0
        ( cd "$self_dir" && mkdir -p tmp \
          && QEMU_FLAKE_LOG=tmp/f.txt sh "$self_script" "m.lisp" "$4" >out.txt 2>&1 ) || rc=$?
        n=$(wc -l < "$self_dir/tmp/f.txt" 2>/dev/null | tr -d ' ' || true)
        n=${n:-0}
        if [ "$rc" = "$2" ] && [ "$n" = "$3" ]; then
            echo "OK($1): rc=$rc flake=$n 件 (期待 rc=$2 flake=$3)"
        else
            echo "FAIL($1): rc=$rc flake=$n 件。期待は rc=$2 flake=$3" >&2
            ok=1
        fi
        rm -f "$self_dir/tmp/f.txt" "$self_dir/test-results.txt" "$self_dir/tmp/.try"
    }
    echo "[self-test] QEMU ミルストンの再試行(結果が無いときだけ)"
    _run "陽性対照1・結果が無い(2 回とも)" 1 1 "true"
    _run "陽性対照2・結果があって失敗 -> 再試行しない" 1 0 \
         "printf '==== isiki tests: 3 passed, 1 failed ====\n' > test-results.txt"  # PASS-LINE: FIXTURE 判定役にかける偽の成果物(不合格側。陽性対照2)
    _run "陰性対照・1 回目で成功" 0 0 \
         "printf '==== isiki tests: 4 passed, 0 failed ====\n' > test-results.txt"  # PASS-LINE: FIXTURE 判定役にかける偽の成果物(合格側。陰性対照)
    _run "陽性対照3・2 回目で成功 -> 通るが記録は残る" 0 1 \
         'test -f tmp/.try && printf "==== isiki tests: 5 passed, 0 failed ====\n" > test-results.txt; touch tmp/.try'  # PASS-LINE: FIXTURE 判定役にかける偽の成果物(2回目で成功。陽性対照3)
    if [ "$ok" = 0 ]; then
        echo "self-test 成功: **結果が無いときだけ再試行し、失敗は再試行しない**ことを確かめた"
        exit 0
    fi
    exit 1
fi

MILESTONE="$1"
QEMU_CMD="$2"
FLAKE_LOG="${QEMU_FLAKE_LOG:-tmp/qemu_flakes.txt}"
MAX_ATTEMPTS="${QEMU_MILESTONE_ATTEMPTS:-2}"

if [ -z "$MILESTONE" ] || [ -z "$QEMU_CMD" ]; then
    echo "使い方: $0 <milestone> <qemu コマンド>" >&2
    exit 2
fi

mkdir -p tmp

attempt=1
while :; do
    rm -f .qemu-test-trigger test-results.txt
    echo "$MILESTONE" > .qemu-test-trigger

    # QEMU 自身が 0 以外で終わることもある(イメージのロック等)。
    # set -e で即死させず、結果の有無で分けて判断する
    qemu_rc=0
    sh -c "$QEMU_CMD" || qemu_rc=$?
    rm -f .qemu-test-trigger

    if [ -f test-results.txt ]; then
        cat test-results.txt
        if grep -q " 0 failed" test-results.txt; then
            exit 0
        fi
        # **結果があって失敗している = 本物の失敗。再試行しない。**
        echo "" >&2
        echo "NG: $MILESTONE が失敗しました(結果は上のとおり)。**再試行しません**" >&2
        echo "    結果が出ているので flake ではなく本物の失敗です" >&2
        exit 1
    fi

    # ここから下は「結果が無い」側
    if [ "$attempt" -ge "$MAX_ATTEMPTS" ]; then
        echo "" >&2
        echo "ERROR: $MILESTONE が $MAX_ATTEMPTS 回とも **結果を出しませんでした**" >&2
        echo "    (test-results.txt が無い。QEMU の終了コード=$qemu_rc)" >&2
        echo "    ゲストが起動途中で終わっているか、ハングして打ち切られている可能性があります。" >&2
        echo "    **2 回続くなら flake ではない。** 次に見るもの:" >&2
        echo "      - QEMU の出力(上)に起動の失敗やロックのメッセージが無いか" >&2
        echo "      - 同じミルストンを単体で走らせて再現するか" >&2
        echo "      - ハングなら、直前の変更が静かにスタックやヒープを壊していないか" >&2
        exit 1
    fi

    # **黙って再試行しない。** 事実を 1 行残す(ベンチ側と同じ作法)
    echo "$MILESTONE" >> "$FLAKE_LOG"
    echo "" >&2
    echo "[FLAKE] $MILESTONE: **結果が無い**(test-results.txt 未作成、QEMU の終了コード=$qemu_rc)。" >&2
    echo "        1 回目なのでやり直します。記録: $FLAKE_LOG" >&2
    echo "        **これは失敗の隠蔽ではない**: 結果が出ていて失敗している場合は再試行しません" >&2
    attempt=$((attempt + 1))
done
