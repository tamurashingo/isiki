#!/usr/bin/env python3
"""[GC監査] summary.tsvを読んで、監査の結果とカバレッジをまとめる。

カバレッジを必ず併記する。**塗り潰しはGCが走らなければ何も検出しない**ので、
GC 0回の試験が通っても保護漏れがないことの証拠にはならない
(documents/pitfalls.md 原則6)。
"""
import sys
import os


def main():
    outdir = sys.argv[1]
    path = os.path.join(outdir, 'summary.tsv')
    rows = []
    with open(path, encoding='utf-8') as f:
        header = f.readline().rstrip('\n').split('\t')
        for ln in f:
            if ln.strip():
                rows.append(dict(zip(header, ln.rstrip('\n').split('\t'))))

    if not rows:
        print('(結果なし)')
        return

    print('| 試験 | 結果 | 秒 | pass | fail | assert | 最初のNG | GC計 | GC0件 | trap回数 | trap箇所 | 対照前 | 対照後 |')
    print('|---|---|---|---|---|---|---|---|---|---|---|---|---|')
    for r in rows:
        print('| {name} | {status} | {elapsed_sec} | {pass} | {fail} | {asserts} | '
              '{first_ng} | {gc_total} | {gc_zero} | {trap_hits} | {trap_sites} | '
              '{ctl_pre} | {ctl_post} |'.format(**r))

    by_status = {}
    for r in rows:
        by_status.setdefault(r['status'], []).append(r['name'])
    print()
    print('## 分類')
    for st in sorted(by_status):
        print(f'- {st}: {len(by_status[st])}件 — {" ".join(by_status[st])}')

    tot_assert = sum(int(r['asserts']) for r in rows)
    tot_zero = sum(int(r['gc_zero']) for r in rows)
    covered = tot_assert - tot_zero
    print()
    print('## カバレッジ(GCを跨いだアサーションの割合)')
    if tot_assert:
        print(f'- 記録されたアサーション: {tot_assert}件')
        print(f'- GCが1回以上走ったもの: {covered}件 ({100.0 * covered / tot_assert:.1f}%)')
        print(f'- GC 0回のもの: {tot_zero}件 — **これらは監査できていない**')
    else:
        print('- アサーションが1件も記録されていない')

    tot_trap = sum(int(r['trap_hits']) for r in rows)
    if tot_trap:
        print()
        print('## 塗り潰しトラップの読み出し')
        print(f'- 延べ回数: {tot_trap}')
        print('- **回数は「箇所数」ではない**(再帰の各段で読まれれば1つのバグが深さぶん数えられる)。')
        print('  箇所は各 <name>.log の #trapsite 行を tools/bench/locate_rip.sh で逆引きすること。')

    # 対照が効いていない回は、その回の「0件」を根拠にできない
    bad = [r['name'] for r in rows if r['ctl_pre'] not in ('999', '-')]
    if bad:
        print()
        print('## 感度が確認できなかった回(結果を根拠にできない)')
        print('- ' + ' '.join(bad))


if __name__ == '__main__':
    main()
