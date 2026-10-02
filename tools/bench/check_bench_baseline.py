#!/usr/bin/env python3
"""[性能測定] 構文別ベンチマークの集計と、記録値からのずれの検出器。

documents/performance-measurement.md「JIT 経路の基準値」節 / 「検出器」節。

**なぜ要るか。**
4 つ(いま 5 つ)の検出器はコードとテストを守っていたが、基準値を守るものが
1 つも無かった。だから documents/performance-measurement.md の記録値は
165 コミットのあいだ誰も測り直さず、AOT 経路が 1.5〜1.7 倍悪化したこと
(issue #114)に気づくまでに 2 週間かかった。

**双方向である理由。**
遅くなった側だけ捕まえても足りない。**予想より速くなったときは、記録が古いか、
測定が壊れている。** この 2 週間だけでも「完全一致」(PR #87/#88 の git stash
空振り)と「違いすぎる数字」(PR #91 の嘘の宣言)の両方で測定ミスを見つけて
いる。改善を黙って通すと、記録はずれ続ける。EVAL-ERROR の予算を上下どちらでも
落とす形にしたのと同じ考え方。

**帯は分解能から決める。**根拠なく ±10% と置かない。帯の由来は
tools/bench/bench_baseline.tsv のヘッダに書いてある。

入力(--results)の形式は 1 行 1 測定の TSV:
    path  case  rep  n  lo  hi
lo は N 回、hi は 3N 回の**ブート全体の総命令数**。傾き (hi-lo)/(2N) が
「仕事 1 単位あたりの命令数」になる。
"""

import argparse
import statistics
import sys


def read_results(path):
    rows = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            p, case, rep, n, lo, hi = line.split("\t")
            n = int(n)
            slope = (int(hi) - int(lo)) / (2 * n)
            rows.append({"path": p, "case": case, "rep": int(rep), "n": n,
                         "lo": int(lo), "hi": int(hi), "slope": slope})
    return rows


def read_baseline(path):
    base = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            cols = line.split("\t")
            if len(cols) != 8:
                sys.exit(f"ERROR: {path}: 列が 8 つではありません: {line!r}")
            p, case, unit, value, band, n, rev, date = cols
            base[(p, case)] = {"unit": unit, "value": float(value),
                               "band": float(band), "n": int(n),
                               "revision": rev, "date": date}
    return base


def group(rows):
    g = {}
    for r in rows:
        g.setdefault((r["path"], r["case"]), []).append(r)
    return g


def print_measured(g):
    print("| 経路 | カテゴリ | N | 回数 | 中央値(命令/単位) | 最小 | 最大 | 幅 |")
    print("|---|---|---:|---:|---:|---:|---:|---:|")
    for (p, case) in sorted(g):
        s = sorted(x["slope"] for x in g[(p, case)])
        med = statistics.median(s)
        n = g[(p, case)][0]["n"]
        print(f"| {p} | {case} | {n} | {len(s)} | {med:.2f} | "
              f"{s[0]:.2f} | {s[-1]:.2f} | {s[-1] - s[0]:.2f} |")


def print_spread(g):
    """繰り返しのばらつき = この計器の 1 回あたりの分解能。

    規則 9 (R-instrument)。「差が出なかった」と言うには、どのくらいの差なら
    見えるのかを先に出しておく必要がある。
    """
    multi = {k: v for k, v in g.items() if len(v) > 1}
    if not multi:
        return
    print()
    print("**繰り返しのばらつき(= 1 回あたりの分解能の実測)**")
    print()
    print("| 経路 | カテゴリ | 中央値 | 絶対幅(命令/単位) | 相対幅 |")
    print("|---|---|---:|---:|---:|")
    worst = 0.0
    for (p, case) in sorted(multi):
        s = sorted(x["slope"] for x in multi[(p, case)])
        med = statistics.median(s)
        spread = s[-1] - s[0]
        worst = max(worst, spread)
        rel = spread / med * 100 if med else float("nan")
        print(f"| {p} | {case} | {med:.2f} | {spread:.2f} | {rel:.1f}% |")
    print()
    print(f"**この実行での最大の絶対幅: {worst:.2f} 命令/単位。**")
    print("帯はこの値から決める(bench_baseline.tsv のヘッダに由来を書くこと)。")


def print_cross(g):
    """経路をまたいだ比較。**どの経路の数字なのかを必ず並べて出す。**

    「経路」の列が無かったことが issue #114 を 165 コミット見逃した原因である。
    """
    cases = sorted({case for (_, case) in g})
    have = {k: statistics.median(x["slope"] for x in v) for k, v in g.items()}
    print()
    print("**経路間の比較**")
    print()
    print("| カテゴリ | C | AOT | JIT | AOT/C | JIT-AOT | JIT/AOT |")
    print("|---|---:|---:|---:|---:|---:|---:|")
    for case in cases:
        c = have.get(("c", case))
        a = have.get(("aot", case))
        j = have.get(("jit", case))
        cs = f"{c:.2f}" if c is not None else "-"
        as_ = f"{a:.2f}" if a is not None else "-"
        js = f"{j:.2f}" if j is not None else "-"
        ac = f"{a / c:.1f}x" if (a is not None and c) else "-"
        jd = f"{j - a:+.2f}" if (a is not None and j is not None) else "-"
        ja = f"{j / a:.2f}x" if (a and j is not None) else "-"
        print(f"| {case} | {cs} | {as_} | {js} | {ac} | {jd} | {ja} |")


