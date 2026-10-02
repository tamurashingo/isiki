#!/usr/bin/env python3
"""[性能測定] 構文別ベンチマークの集計と、記録値からのずれの検出器。

documents/performance-measurement.md「JIT 経路の基準値」節 /
「インタプリタ落ちの検出器」節。

**なぜ要るか。**
4 つ(いま 5 つ)の検出器はコードとテストを守っていたが、基準値を守るものが
1 つも無かった。だから documents/performance-measurement.md の記録値は
165 コミットのあいだ誰も測り直さず、AOT 経路が 1.5〜1.7 倍悪化したこと
(issue #114)に気づくまでに 2 週間かかった。

**双方向である理由。**
遅くなった側だけ捕まえても足りない。**予想より速くなったときは、記録が古いか、
測定が壊れている。** この 2 週間だけでも「完全一致」(PR #87/#88 の git stash
空振り)と「違いすぎる数字」(PR #91 の嘘の宣言)の両方で測定ミスを見つけて
いる。改善を黙って通すと、記録はずれ続ける。

**os_eval のゲート。**
%%za-compiled-p が T でも、本体が全部ネイティブで走るとは限らない。
反復ごとに os_eval へ落ちていれば、その数字は「JIT の性能」でも
「AOT の性能」でもない。**N に対する os_eval の命令数の傾きが 0 でない
カテゴリは、測定値を出さずに落とす。**

入力(--results)の形式は 1 行 1 測定の TSV:
    path  case  rep  n  lo  hi  [lo_eval  hi_eval]
lo は N 回、hi は 3N 回の**ブート全体の総命令数**。傾き (hi-lo)/(2N) が
「仕事 1 単位あたりの命令数」になる。lo_eval/hi_eval は同じブートで
os_eval 区間だけを数えた命令数(TCG プラグインの range_insns)。
"""

import argparse
import statistics
import sys

BASELINE_COLS = 10   # path case unit estimator trials value band n revision date


def read_results(path):
    rows = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            cols = line.split("\t")
            if len(cols) == 6:
                p, case, rep, n, lo, hi = cols
                lo_ev = hi_ev = None
            elif len(cols) == 8:
                p, case, rep, n, lo, hi, lo_ev, hi_ev = cols
                lo_ev, hi_ev = int(lo_ev), int(hi_ev)
            else:
                sys.exit(f"ERROR: {path}: 列数が 6 でも 8 でもありません: {line!r}")
            n = int(n)
            rows.append({"path": p, "case": case, "rep": int(rep), "n": n,
                         "lo": int(lo), "hi": int(hi),
                         "slope": (int(hi) - int(lo)) / (2 * n),
                         "lo_eval": lo_ev, "hi_eval": hi_ev,
                         "eval_slope": None if lo_ev is None
                                       else (hi_ev - lo_ev) / (2 * n)})
    return rows


def read_baseline(path):
    base = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            cols = line.split("\t")
            if len(cols) != BASELINE_COLS:
                sys.exit(
                    f"ERROR: {path}: 列が {BASELINE_COLS} つではありません "
                    f"({len(cols)} 列): {line!r}\n"
                    "       形式は path case unit estimator trials value band n "
                    "revision date です。\n"
                    "       **推定量と試行回数の列は 2026-10-02 に足した。** "
                    "最小値は試行回数を増やすと系統的に下がるので、\n"
                    "       回数を書かずに記録すると比較できない値になる。")
            p, case, unit, est, trials, value, band, n, rev, date = cols
            # band が "-" の行は**照合しない**。「この数字が何を測っているのか
            # まだ分からない」ものを検出器に載せると、調査の結論が出る前に
            # 誰かがその数字を根拠に何かを言えてしまう。
            # **帯を広げてごまかすのではなく、対象から外す。**
            base[(p, case)] = {"unit": unit, "estimator": est,
                               "trials": int(trials), "value": float(value),
                               "band": None if band == "-" else float(band),
                               "n": int(n), "revision": rev, "date": date}
    return base


