#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""試験件数の点検(#202 / T1)。**判定はしない。数えるだけの道具である。**

規則 13(R-count)に従い、報告に書く件数を手で数えずに導出できる形にしてある。
**この道具のモードは `make` からも CI からも呼ばれない。合否に一切影響しない。**

ただし [#208] 以降、**コメントを剥がす 3 つの関数だけ**は
`tools/check_test_count_ratchet.py`(= `make test` が呼ぶ検査)が取り込んで使う
(`strip_lisp` / `strip_c_comments` / `strip_make_comments`)。
剥がし方が 2 本に分かれると**道具によって「走っている」の答えが違う**状態になり、
それは #208 の誤検出とまったく同じ族だからである。
したがって**この 3 つを壊すと `make test` が落ちる。** モードのほうは落ちない。

2 つのモード:

  --static <台本...>   台本(qemu_boot_*.lisp)が読む試験ファイルを辿り、
                       アサーションの静的な出現数から**期待件数**を出す。
  --log <ログ>         `make test-qemu-all` の出力ログから、段ごとの
                       **実際の件数**(#count 行と合否の一行)を取り出す。
  --compare <ログ>     上の 2 つを突き合わせた表を出す。

期待件数の定義:
  段の期待(= 合否の一行の passed+failed)
    = (台本が読む試験ファイルのアサーション出現数の合計)
      + 2 × (isiki-test-load で読むファイルの本数)
  ファイルの期待(= #count 行の値)
    = そのファイルのアサーション出現数(**+2 はしない**)

  **#count は end-check の 2 件が走る前に出る**(test_framework.lisp:237-238 で
  n を確定してから :239-240 の assert-equal 2 件が走る)。したがって
  2 件は「どの #count にも入らず、合計にだけ入る」。
"""

import argparse
import os
import re
import sys

# **数えるアサーション。** assert-output は**数えない**: 集計を一切触らない
# 入れ物のマクロで、with-standard-output + let に展開するだけである
# (test/lisp/test_framework.lisp:321-327)。中に書いた assert-equal 等が数えられる。
ASSERT_NAMES = ("assert-equal", "assert-error-class", "assert-error", "assert-float-close")
HELPER_NAMES = ("assert-output",)

# この中に書かれたアサーションは「読み込むだけでは走らない」。
#   defun   … 呼ばれない身代わり(例: test/lisp/init_test.lisp:20 の
#             should-not-be-called-and。呼ばれたら落ちるために置いてある)
#   lambda  … 集計を元に戻す捕獲(例: test/lisp/test_framework_test.lisp:72 の
#             tf-capture。走るが pass/fail を復元するので #count に出ない)
#   defmacro… 展開されなければ走らない
DEFER_HEADS = ("defun", "defmacro", "lambda")

TOKEN_RE = re.compile(r"[^\s()'`,;\"]+")

# 台本が読む行。isiki-test-load は件数の証人つき、素の load は証人なし。
LOAD_RE = re.compile(r"\((isiki-test-load|load)\s+\"([^\"]+)\"\s*\)")

FRAMEWORK = "test/lisp/test_framework.lisp"

# 1 ファイルあたり isiki-file-end-check が数える件数
END_CHECK_ASSERTS = 2


def strip_lisp(text, keep_strings=False):
    """行コメントを取り除く。keep_strings が偽なら文字列リテラルも取り除く。

    `#\\;` (セミコロンの文字リテラル)をコメント開始と誤認しないこと、
    文字列の中の `;` や `(assert-...` を数えないことの両方が要る。
    """
    out = []
    i = 0
    n = len(text)
    in_string = False
    while i < n:
        c = text[i]
        if in_string:
            if c == "\\":
                i += 2
                continue
            if c == '"':
                in_string = False
            if keep_strings:
                out.append(c)
            i += 1
            continue
        # 文字リテラル #\x は 3 文字まとめて読み飛ばす(#\; #\" を守る)
        if c == "#" and i + 2 < n and text[i + 1] == "\\":
            out.append("   ")
            i += 3
            continue
        if c == '"':
            in_string = True
            out.append(c if keep_strings else " ")
            i += 1
            continue
        if c == ";":
            # 行末まで捨てる(改行は残す)
            j = text.find("\n", i)
            if j < 0:
                break
            out.append("\n")
            i = j + 1
            continue
        out.append(c)
        i += 1
    return "".join(out)


def scan_asserts(src):
    """トップレベルで必ず走るアサーション数と、除外した内訳を返す。

    返り値 (counted, deferred, helpers):
      counted  … #count 行に出るはずの件数
      deferred … DEFER_HEADS の中にあって走らない(か集計に出ない)件数
      helpers  … assert-output の出現数(それ自身は 0 件)
    """
    stack = []
    counted = deferred = helpers = 0
    i, n = 0, len(src)
    while i < n:
        ch = src[i]
        if ch == "(":
            m = TOKEN_RE.match(src, i + 1)
            head = m.group(0) if m else ""
            if head in HELPER_NAMES:
                helpers += 1
            elif head in ASSERT_NAMES:
                if any(h in DEFER_HEADS for h in stack):
                    deferred += 1
                else:
                    counted += 1
            stack.append(head)
        elif ch == ")":
            if stack:
                stack.pop()
        i += 1
    return counted, deferred, helpers


def count_asserts(path):
    with open(path, encoding="utf-8") as f:
        return scan_asserts(strip_lisp(f.read()))[0]


def count_asserts_detail(path):
    with open(path, encoding="utf-8") as f:
        return scan_asserts(strip_lisp(f.read()))


def toplevel_form_count(path):
    """最上位フォームの数。1 なら「丸ごと消えうる」構造である(§0-D)。"""
    with open(path, encoding="utf-8") as f:
        src = strip_lisp(f.read())
    depth = 0
    forms = 0
    for c in src:
        if c == "(":
            if depth == 0:
                forms += 1
            depth += 1
        elif c == ")":
            if depth > 0:
                depth -= 1
    return forms


# **自前の集計を持つ段**(共通の枠組みを使えない。logic.md §0-1)。
# 基盤ライブラリ未読込の状態そのものを要件とするので枠組みが読めず、
# ck という自前の確認手続きで 1 件ずつ数えて合否の一行を自分で組む
# (test/lisp/qemu_boot_no_init_signal.lisp:30-44,114)。
SELF_TALLY = {"test/lisp/qemu_boot_no_init_signal.lisp": re.compile(r"^\(ck ", re.M)}


def analyze_script(script, root):
    """台本 1 本の期待件数と内訳。"""
    with open(os.path.join(root, script), encoding="utf-8") as f:
        src = strip_lisp(f.read(), keep_strings=True)
    files = []
    # **台本自身に書かれたトップレベルのアサーションも合計に入る。**
    # 例: test/lisp/qemu_boot_test.lisp の
    #     (assert-equal t *isiki-test-syntax-load-result*)
    script_own = scan_asserts(src)[0]
    total = script_own
    if script in SELF_TALLY:
        # 自前集計。assert-* ではなく ck の出現数が期待件数である
        with open(os.path.join(root, script), encoding="utf-8") as f:
            return {
                "script": script,
                "expected": len(SELF_TALLY[script].findall(f.read())),
                "script_own": 0,
                "witnessed_files": 0,
                "self_tally": True,
                "files": [],
            }
    witnessed_files = 0
    for how, path in LOAD_RE.findall(src):
        if path == FRAMEWORK:
            continue  # 枠組み自身。アサーションは end-check の 2 件として足す
        full = os.path.join(root, path)
        if not os.path.exists(full):
            files.append((how, path, None, None))
            continue
        n = count_asserts(full)
        tl = toplevel_form_count(full)
        witnessed = how == "isiki-test-load"
        if witnessed:
            witnessed_files += 1
            n_eff = n + END_CHECK_ASSERTS
        else:
            n_eff = n
        total += n_eff
        files.append((how, path, n, tl))
    return {
        "script": script,
        "expected": total,
        "script_own": script_own,
        "witnessed_files": witnessed_files,
        "files": files,
    }


# 9P を外した段は Lisp の枠組みを使わない。件数は C 側の確認手続きの
# 呼び出し箇所の数そのものである(src/c/kernel.c の run_boot_smoke)。
# Makefile:1177 がこの数を grep に直書きしているので、両者がずれていないかも見る。
SMOKE_CK_RE = re.compile(r"^\s+smoke_ck\(", re.M)


def no9p_expected(root="."):
    with open(os.path.join(root, "src/c/kernel.c"), encoding="utf-8") as f:
        return len(SMOKE_CK_RE.findall(f.read()))


SUMMARY_RE = re.compile(r"==== isiki tests: (\d+) passed, (\d+) failed ====")
NO9P_RE = re.compile(r"\[no9p\] (\d+) passed, (\d+) failed")
COUNT_RE = re.compile(r"^#count (\S+) (\d+)\s*$")
MILESTONE_RE = re.compile(r"MILESTONE=(\S+)")
DISKIMG_RE = re.compile(r"QEMU_DISK_IMG=(\S+)")
MAKE_TARGET_RE = re.compile(r"^(?:\s*)(?:\$\(MAKE\)|make)\s+((?:\S+=\S+\s+)*)(test-qemu\S*)")
# 段を手で並べて回すとき(CI 専用の段など)の印。README 的な約束:
#   for m in ...; do echo "### STAGE $m"; make test-qemu-milestone MILESTONE=...; done
STAGE_MARK_RE = re.compile(r"^### STAGE (\S+)\s*$")


def parse_log(logpath):
    """ログから段ごとの実測を取り出す。

    段の区切りは make が echo する再帰呼び出しの行。段の終わりは合否の一行。
    証人の無い段は #count 行が 1 行も無いので、counts が空のまま閉じる。
    """
    stages = []
    stage = None
    counts = []
    with open(logpath, encoding="utf-8", errors="replace") as f:
        for line in f:
            sm = STAGE_MARK_RE.match(line)
            if sm:
                stage = "test/lisp/qemu_boot_%s.lisp" % sm.group(1)
                continue
            m = MAKE_TARGET_RE.match(line)
            if m:
                target = m.group(2)
                ms = MILESTONE_RE.search(line)
                img = DISKIMG_RE.search(line)
                if ms:
                    name = ms.group(1)
                    if img:
                        name += " [" + os.path.basename(img.group(1)) + "]"
                else:
                    name = target
                stage = name
                continue
            mc = COUNT_RE.match(line)
            if mc:
                counts.append((mc.group(1), int(mc.group(2))))
                continue
            ms = SUMMARY_RE.search(line) or NO9P_RE.search(line)
            if ms:
                stages.append(
                    {
                        "stage": stage or "(不明)",
                        "passed": int(ms.group(1)),
                        "failed": int(ms.group(2)),
                        "counts": counts,
                        "no9p": bool(NO9P_RE.search(line)),
                    }
                )
                counts = []
    if counts:
        stages.append(
            {
                "stage": (stage or "(不明)") + " **合否の一行が無い**",
                "passed": None,
                "failed": None,
                "counts": counts,
                "no9p": False,
            }
        )
    return stages


# test-qemu-all の段 → 台本 の対応。Makefile:1257-1282 の順序。
# MILESTONE= を持たない段(自前のレシピ)だけここで補う。
TARGET_TO_SCRIPT = {
    "test-qemu": "test/lisp/qemu_boot_test.lisp",
    "test-qemu-no-embedded-init": "test/lisp/qemu_boot_no_init_signal.lisp",
    "test-qemu-no-9p": None,  # C 側(run_boot_smoke)。件数は判定に直書き
    "test-qemu-no-9p-run": None,
    "test-qemu-load-disk": "test/lisp/qemu_boot_load_disk.lisp",
    "test-qemu-error-escape-gc": "test/lisp/qemu_boot_error_escape_gc.lisp",
    "test-qemu-load-gc-stress": "test/lisp/qemu_boot_load_gc_stress.lisp",
    "test-qemu-bench-jit": "test/lisp/qemu_boot_bench_jit.lisp",
    "test-qemu-bench-interp": "test/lisp/qemu_boot_bench_interp.lisp",
    "test-qemu-heap-exhaustion": "test/lisp/qemu_boot_heap_exhaustion.lisp",
    "test-qemu-compare-single": "test/lisp/qemu_boot_compare_single.lisp",
    "test-qemu-compare-fixnum": "test/lisp/qemu_boot_compare_fixnum.lisp",
    "test-qemu-inline-muldiv": "test/lisp/qemu_boot_inline_muldiv.lisp",
    "test-qemu-safety3": "test/lisp/qemu_boot_safety3.lisp",
}


def stage_to_script(stage):
    base = stage.split(" [")[0]
    if base.endswith(".lisp"):
        return base
    return TARGET_TO_SCRIPT.get(base, "__unknown__")


DEFUN_NAME_RE = re.compile(r"\(defun\s+(\S+)")


def deferred_audit(root="."):
    """**入れ物の中のアサーションを 1 件ずつ洗い出す。**

    静的な数え方がいちばん外しやすい場所である。3 つに分かれる:
      (a) 呼ばれない身代わり  … 実行時 0 件(静的に数えると過大)
      (b) 集計を復元する捕獲  … 実行時 0 件(同上)
      (c) 複数回呼ばれる補助   … 実行時 = 本体の件数 × 呼び出し回数(**過小**)
    (c) があるファイルは素の出現数では期待件数が出せない。
    """
    rows = []
    for f in sorted(os.listdir(os.path.join(root, "test/lisp"))):
        if not f.endswith(".lisp"):
            continue
        path = "test/lisp/" + f
        src = strip_lisp(open(os.path.join(root, path), encoding="utf-8").read())
        counted, deferred, helpers = scan_asserts(src)
        if deferred == 0 and helpers == 0:
            continue
        # defun 名ごとに本体の assert 数を数え、同じファイル内の呼び出し箇所を数える
        bodies = {}
        stack = []
        cur = None
        curdepth = None
        i, n = 0, len(src)
        while i < n:
            ch = src[i]
            if ch == "(":
                m = TOKEN_RE.match(src, i + 1)
                head = m.group(0) if m else ""
                if head == "defun":
                    m2 = DEFUN_NAME_RE.match(src, i)
                    cur = m2.group(1) if m2 else "?"
                    curdepth = len(stack)
                    bodies.setdefault(cur, 0)
                elif head in ASSERT_NAMES and cur is not None:
                    bodies[cur] = bodies.get(cur, 0) + 1
                stack.append(head)
            elif ch == ")":
                if stack:
                    stack.pop()
                if curdepth is not None and len(stack) <= curdepth:
                    cur = None
                    curdepth = None
            i += 1
        helpers_called = {}
        for nm, v in bodies.items():
            if v == 0:
                continue
            occ = len(re.findall(r"\(" + re.escape(nm) + r"[\s)]", src))
            helpers_called[nm] = (v, occ)
        rows.append((path, counted, deferred, helpers, helpers_called))
    return rows


def strip_c_comments(text):
    """C の `//` と `/* */` を剥がす。**文字列リテラルの中は守る。**

    [#208] コメントの中の名前を数えると**偽陽性**が出る。実例:
    `test/c/repl_test.c` のコメントは `test/lisp/toplevel_abort_test.lisp` の
    名前を含むが、その台本を走らせているのは別の段である。
    """
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == '"':
            out.append(c)
            i += 1
            while i < n:
                if text[i] == "\\":
                    out.append("  ")
                    i += 2
                    continue
                out.append(text[i])
                if text[i] == '"':
                    i += 1
                    break
                i += 1
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            while i < n and text[i] != "\n":
                i += 1
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            i = n if j < 0 else j + 2
            continue
        out.append(c)
        i += 1
    return "".join(out)


def strip_make_comments(text):
    """**行まるごとのコメント**(空白のみ先行する `#`)を落とす。

    [#208] 行の途中の `#` は落とさない。レシピには `sed -n 's/^#gate //p'` の
    ように `#` を含む本物のコマンドがあり、そこから先を切ると
    **本当に走っている段が「走らない」に化ける**(= 新しい偽陰性を作る)。
    """
    return "\n".join(
        "" if ln.lstrip().startswith("#") else ln for ln in text.split("\n")
    )


# [#208] 母艦側の単体試験が Lisp の台本を走らせる呼び出し。
# **呼び出しの形だけを見る**(コメントの中の名前は数えない)
RUN_LISP_FILE_RE = re.compile(r'run_lisp_file\s*\([^;{}]*?"([^"]+)"\s*\)')


def host_unit_tests(root="."):
    """母艦側の単体試験(`test/c/*.c`)が `run_lisp_file` で走らせる台本。

    [#208] **これが元の誤検出の真因である。** 「どこで走るか」の導出が
    `Makefile` と継続的検査の 2 経路しか見ていなかったので、
    `test/lisp/qemu_boot_diag_m12_host.lisp`(`test/c/script_test.c:332` が
    毎回読んでいて `make test` で必ず走る)を
    **「どこからも走らない」と誤判定していた。**
    その誤判定が #208 の「7 本」という報告を作った。
    """
    out = {}
    testc = os.path.join(root, "test/c")
    if not os.path.isdir(testc):
        return out
    for f in sorted(os.listdir(testc)):
        if not f.endswith(".c"):
            continue
        rel = "test/c/" + f
        with open(os.path.join(root, rel), encoding="utf-8") as fh:
            src = strip_c_comments(fh.read())
        for target in RUN_LISP_FILE_RE.findall(src):
            out.setdefault(target, []).append(rel)
    return out


def where_it_runs(root="."):
    """台本 → どこで走るか。**定義から導出する**(手で書かない)。

    起点は **3 種類**ある。2 種類しか見ていなかったのが #208 の誤検出の真因:

    - `make test-qemu-all` のレシピ本体に出てくるもの
    - `.github/workflows/ci.yml` に出てくるもの
    - **母艦側の単体試験(`test/c/*.c`)が `run_lisp_file` で読むもの**(#208 で追加)
    - 上のどれでもないが Makefile のどこかには出るもの = 手で走らせる段

    起点を足したいときは、**足したことがここの docstring と出力に出る**ようにすること。
    知らない起点を黙って「無い」と断定するのが穴である。
    """
    mk = strip_make_comments(
        open(os.path.join(root, "Makefile"), encoding="utf-8").read()
    )
    ci = strip_make_comments(
        open(os.path.join(root, ".github/workflows/ci.yml"), encoding="utf-8").read()
    )
    hosts = host_unit_tests(root)
    # test-qemu-all のレシピ(タブで始まる行が続く範囲)
    i = mk.index("\ntest-qemu-all:")
    j = i + 1
    lines = mk[j:].split("\n")
    recipe = []
    for k, ln in enumerate(lines):
        if k == 0:
            continue
        if ln.startswith("\t"):
            recipe.append(ln)
        elif ln.strip() == "":
            # 空行はレシピを終わらせない。**行まるごとのコメントも同じ**で、
            # 上の strip_make_comments が空行にしてある(#208。レシピの中に
            # コメントを書くと、ここで段の並びが途中で切れて
            # 「それ以降が走らない」に化けていた)
            continue
        else:
            break
    all_text = "\n".join(recipe)
    # test-qemu-all が呼ぶ中間ターゲットも辿る(test-qemu-load-gc-stress 等)
    seen = set()
    pending = re.findall(r"\$\(MAKE\)\s+((?:\S+=\S+\s+)*)(test-qemu[\w-]*)", all_text)
    texts = [all_text]
    queue = [t for _, t in pending]
    while queue:
        t = queue.pop()
        if t in seen:
            continue
        seen.add(t)
        m = re.search(r"\n" + re.escape(t) + r":[^\n]*\n((?:\t[^\n]*\n)+)", mk)
        if m:
            texts.append(m.group(1))
            queue += [x for _, x in re.findall(
                r"\$\(MAKE\)\s+((?:\S+=\S+\s+)*)(test-qemu[\w-]*)", m.group(1))]
    joined = "\n".join(texts)
    # `make test-qemu` は台本の名前をコマンド行に書かない。空の引き金を置き、
    # ゲスト側の既定値(src/c/kernel.c:91 の g_qemu_test_boot_script)が使われる。
    if "test-qemu\n" in mk or re.search(r"\ntest-qemu:", mk):
        with open(os.path.join(root, "src/c/kernel.c"), encoding="utf-8") as f:
            m = re.search(r'g_qemu_test_boot_script\[\d+\]\s*=\s*"([^"]+)"', f.read())
        if m:
            joined += "\n" + m.group(1)
    out = {}
    for f in sorted(os.listdir(os.path.join(root, "test/lisp"))):
        if not (f.startswith("qemu_boot_") and f.endswith(".lisp")):
            continue
        path = "test/lisp/" + f
        where = []
        if path in joined:
            where.append("all")
        if path in ci:
            where.append("CI")
        if path in hosts:
            # [#208] 第 3 の起点。母艦側の単体試験が読むものは
            # **`make test` で毎回走る**(QEMU も要らない)
            where.append("母艦 " + ",".join(hosts[path]))
        if not where:
            # Makefile に名前があれば「手で打つ段(make のターゲットはある)」。
            # 無ければどの起点からも辿れず、文書の手順として残っているだけ
            where.append("手" if path in mk else "**どの起点にも無い**")
        out[path] = where
    return out



# ---------------------------------------------------------------------------
# [#203 / T2] **ファイル単位の名簿の初期値を導出して刷る。**
#
# `tools/check_test_count_ratchet.py` の `FILES` は**手で数えた数字を 1 つも
# 置かない**(規則 13)。初期値はこのモードの出力をそのまま貼ったものである。
#
#     python3 tools/census_test_counts.py --file-roster
#
# **これは生成器であって権威ではない。** 権威は貼ったあとの literal のほうで、
# 照合器はこの道具を実行時に呼ばない(§7-3 の「推定を権威にするな」)。
# 静的な数え上げが壊れても、literal が実機のログと合わなくなる形で鳴る。

# 自前集計の段の 1 件 1 行の名前(台本の ck の呼び出し)
SELF_CK_LABEL_RE = re.compile(r'^\(ck\s+"([^"]+)"', re.M)
# C 側の起動スモークの 1 件 1 行の名前(src/c/kernel.c の smoke_ck)
SMOKE_CK_LABEL_RE = re.compile(r'^\s+smoke_ck\("([^"]+)"', re.M)


def file_roster(root="."):
    """段ごとの「1 件 1 行の証人」の一覧と件数を導出する。

    返り値: [(段, 読み方, 台本自身の件数, [(名前, 件数), ...], 導出した段の合計)]

    証人の形は読み方で違う。**3 つとも「名前つきで 1 件 1 行」である**ことが
    ファイル単位(あるいは項目単位)の特定を成り立たせている:
      isiki      … `#count <ファイル> <件数>`(isiki-test-load が出す)
      self-tally … `#ck <名前>`(台本の ck)
      no9p       … `[no9p] ok <名前>`(C 側の smoke_ck)
    """
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import check_test_count_ratchet as ratchet

    out = []
    for stage, _count, reader, _rounds in ratchet.COUNTS:
        script = stage_to_script(stage)
        if reader == "no9p":
            with open(os.path.join(root, "src/c/kernel.c"), encoding="utf-8") as f:
                items = [(n, 1) for n in SMOKE_CK_LABEL_RE.findall(f.read())]
            own = 0
            total = sum(n for _, n in items)
        elif reader == "self-tally":
            with open(os.path.join(root, script), encoding="utf-8") as f:
                items = [(n, 1) for n in SELF_CK_LABEL_RE.findall(f.read())]
            own = 0
            total = sum(n for _, n in items)
        else:
            a = analyze_script(script, root)
            items = [(p, n) for how, p, n, _ in a["files"] if how == "isiki-test-load"]
            own = a["script_own"]
            # **出口の検査自身の 2 件**が 1 本あたり加わる(§2-1 の定義)
            total = sum(n for _, n in items) + END_CHECK_ASSERTS * len(items) + own
        out.append((stage, reader, own, items, total))
    return out

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=".")
    ap.add_argument("--static", nargs="*", help="台本のパス")
    ap.add_argument("--log", help="make test-qemu-all のログ")
    ap.add_argument("--compare", help="ログを静的な期待件数と突き合わせる")
    ap.add_argument("--detail", action="store_true", help="ファイル単位の内訳も出す")
    ap.add_argument("--inventory", action="store_true", help="全台本の棚卸し表を出す")
    ap.add_argument("--per-file", help="ログの #count 行を 1 ファイルずつ照合する")
    ap.add_argument("--file-roster", action="store_true",
                    help="ファイル単位の名簿(check_test_count_ratchet.py の FILES)の初期値を刷る")
    ap.add_argument("--deferred-audit", action="store_true",
                    help="入れ物(defun/lambda/defmacro)の中のアサーションを洗い出す")
    args = ap.parse_args()

    if args.file_roster:
        print("FILES = {")
        for stage, reader, own, items, total in file_roster(args.root):
            print('    # %s … 読み方 %s / 台本自身 %d 件 / 証人 %d 本 / 導出の合計 %d 件'
                  % (stage, reader, own, len(items), total))
            print('    "%s": (%d, (' % (stage, own))
            for name, n in items:
                print('        ("%s", %d),' % (name, n))
            print("    )),")
        print("}")
        return 0

    if args.per_file:
        ok = bad = 0
        rows = []
        for st in parse_log(args.per_file):
            script = stage_to_script(st["stage"])
            if script in (None, "__unknown__") or not st["counts"]:
                continue
            a = analyze_script(script, args.root)
            exp = {p: n for how, p, n, _ in a["files"] if n is not None}
            for path, n in st["counts"]:
                e = exp.get(path)
                if e == n:
                    ok += 1
                else:
                    bad += 1
                    rows.append((st["stage"], path, e, n))
        print("ファイル単位の照合: 一致 %d 件 / 食い違い %d 件" % (ok, bad))
        for stg, path, e, n in rows:
            print("  %-46s %-44s 期待 %s 実際 %d" % (stg, path, e, n))
        return 0

    if args.deferred_audit:
        print("| 試験ファイル | 数えた件数 | 入れ物の中 | assert-output | 件数を持つ defun(本体の件数, 出現回数) |")
        print("|---|---:|---:|---:|---|")
        for path, k, d, h, hc in deferred_audit(args.root):
            desc = ", ".join("`%s`(%d, %d)" % (nm, v, o) for nm, (v, o) in hc.items())
            print("| `%s` | %d | %d | %d | %s |" % (os.path.basename(path), k, d, h, desc or "—"))
        return 0

    if args.inventory:
        wr = where_it_runs(args.root)
        print("| 台本 | 期待件数 | 証人つき本数 | 証人なし本数 | どこで走るか |")
        print("|---|---:|---:|---:|---|")
        for path, where in wr.items():
            a = analyze_script(path, args.root)
            nw = sum(1 for how, _, n, _ in a["files"] if how == "load" and n is not None)
            note = "自前集計" if a.get("self_tally") else ""
            print("| `%s` | %d%s | %d | %d | %s |" % (
                os.path.basename(path), a["expected"],
                (" (" + note + ")") if note else "",
                a["witnessed_files"], nw, " / ".join(where)))
        return 0

    if args.static is not None:
        scripts = args.static or sorted(
            os.path.join("test/lisp", f)
            for f in os.listdir(os.path.join(args.root, "test/lisp"))
            if f.startswith("qemu_boot_") and f.endswith(".lisp")
        )
        print("| 台本 | 期待件数 | 証人つき本数 | 証人なし本数 |")
        print("|---|---:|---:|---:|")
        for s in scripts:
            a = analyze_script(s, args.root)
            nw = sum(1 for how, _, n, _ in a["files"] if how == "load" and n is not None)
            print(
                "| `%s` | %d | %d | %d |"
                % (os.path.basename(a["script"]), a["expected"], a["witnessed_files"], nw)
            )
            if args.detail:
                for how, path, n, tl in a["files"]:
                    if n is None:
                        print("|   └ `%s` | **見つからない** | | |" % path)
                    else:
                        print(
                            "|   └ `%s` | %d%s | %s | 最上位 %d |"
                            % (
                                os.path.basename(path),
                                n,
                                " (+2)" if how == "isiki-test-load" else "",
                                "証人あり" if how == "isiki-test-load" else "**証人なし**",
                                tl,
                            )
                        )
        return 0

    target = args.log or args.compare
    if not target:
        ap.error("--static か --log か --compare のどれかを指定すること")

    stages = parse_log(target)
    if args.log and not args.compare:
        for st in stages:
            print(
                "%-62s passed=%s failed=%s  #count行=%d 件(合計 %d)"
                % (
                    st["stage"],
                    st["passed"],
                    st["failed"],
                    len(st["counts"]),
                    sum(n for _, n in st["counts"]),
                )
            )
        return 0

    print("| # | 段 | 実際(passed/failed) | 期待 | 証人(#count の合計 / 行数) | 判定 |")
    print("|---:|---|---:|---:|---|---|")
    for i, st in enumerate(stages, 1):
        script = stage_to_script(st["stage"])
        if script is None:
            exp = no9p_expected(args.root)
            actual = st["passed"]
            verdict = "一致(C 側 smoke_ck の数から導出)" if actual == exp else "**食い違い**"
        elif script == "__unknown__" or not os.path.exists(
            os.path.join(args.root, script)
        ):
            exp = "?"
            verdict = "**台本が特定できない**"
        else:
            a = analyze_script(script, args.root)
            exp = a["expected"]
            actual = st["passed"]
            if actual is None:
                verdict = "**合否の一行が無い**"
            elif actual == exp:
                verdict = "一致"
            elif actual < exp:
                verdict = "**不足 %d**" % (exp - actual)
            else:
                verdict = "超過 +%d" % (actual - exp)
        wit = (
            "%d / %d 行" % (sum(n for _, n in st["counts"]), len(st["counts"]))
            if st["counts"]
            else "**無証人**"
        )
        print(
            "| %d | `%s` | %s / %s | %s | %s | %s |"
            % (i, st["stage"], st["passed"], st["failed"], exp, wit, verdict)
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
