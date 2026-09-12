#!/bin/bash
# [GC監査] 塗り潰し(ISIKIOS_GC_PAINT)下で既存のQEMU試験を流し、保護漏れを
# 洗い出すためのドライバ。documents/pitfalls.md 原則6。
#
# 設計上の要件(指示書 第0部):
#   1. 逐次出力: 1アサーションごとにゲスト側がtest-results.txtへ書き、
#      finish-outputまで行う(9p越しにホストのファイルへ直接届く)。
#      ゲストがハングしても、そこまでの記録は残る。
#   2. 検出しても中断しない: NGは記録して続行する。ただし「何件目以降の結果か」
#      (#audit first-ng)を必ず残す。最初の検出より後ろは独立の再確認が要る。
#   3. GC回数の記録: 塗り潰しはGCが走らなければ何も検出しない。各アサーションの
#      GC回数差を記録し、GC 0回の試験を「監査できていない」として集計する。
#   4. タイムアウトを失敗と区別: QEMUにtimeoutを直接かけ、終了コード124を
#      TIMEOUTとして別カテゴリに記録する。通常ビルドの所要時間と並べて見る。
#   5. 再開・分割: グループ単位で1回ずつQEMUを起動し、完了済みグループは飛ばす。
#
# 使い方:
#   # 塗り潰し有効ビルドで監査
#   rm -f esp_dir/EFI/BOOT/BOOTX64.EFI    # フラグ変更では再ビルドされないため必須
#   GC_DEBUG=1 GC_PAINT=1 make build
#   tools/bench/run_paint_audit.sh
#
#   # 基準時間(通常ビルド)の取得。対照は使えないのでAUDIT_CONTROL=0
#   rm -f esp_dir/EFI/BOOT/BOOTX64.EFI && make build
#   AUDIT_CONTROL=0 AUDIT_OUTDIR=tmp/paint_audit_base tools/bench/run_paint_audit.sh
#
# 環境変数:
#   AUDIT_OUTDIR   結果の保存先(既定 tmp/paint_audit)。ここに既に .status が
#                  ある試験は飛ばす(再開)。最初からやり直すならディレクトリを消す
#   AUDIT_TIMEOUT  1グループあたりの打ち切り秒数(既定 1800)
#   AUDIT_CONTROL  1なら前置・後置の対照を各グループに挟む(既定 1)。
#                  通常ビルド(%%DIAG-GC-STRESSが無い)では0にすること
#   AUDIT_STRESS   0以外なら監査本体の前に(%%diag-gc-stress N)を設定する。
#                  GC 0回の試験が多かった場合に、集計を見てから使う
#   AUDIT_FILES    流す試験ファイル(空白区切り)。既定はqemu_boot_test.lispと同じ列
#   AUDIT_DISK_IMG QEMUのhd0に与えるイメージ(既定はMakefileのIDE_DISK_IMG)

set -eu

OUTDIR="${AUDIT_OUTDIR:-tmp/paint_audit}"
TIMEOUT_SEC="${AUDIT_TIMEOUT:-1800}"
USE_CONTROL="${AUDIT_CONTROL:-1}"
STRESS="${AUDIT_STRESS:-0}"

# qemu_boot_test.lisp と同じ試験列。1ファイル = 1グループ = 1回のQEMU起動にする。
# 分離しておくと、ハングしたときにどのファイルかが起動単位で確定し、
# 再開も1ファイル単位でできる
DEFAULT_FILES="init_test isiki_test za_test za_test_ext5 za_test_ext7 za_test_ext8 \
za_test_ext9 za_test_ext10 za_test_ext11 za_test_ext12 za_test_ext13 za_test_ext14 \
za_test_ext15 za_test_ext16 za_test_ext17 za_test_ext18 za_test_ext19 \
environment_pages_test environment_literal_slots_test environment_utilities_test \
room_test aot_leaf_gc_test"
FILES="${AUDIT_FILES:-$DEFAULT_FILES}"

mkdir -p "$OUTDIR" tmp
SUMMARY="$OUTDIR/summary.tsv"
if [ ! -f "$SUMMARY" ]; then
    printf 'name\tstatus\tqemu_exit\telapsed_sec\tpass\tfail\tasserts\tfirst_ng\tgc_total\tgc_zero\ttrap_hits\ttrap_sites\tctl_pre\tctl_post\n' > "$SUMMARY"
fi

# $1: 試験ファイル名(拡張子なし) -> boot-entryスクリプトのパスを標準出力へ
gen_boot() {
    local name="$1"
    local boot="tmp/audit_boot_${name}.lisp"
    {
        echo '(load "src/lisp/init.lisp")'
        echo '(load "test/lisp/test_framework.lisp")'
        echo '(setq *isiki-audit* t)'
        if [ "$USE_CONTROL" = "1" ]; then
            echo '(setq *isiki-audit-stale-available* t)'
            echo '(isiki-audit-begin "control-pre")'
            echo '(load "test/lisp/audit_control_pre.lisp")'
        fi
        if [ "$STRESS" != "0" ]; then
            echo "(%%diag-gc-stress $STRESS)"
        fi
        echo "(isiki-audit-begin \"$name\")"
        echo "(load \"test/lisp/${name}.lisp\")"
        if [ "$STRESS" != "0" ]; then
            echo '(%%diag-gc-stress 0)'
        fi
        # 本体だけの計数を出してから後置対照へ入る(対照の検出を混ぜない)
        echo '(isiki-audit-trap-report)'
        if [ "$USE_CONTROL" = "1" ]; then
            echo '(isiki-audit-begin "control-post")'
            echo '(load "test/lisp/audit_control_post.lisp")'
        fi
        echo '(isiki-test-report)'
        echo '(close *isiki-test-stream*)'
    } > "$boot"
    echo "$boot"
}

echo "=== 塗り潰し監査: outdir=$OUTDIR timeout=${TIMEOUT_SEC}s control=$USE_CONTROL stress=$STRESS ==="

for name in $FILES; do
    if [ -f "$OUTDIR/$name.status" ]; then
        echo "--- $name: 済み($(cat "$OUTDIR/$name.status")) — 飛ばす"
        continue
    fi
    boot=$(gen_boot "$name")
    serial="$OUTDIR/$name.serial.txt"
    : > "$serial"

    echo "--- $name ---"
    start=$(date +%s)
    set +e
    make test-qemu-audit-run \
        MILESTONE="$boot" \
        AUDIT_TIMEOUT="$TIMEOUT_SEC" \
        QEMU_EXTRA_FLAGS="-serial file:$PWD/$serial" \
        ${AUDIT_DISK_IMG:+QEMU_DISK_IMG="$AUDIT_DISK_IMG"} \
        > "$OUTDIR/$name.make.log" 2>&1
    make_rc=$?
    set -e
    end=$(date +%s)
    elapsed=$((end - start))

    qemu_exit=$(cat tmp/qemu-exit.txt 2>/dev/null || echo "?")
    # 逐次出力の本体。ゲストがハングしていても、そこまでの行は残っている
    cp -f test-results.txt "$OUTDIR/$name.log" 2>/dev/null || : > "$OUTDIR/$name.log"

    python3 tools/bench/parse_paint_audit.py \
        "$name" "$qemu_exit" "$make_rc" "$elapsed" \
        "$OUTDIR/$name.log" "$serial" >> "$SUMMARY"
    tail -1 "$SUMMARY" | sed 's/^/    /'
    tail -1 "$SUMMARY" | cut -f2 > "$OUTDIR/$name.status"
done

echo
echo "=== 集計 ==="
python3 tools/bench/report_paint_audit.py "$OUTDIR"