def read_calibration(path):
    """**真の値が分かっている行**(校正点)を読む。

    documents/performance-measurement.md「推定量を校正点で決めた」節。
    **これはコードを検査するテストではなく、計器を検査するテストである。**
    推定量を変えたとき・N を変えたとき・プラグインを触ったときに、
    真の値から外れたら計器が壊れている。
    """
    cal = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            cols = line.split("\t")
            if len(cols) != 5:
                sys.exit(f"ERROR: {path}: 列が 5 つではありません: {line!r}")
            p, case, true_value, tol, basis = cols
            cal[(p, case)] = {"true": float(true_value), "tol": float(tol),
                              "basis": basis}
    return cal


def check_calibration(g, cal, estimator):
    """校正点に当てる。**選んだ推定量が外れたら落ちる。**"""
    rows = [(k, v) for k, v in cal.items() if k in g]
    if not rows:
        return 0
    print()
    print("=== 校正点(真の値が分かっている行)===")
    ng = []
    for (p, case), c in sorted(rows):
        e = estimates(g[(p, case)])
        print(f"  {p}/{case}: 真値 {c['true']:.3f}  許容 ±{c['tol']:.3f}  "
              f"N={e['n']}  回数={e['trials']}")
        print(f"      根拠: {c['basis']}")
        for name in ESTIMATOR_NAMES:
            d = e[name] - c["true"]
            mark = "  <= 選択中" if name == estimator else ""
            ok = "OK " if abs(d) <= c["tol"] else "**外れ**"
            print(f"      {ok} {name:18s} {e[name]:8.3f}  差 {d:+7.3f}{mark}")
        if abs(e[estimator] - c["true"]) > c["tol"]:
            ng.append((p, case, estimator, e[estimator], c))
    if ng:
        print()
        print("=== 校正点を外しました — **計器が壊れています** ===")
        for p, case, est, got, c in ng:
            print(f"[NG] {p}/{case}: 推定量 {est} が {got:.3f}。"
                  f"真値 {c['true']:.3f} ±{c['tol']:.3f} から外れている")
        print("""
**これは測定対象の退行ではない。計器の故障である。**
記録値を更新しても直らない。見るところ:

  1. **推定量を変えたのではないか。** 上の表で真値に入っている推定量がある。
     --estimator / BENCH_ESTIMATOR でそれを選び、記録を取り直すこと
  2. **N が小さすぎないか。** 1 ブートの異常(実測で約 31M 命令)を 2N で
     割った値が誤差になる。真値 4 に対して N=10,000,000 では
     31M/2e7 = 1.56 命令/単位、つまり 39% の誤差になりうる
  3. **プラグインを触ったのではないか。** total_insns の数え方が変わると
     全部ずれる
  4. **ベンチの C 実装を変えたのではないか。** その場合は逆アセンブルし直して
     tools/bench/bench_calibration.tsv の真値と根拠を更新すること
""")
        return 1
    print()
    print("校正点: すべて真の値の許容範囲に入っています。")
    return 0


def group(rows):
    g = {}
    for r in rows:
        g.setdefault((r["path"], r["case"]), []).append(r)
    return g


ESTIMATOR_NAMES = ["slope-of-medians", "median", "min-slope",
                   "slope-of-mins", "slope-of-maxes"]

# **試行回数を増やすと系統的にずれる推定量。** 3 回の最小値と 10 回の最小値は
# 別の量である。中央値はこの性質を持たない
TRIALS_DEPENDENT = {"min-slope", "slope-of-mins", "slope-of-maxes"}


