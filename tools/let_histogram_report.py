#!/usr/bin/env python3
"""[調査] tools/let_histogram.lisp の出力を集計する。documents/jit-frame-survey.md §3。

**AOT 入力と実行時 load を分けるのが要点である。**
`let` の束縛数の上限(ZA_MAX_LOCALS_PER_LET / ZA_MAX_LET_DEPTH)は JIT だけに効く。
transpile.lisp は C を生成するので、TRANSPILE_LISP_SRC のファイルには一切効かない。

使い方:
  make let-histogram > tmp/let_hist.tsv
  python3 tools/let_histogram_report.py tmp/let_hist.tsv
"""

import collections
import sys

# Makefile の TRANSPILE_LISP_SRC と同期を保つこと(共通の真実源が無いため手動)
AOT = set("""src/lisp/transpile.lisp test/lisp/transpile_fixture.lisp src/lisp/init_aot.lisp
src/lisp/utility.lisp src/lisp/device.lisp src/lisp/ide.lisp src/lisp/partition.lisp
src/lisp/mount.lisp src/lisp/file-node.lisp src/lisp/fat16.lisp src/lisp/fat32.lisp
src/lisp/file-cmd.lisp src/lisp/bench_aot.lisp""".split())


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "tmp/let_hist.tsv"
    rows, skips = [], []
    for ln in open(path, encoding="utf-8"):
        f = ln.rstrip("\n").split("\t")
        if f[0] == "IIFE" and f[1].lstrip("-").isdigit():
            rows.append((int(f[1]), int(f[2]), f[3], f[4]))
        elif f[0] == "SKIP":
            skips.append(f[1:])
    sk = collections.Counter(s[0] for s in skips)
    print(f"IIFE {len(rows)} 件 / 飛ばした {len(skips)} 件 "
          f"({', '.join(f'{k}={v}' for k, v in sk.items())})")

    for label, aot in (("AOT 入力(この上限は効かない)", True),
                       ("実行時 load(JIT が見る。この上限が効く)", False)):
        sub = [r for r in rows if (r[2] in AOT) == aot]
        w = collections.Counter(r[0] for r in sub)
        d = collections.Counter(r[1] for r in sub)
        per = collections.defaultdict(lambda: [0, 0])
        for ww, dd, fi, nm in sub:
            k = (fi, nm)
            per[k][0] = max(per[k][0], ww)
            per[k][1] = max(per[k][1], dd)
        print()
        print(f"### {label}")
        print(f"  IIFE {len(sub)} 件 / 関数 {len(per)} 本")
        print("  幅: " + "  ".join(f"{k}→{w[k]}" for k in sorted(w)))
        print(f"  **幅 5 以上: {sum(v for k, v in w.items() if k >= 5)} 件 / "
              f"{len([1 for v in per.values() if v[0] >= 5])} 本**")
        print(f"  深さ 9 以上: {len([1 for v in per.values() if v[1] >= 9])} 本 / "
              f"17 以上: {len([1 for v in per.values() if v[1] >= 17])} 本")
        for k, v in sorted(((k, v) for k, v in per.items()
                            if v[0] >= 5 or v[1] >= 9),
                           key=lambda x: (-x[1][0], -x[1][1])):
            print(f"     幅{v[0]:2d} 深さ{v[1]:2d}  {k[0]}  {k[1]}")


if __name__ == "__main__":
    main()
