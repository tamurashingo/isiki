#!/usr/bin/env python3
"""[GC監査] 通常ビルドと監査ビルドの所要時間を試験ごとに突き合わせる。

全体の平均比が出せても、確保の多い試験だけ極端に遅ければ**単一のタイムアウト閾値では
分類できない**。試験ごとの比のばらつきと、GC回数と所要時間の相関を見る。

相関が取れれば「この試験はGCがN回走るからM秒かかるはず」という予測ができ、
そこから外れたものだけをハング候補にできる。

使い方: compare_audit_time.py <通常ビルドのoutdir> <監査ビルドのoutdir>
"""
import sys, os


def load(d):
    rows = {}
    p = os.path.join(d, 'summary.tsv')
    with open(p, encoding='utf-8') as f:
        head = f.readline().rstrip('\n').split('\t')
        for ln in f:
            if not ln.strip():
                continue
            r = dict(zip(head, ln.rstrip('\n').split('\t')))
            rows[r['name']] = r
    return rows


def main():
    base, dbg = load(sys.argv[1]), load(sys.argv[2])
    names = [n for n in base if n in dbg]
    if not names:
        sys.exit('共通の試験がありません')

    # bootの所要時間は最小値で近似する(最も軽い試験はほぼbootだけ)
    b_boot = min(int(base[n]['elapsed_sec']) for n in names)
    d_boot = min(int(dbg[n]['elapsed_sec']) for n in names)
    print(f'boot近似: 通常={b_boot}s  監査={d_boot}s  (最も軽い試験の所要時間)')
    print()
    print('| 試験 | 通常(s) | 監査(s) | 全体比 | 限界(通常) | 限界(監査) | 限界比 | GC回数 |')
    print('|---|---|---|---|---|---|---|---|')
    ratios, marg = [], []
    for n in sorted(names, key=lambda x: -int(dbg[x]['elapsed_sec'])):
        b, d = int(base[n]['elapsed_sec']), int(dbg[n]['elapsed_sec'])
        gc = dbg[n]['gc_total']
        r = d / b if b else float('nan')
        mb, md = b - b_boot, d - d_boot
        mr = (md / mb) if mb > 0 else None
        ratios.append(r)
        if mr is not None and mb > 0:
            marg.append((n, mr, int(gc)))
        mrs = f'{mr:.1f}x' if mr is not None else '—'
        print(f'| {n} | {b} | {d} | {r:.1f}x | {mb} | {md} | {mrs} | {gc} |')

    print()
    print(f'全体比: 最小 {min(ratios):.1f}x / 最大 {max(ratios):.1f}x '
          f'/ 幅 {max(ratios) / min(ratios):.1f}倍')
    if marg:
        vals = [m for _, m, _ in marg]
        print(f'限界比: 最小 {min(vals):.1f}x / 最大 {max(vals):.1f}x '
              f'/ 幅 {max(vals) / min(vals):.1f}倍')
    print()
    print('判定: 幅が大きいほど単一閾値での分類は成り立たない。')
    print('      GC回数との相関が取れるなら、予測値からの乖離でハングを判定する。')


if __name__ == '__main__':
    main()