def estimates(rows):
    """同じデータから推定量を 5 通り出す。**どれを選んでも 5 つとも出力に出る。**

    `lo` は N 回、`hi` は 3N 回のブート全体の総命令数。傾きは (hi-lo)/(2N)。

    **「妨害は増やす方向にしか働かない」はこの計器では成り立たない。**
    校正点(`c/for` は逆アセンブルで 4 命令)に当てると、
    **1 ブートだけ約 31M 命令**少ない値が出ることがあり、それが `hi` 側に
    当たると最小値ベースの推定量はその外れ値を**選んで**拾う。
    実測(2026-10-02、N=10,000,000、真値 4.000):

        hi1 -0.08 / hi2 +0.08 / **hi3 -1.56**(= -31,176,758 命令)
        min-slope 2.44 / slope-of-mins 2.44 / median 3.92 / slope-of-maxes 4.07

    導出は documents/performance-measurement.md「推定量を校正点で決めた」節。
    """
    n = rows[0]["n"]
    sl = [r["slope"] for r in rows]
    los = [r["lo"] for r in rows]
    his = [r["hi"] for r in rows]
    e = {
        # 傾きを 1 回ずつ求めてから中央値。**既定。**1 本の外れたブートに強い
        "median": statistics.median(sl),
        # 端点ごとに中央値を採ってから差を取る
        "slope-of-medians": (statistics.median(his) - statistics.median(los)) / (2 * n),
        # 傾きの最小値。**低い側の外れ値を選んで拾う。**校正点を外す
        "min-slope": min(sl),
        # 端点ごとに最小値を採ってから差を取る(「妨害は増やす方向にしか
        # 働かない」をそのまま適用した形)。**校正点を外す**
        "slope-of-mins": (min(his) - min(los)) / (2 * n),
        # 端点ごとに最大値を採ってから差を取る。校正点には近いが、
        # **本物の妨害(ISR)は増やす方向なので、原理的にはそれを拾う**
        "slope-of-maxes": (max(his) - max(los)) / (2 * n),
    }
    e.update({"slopes": sorted(sl), "n": n, "trials": len(rows)})
    return e


def print_measured(g, estimator):
    heads = " | ".join(ESTIMATOR_NAMES)
    print(f"| 経路 | カテゴリ | N | 回数 | **{estimator}** | {heads} | 最小 | 最大 |")
    print("|---|---|---:|---:|---:|" + "---:|" * len(ESTIMATOR_NAMES) + "---:|---:|")
    for (p, case) in sorted(g):
        e = estimates(g[(p, case)])
        s = e["slopes"]
        cells = " | ".join(f"{e[k]:.2f}" for k in ESTIMATOR_NAMES)
        print(f"| {p} | {case} | {e['n']} | {e['trials']} | "
              f"**{e[estimator]:.2f}** | {cells} | {s[0]:.2f} | {s[-1]:.2f} |")


def print_eval(g, tol):
    """os_eval 区間の命令数の傾き。**0 でなければ反復ごとに落ちている。**"""
    have = {k: v for k, v in g.items() if v[0]["eval_slope"] is not None}
    if not have:
        print()
        print("(os_eval の区間を測っていない。range_insns の列が無い)")
        return {}
    print()
    print("**os_eval 区間の命令数(インタプリタ落ちの検出)**")
    print()
    print("| 経路 | カテゴリ | 固定費(N 回側) | 傾き(命令/反復) | 判定 |")
    print("|---|---|---:|---:|---|")
    verdict = {}
    for (p, case) in sorted(have):
        rows = have[(p, case)]
        slopes = [r["eval_slope"] for r in rows]
        # 傾きは本来ちょうど 0 になる(os_eval へ落ちていなければ range_insns は
        # N に依存しない)。最大値で判定する: 1 回でも増えていれば疑う
        worst = max(slopes, key=abs)
        fixed = rows[0]["lo_eval"]
        ok = abs(worst) <= tol
        verdict[(p, case)] = (ok, worst, fixed)
        print(f"| {p} | {case} | {fixed} | {worst:+.3f} | "
              f"{'OK(落ちていない)' if ok else '**NG(反復ごとに落ちている)**'} |")
    return verdict


