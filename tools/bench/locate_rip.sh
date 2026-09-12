#!/bin/bash
# [GC監査] CPU例外のrip・スタック上の戻りアドレスを、ソース上の関数へ逆引きする。
#
# UEFIがイメージを読み込むアドレスは実行ごとに変わるので、絶対アドレスのままでは
# 引けない。例外ハンドラが自分自身のアドレス(handler=)を併記しているので、
# addr - handler の差を、PEファイル上のc_cpu_exception_handlerのアドレスに
# 足し直せばファイル上のアドレスになる。
#
# 使い方:
#   tools/bench/locate_rip.sh <handler> <addr> [addr...]
#   tools/bench/locate_rip.sh --serial <serialファイル>
#   tools/bench/locate_rip.sh --trap <anchor> <addr> [addr...]
#
# --trap は、ゲスト内の %%DIAG-GC-TRAP-SITE-ADDR が報告した検出箇所を引くための
# 形。基準シンボルが例外ダンプ(c_cpu_exception_handler)ではなく
# %%DIAG-IMAGE-ANCHOR(os_gc_debug_trap_read)になる
#
# --serial を使うと、例外ダンプのrip・汎用レジスタ・stack(rsp..)の全語を読み、
# イメージ範囲に入るものだけを関数名へ逆引きして並べる(= 簡易バックトレース)。
set -eu

efi="esp_dir/EFI/BOOT/BOOTX64.EFI"
dis="tmp/bootx64.dis"

ensure_dis() {
    if [ ! -f "$dis" ] || [ "$efi" -nt "$dis" ]; then
        echo "(逆アセンブルを生成中: $dis)" >&2
        docker run --rm --user "$(id -u):$(id -g)" --entrypoint x86_64-w64-mingw32-objdump \
            -v "$PWD":/workspace isiki-builder -d /workspace/$efi > "$dis"
    fi
}

ensure_dis

if [ "${1:-}" = "--serial" ]; then
    python3 tools/bench/locate_rip.py "$dis" --serial "$2"
elif [ "${1:-}" = "--trap" ]; then
    shift
    anchor="$1"; shift
    python3 tools/bench/locate_rip.py "$dis" --anchor-symbol os_gc_debug_trap_read \
        --handler "$anchor" "$@"
else
    handler="$1"; shift
    python3 tools/bench/locate_rip.py "$dis" --handler "$handler" "$@"
fi
