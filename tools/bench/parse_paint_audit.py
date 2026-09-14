#!/usr/bin/env python3
"""[GC監査] 1グループぶんの実行結果を1行のTSVへ要約する。

tools/bench/run_paint_audit.sh から呼ばれる。指示書 第0部の要件のうち
「タイムアウトを失敗と区別する」「GC回数(カバレッジ)を記録する」
「何件目以降の結果か」をここで判定する。

ゲスト側の逐次出力(test-results.txt)の行形式:
    #file <label>
    #t <idx> <label> OK|NG gc=<delta> tick=<delta> stale=<total>
    #control pre|post positive=<n> negative=<n>
    #audit total=<n> first-ng=<n>
    ==== isiki tests: <p> passed, <f> failed ====
"""
import sys


def main():
    (name, qemu_exit, make_rc, elapsed, log_path, serial_path) = sys.argv[1:7]

    lines = []
    try:
        with open(log_path, encoding='utf-8', errors='replace') as f:
            lines = f.read().splitlines()
    except OSError:
        pass
    serial = ''
    try:
        with open(serial_path, encoding='utf-8', errors='replace') as f:
            serial = f.read()
    except OSError:
        pass

    passed = failed = None
    total = first_ng = 0
    ctl_pre = ctl_post = '-'
    gc_total = 0
    gc_zero = 0
    asserts = 0
    trap_hits = 0
    trap_sites = 0

    for ln in lines:
        if ln.startswith('#t '):
            parts = ln.split()
            # #t <idx> <label> OK|NG gc=.. tick=.. stale=..
            label = parts[2]
            if label != name:
                continue          # 対照の分は本体の統計に混ぜない
            asserts += 1
            gc = int(parts[4].split('=')[1])
            gc_total += gc
            if gc == 0:
                gc_zero += 1
        elif ln.startswith('#trap '):
            for tok in ln.split()[1:]:
                k, _, v = tok.partition('=')
                if k == 'hits':
                    trap_hits = int(v)
                elif k == 'sites':
                    trap_sites = int(v)
        elif ln.startswith('#control pre '):
            ctl_pre = ln.split('positive=')[1].split()[0]
        elif ln.startswith('#control post '):
            ctl_post = ln.split('positive=')[1].split()[0]
        elif ln.startswith('#audit '):
            for tok in ln.split()[1:]:
                k, _, v = tok.partition('=')
                if k == 'total':
                    total = int(v)
                elif k == 'first-ng':
                    first_ng = int(v)
        elif ln.startswith('==== isiki tests:'):
            toks = ln.replace(',', ' ').split()
            passed = int(toks[3])
            failed = int(toks[5])

    # 後置対照が走っていれば、その1件は**意図的なNG**なので実failから引く
    intentional = 1 if ctl_post != '-' else 0
    real_fail = None if failed is None else max(failed - intentional, 0)

    # 分類。タイムアウトを失敗として扱うと実在しないバグを追うことになるので、
    # 必ず別カテゴリにする
    if qemu_exit in ('124', '137'):
        status = 'TIMEOUT'
    elif 'CPU EXCEPTION' in serial:
        status = 'EXCEPTION'
    elif 'PANIC' in serial or 'panic' in serial:
        status = 'PANIC'
    elif passed is None:
        # 最終行が出ていない = 報告に到達する前に止まった
        status = 'INCOMPLETE'
    elif trap_hits:
        status = 'TRAP'
    elif real_fail:
        status = 'FAIL'
    else:
        status = 'OK'

    print('\t'.join(str(x) for x in [
        name, status, qemu_exit, elapsed,
        '-' if passed is None else passed,
        '-' if real_fail is None else real_fail,
        asserts, first_ng, gc_total, gc_zero, trap_hits, trap_sites,
        ctl_pre, ctl_post,
    ]))


if __name__ == '__main__':
    main()