def print_cross(g, estimator, verdict):
    """経路をまたいだ比較。**どの経路の数字なのかを必ず並べて出す。**

    「経路」の列が無かったことが issue #114 を 165 コミット見逃した原因である。
    """
    cases = sorted({case for (_, case) in g})
    have = {k: estimates(v)[estimator] for k, v in g.items()}

    def mark(key):
        v = verdict.get(key)
        return "" if (v is None or v[0]) else "!"

    print()
    print(f"**経路間の比較(推定量 {estimator}。! は os_eval へ落ちている)**")
    print()
    print("| カテゴリ | C | AOT | JIT | AOT/C | JIT-AOT | JIT/AOT |")
    print("|---|---:|---:|---:|---:|---:|---:|")
    for case in cases:
        c = have.get(("c", case))
        a = have.get(("aot", case))
        j = have.get(("jit", case))
        cs = f"{c:.2f}{mark(('c', case))}" if c is not None else "-"
        as_ = f"{a:.2f}{mark(('aot', case))}" if a is not None else "-"
        js = f"{j:.2f}{mark(('jit', case))}" if j is not None else "-"
        ac = f"{a / c:.1f}x" if (a is not None and c) else "-"
        jd = f"{j - a:+.2f}" if (a is not None and j is not None) else "-"
        ja = f"{j / a:.2f}x" if (a and j is not None) else "-"
        print(f"| {case} | {cs} | {as_} | {js} | {ac} | {jd} | {ja} |")


def check(g, base, estimator, tight_band, verdict):
    print()
    print("=== 記録値との照合(検出器、双方向) ===")
    ng = []
    warn = []
    for (p, case) in sorted(g):
        e = estimates(g[(p, case)])
        v = verdict.get((p, case))
        if v is not None and not v[0]:
            ng.append((p, case,
                       f"**os_eval へ反復ごとに落ちている(傾き {v[1]:+.3f} "
                       f"命令/反復)。測定値は出さない。**"
                       f"この数字は JIT でも AOT でもない何かを測っている"))
            continue
        b = base.get((p, case))
        med = e[estimator]
        if b is None:
            print(f"[未記録] {p}/{case}: 測定値 {med:.2f} 命令/単位。"
                  f"bench_baseline.tsv に記録が無い")
            continue
        if b["band"] is None:
            print(f"[保留] {p}/{case}: 測定値 {med:.2f}(記録 {b['value']:.2f})。"
                  f"**この行は調査中で、基準値として使わない。**照合しない")
            continue
        if b["estimator"] != estimator:
            ng.append((p, case,
                       f"推定量が記録と違う(記録 {b['estimator']} / "
                       f"今回 {estimator})。**推定量が違う値は比較できない。**"
                       f"校正点を外す推定量もある"
                       f"(tools/bench/bench_calibration.tsv)"))
            continue
        # **試行回数に系統的に依存する推定量だけ、回数の一致も要求する。**
        # 最小値・最大値は回数を増やすと系統的に下がる/上がるので、
        # 3 回の最小値と 10 回の最小値は別の量である。
        # 中央値は回数に系統的に依存しないので、回数が違っても比較できる
        if estimator in TRIALS_DEPENDENT and b["trials"] != e["trials"]:
            ng.append((p, case,
                       f"試行回数が記録と違う(記録 {b['trials']} 回 / "
                       f"今回 {e['trials']} 回)。**推定量 {estimator} は"
                       f"試行回数を増やすと系統的にずれるので、"
                       f"回数が違う値は比較できない**"))
            continue
        if b["trials"] != e["trials"]:
            warn.append((p, case,
                         f"試行回数が記録と違う(記録 {b['trials']} 回 / "
                         f"今回 {e['trials']} 回)。{estimator} は回数に系統的には"
                         f"依存しないので比較はしたが、ばらつきの幅は変わる"))
        if b["n"] != e["n"]:
            ng.append((p, case,
                       f"N が記録と違う(記録 {b['n']} / 今回 {e['n']})。"
                       f"N が違う傾きは比較できない"))
            continue
        diff = med - b["value"]
        if abs(diff) <= b["band"]:
            print(f"[OK] {p}/{case}: {med:.2f} (記録 {b['value']:.2f} "
                  f"±{b['band']:.2f}, 差 {diff:+.2f})")
            # 細い帯は、その行の帯より細いときだけ意味がある
            # (C 行は帯 0.50 なので 7.00 の細い帯は発火しない)
            if (e["trials"] >= 3 and tight_band < b["band"]
                    and abs(diff) > tight_band):
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

  1. **os_eval へ落ちていると出たとき**は、記録を直す話ではない。
     その数字は「JIT の性能」でも「AOT の性能」でもない。
     どの部分式が落ちているかを特定し、documents/jit-unsupported-syntax.md へ
     足すこと。**帯を広げてごまかさないこと。**
  2. **数字がずれたときは、まず測定を疑う。** 同じコマンドをもう一度流す。
     差が再現しなければノイズである(規則 4)。
  3. 再現したら、**それが意図した変更かどうかを決める。**
     - 意図した変更なら、tools/bench/bench_baseline.tsv の該当行を
       新しい値・新しいリビジョン・新しい測定日で**書き換えてコミットする。**
       経路・単位・推定量・試行回数・N・帯の列も合っているか見ること。
     - 意図しない変更なら、それが今回見つけたい退行(または改善)である。
       **数字を黙って更新しないこと。**
  4. 「速くなった」で落ちたときも同じ手順を踏む。記録が古いか、
     測定が壊れているか、本物の改善かは、測り直さないと決まらない。