def check(g, base, tight_band):
    print()
    print("=== 記録値との照合(検出器、双方向) ===")
    ng = []
    warn = []
    for (p, case) in sorted(g):
        b = base.get((p, case))
        s = [x["slope"] for x in g[(p, case)]]
        med = statistics.median(s)
        n = g[(p, case)][0]["n"]
        if b is None:
            print(f"[未記録] {p}/{case}: 測定値 {med:.2f} 命令/単位。"
                  f"bench_baseline.tsv に記録が無い")
            continue
        if b["n"] != n:
            ng.append((p, case,
                       f"N が記録と違う(記録 {b['n']} / 今回 {n})。"
                       f"N が違う傾きは比較できない"))
            continue
        diff = med - b["value"]
        if abs(diff) <= b["band"]:
            print(f"[OK] {p}/{case}: {med:.2f} (記録 {b['value']:.2f} "
                  f"±{b['band']:.2f}, 差 {diff:+.2f})")
            # 繰り返しが 3 回以上あるときは中央値が 1 発の外れ値に強いので、
            # **もっと細い帯でも見る。落とさずに警告する**(規則 6/8)。
            # 既定の帯(1 回測定でも誤報しない幅)では、+2.6% のような
            # 小さな退行(PR #113 が報告した tailrec/for の悪化)が通ってしまう
            if len(s) >= 3 and abs(diff) > tight_band:
                warn.append((p, case,
                             f"{med:.2f} (記録 {b['value']:.2f}, 差 {diff:+.2f})"
                             f" -- 既定の帯 ±{b['band']:.2f} には収まるが、"
                             f"3 回測定の分解能 ±{tight_band:.2f} を超えている"))
        else:
            direction = "遅くなった" if diff > 0 else "**速くなった**"
            ng.append((p, case,
                       f"{med:.2f} (記録 {b['value']:.2f} ±{b['band']:.2f}, "
                       f"差 {diff:+.2f}) -- {direction}"))

    measured = set(g)
    for key in sorted(base):
        if key not in measured:
            print(f"[未測定] {key[0]}/{key[1]}: 記録 {base[key]['value']:.2f} "
                  f"(この実行では測っていない)")

    if warn:
        print()
        print("--- 注意(落としはしない) ---")
        for p, case, msg in warn:
            print(f"[注意] {p}/{case}: {msg}")
        print("もう一度測って再現するなら、記録を見直すこと(規則 4)。")

    if not ng:
        print()
        print("検出器: 既定の帯を超えたずれはありません。")
        return 0

    print()
    print("=== 検出器が落ちました ===")
    for p, case, msg in ng:
        print(f"[NG] {p}/{case}: {msg}")
    print("""
直し方:

  1. **まず測定を疑う。** 同じコマンドをもう一度流す。差が再現しなければ
     それはノイズである(規則 4: 再現するまでは効果と呼ばない)。
     繰り返し測るには BENCH_REPEAT=3 を付ける。
  2. 再現したら、**それが意図した変更かどうかを決める。**
     - 意図した変更なら、tools/bench/bench_baseline.tsv の該当行を
       新しい値・新しいリビジョン・新しい測定日で**書き換えてコミットする。**
       経路・単位・N・帯の列も合っているか見ること。
     - 意図しない変更なら、それが今回見つけたい退行(または改善)である。
       **数字を黙って更新しないこと。**
  3. 「速くなった」で落ちたときも同じ手順を踏む。記録が古いか、
     測定が壊れているか、本物の改善かは、測り直さないと決まらない。
""")
    return 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--results", required=True)
    ap.add_argument("--baseline")
    ap.add_argument("--no-check", action="store_true",
                    help="集計だけして記録値との照合をしない(基準値を取り直すとき)")
    ap.add_argument("--tight-band", type=float, default=3.0,
                    help="繰り返しが 3 回以上あるときに追加で見る細い帯"
                         "(既定 3.0 命令/単位)。超えても落とさず警告する")
    args = ap.parse_args()

    rows = read_results(args.results)
    if not rows:
        sys.exit("ERROR: 測定結果が空です")
    g = group(rows)

    print_measured(g)
    print_spread(g)
    print_cross(g)

    if args.no_check or not args.baseline:
        print()
        print("(--no-check: 記録値との照合をしていません)")
        return 0
    return check(g, read_baseline(args.baseline), args.tight_band)


if __name__ == "__main__":
    sys.exit(main())