""")
    return 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--results", required=True)
    ap.add_argument("--baseline")
    ap.add_argument("--estimator", default="median",
                    choices=ESTIMATOR_NAMES,
                    help="記録・照合に使う推定量(既定 median)。"
                         "**どれを選んでも 5 つとも出力に出る。**"
                         "校正点(c/for の真値 4.000)を外す推定量を選ぶと落ちる")
    ap.add_argument("--no-check", action="store_true",
                    help="集計だけして記録値との照合をしない(基準値を取り直すとき)")
    ap.add_argument("--tight-band", type=float, default=7.0,
                    help="繰り返しが 3 回以上あるときに追加で見る細い帯"
                         "(既定 7.0 命令/単位)。超えても落とさず警告する。"
                         "**min3 では本体の帯 18.0 に対して 7.0 しか詰められない。**"
                         "median3 なら 3.0 まで詰められた(推定量を変えた代償)")
    ap.add_argument("--eval-tol", type=float, default=0.5,
                    help="os_eval の傾きの許容値(既定 0.5 命令/反復)。"
                         "本来ちょうど 0 になる")
    ap.add_argument("--no-eval-gate", action="store_true",
                    help="os_eval のゲートを外す(**陽性対照専用**)")
    ap.add_argument("--calibration", default="tools/bench/bench_calibration.tsv",
                    help="真の値が分かっている行(校正点)の表")
    ap.add_argument("--no-calibration", action="store_true",
                    help="校正点の検査を省く(**推定量の比較実験専用**)")
    args = ap.parse_args()

    rows = read_results(args.results)
    if not rows:
        sys.exit("ERROR: 測定結果が空です")
    g = group(rows)

    print_measured(g, args.estimator)

    cal_rc = 0
    if not args.no_calibration:
        try:
            cal = read_calibration(args.calibration)
        except FileNotFoundError:
            print()
            print(f"(校正点の表 {args.calibration} が無い)")
            cal = {}
        cal_rc = check_calibration(g, cal, args.estimator)

    verdict = print_eval(g, args.eval_tol)
    if args.no_eval_gate:
        print()
        print("(--no-eval-gate: os_eval のゲートを外している。陽性対照専用)")
        verdict = {}
    print_cross(g, args.estimator, verdict)

    if args.no_check or not args.baseline:
        print()
        print("(--no-check: 記録値との照合をしていません)")
        # ゲートだけは効かせる。測定値を出してよいかの判断はこちらが持つ
        bad = [k for k, v in verdict.items() if not v[0]]
        if bad:
            print()
            print("=== os_eval のゲートが落ちました ===")
            for p, case in sorted(bad):
                print(f"[NG] {p}/{case}: 反復ごとに os_eval へ落ちている"
                      f"(傾き {verdict[(p, case)][1]:+.3f} 命令/反復)")
            return 1
        return cal_rc
    return check(g, read_baseline(args.baseline), args.estimator,
                 args.tight_band, verdict) or cal_rc


if __name__ == "__main__":
    sys.exit(main())
