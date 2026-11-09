#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""**合否の一行そのものが信用できるか**を見る(#205 / T4)。

## なぜ必要か

実機の段の合否は、成果物(`test-results.txt`)の最後に出る 1 行

    ==== isiki tests: <通った件数> passed, <落ちた件数> failed ====

だけで決まっていた。これは**ただの文字列**なので、書ける者は誰でも書ける。
調査(`docs/investigation/detector-t4-passline.logic.md` §9-1)で、偽造できる形が
**3 つ**あることが分かっている:

| # | 形 | 判定を通るか |
|---|---|---|
| (1) | **部分一致に当てる。** 判定役は成果物の「どこかに」`" 0 failed"` があればよいとしていた。本物の合否の一行が `5 failed` でも、別の行にその文字列を出せば通る | 通る(本文書の `--self-test` で実測した) |
| (2) | **一行を直接書く。** 枠組みを使わず合格の一行を自分で組んで成果物へ書く。検査 0 件でも合格と言える | 通る |
| (3) | **集計変数を戻す。** 意図的に出した不合格を `setq` で元の値へ戻す | 通る |

**決心した書き手による偽造は止められない。** 一行は結局ただの文字列である。
だから目標は「止める」ではなく **「偶然には偽造できないようにし、
意図的にやったら目に見えるようにする」**(= 在庫を固定して、増えたら落ちる)。
既にこの企画で使っている形(`tools/check_eval_error_budget.py` の予算、
`tools/check_jit_bail_lines.py` の印)と同じ考え方である。

**件数の照合はこの道具の仕事ではない。** それは `tools/check_test_count_ratchet.py`
(#204)が段ごとの名簿として持っている。こちらは「一行そのもの」を見る。

## 3 つをどう塞いでいるか

- **(1) 判定を行まるごとの一致にし、位置も見る** —— `--verify`。
  口は 3 つに分かれている(調査 §A-4)。`tools/run_qemu_milestone.sh` と
  `Makefile` の `test-qemu` が**この 1 箇所を呼ぶ**形に寄せた。
  残る 1 つ(9P の無い起動)は合否の一行を出さない段で、件数照合器が見ている
- **(2) 合否の一行を書く箇所の在庫を固定する** —— 下の `WRITERS`。
  増えたら落ちる。**減っても落ちる**(規則 16。黙って通すと計器が形骸化する)
- **(3) 集計を減らす代入の在庫を固定する** —— 下の `RESTORES`。
  **増やす向き(不合格を増やす)は安全側なので数えない。**
  実際に増やすだけの箇所も 3 つあり、そちらは偽造にならない(調査 §A-2)

## 印(分類)の作法 —— ソースのその行にコメントで置く

成果物の中に印を置くと**印そのものが偽造できる**(成果物はゲストが書く出力である)。
だから印はソース側に置き、母艦がソースを読んで照合する。`BAIL-TAG` と同じ作法。

    PASS-LINE: <分類> <理由>        合否の一行を書く箇所の分類
    TALLY-RESTORE: <理由>          集計を減らす代入の印

分類は 4 つだけ:

| 分類 | 意味 |
|---|---|
| `REAL` | 本物の集計が書く一行(枠組み / C 側の起動スモーク) |
| `SELF-TALLY` | 枠組みが使えない前提の段が自前の集計で書く一行 |
| `PROBE` | 計測プローブが判定を通すために書く固定文字列。**在庫を増やさない** |
| `FIXTURE` | **判定役・比較器にかけるための偽の一行**(陽性対照)。偽造ではない |

### 印が乱用されないようにしてあること(#205 の完了条件)

**印を付ければ何でも通る形にはしていない。** 4 つの縛りがある:

1. **在庫は (ファイル, 分類, 件数) で固定する。** 印を付けても、名簿に無いファイルや
   名簿より多い件数は落ちる。「印を付けて 4 本目を足す」は通らない
2. **`FIXTURE` は自己試験を持つファイルだけ**に許す(`--self-test` / `self-test` の
   入口があること)。試験ファイルや台本が自分を「判定役の資料」と名乗れない
3. **`PROBE` は `tools/` の下だけ、`REAL`/`SELF-TALLY` は `test/lisp/` と `src/c/` だけ**。
   置き場所で役を縛る
4. **印には理由を書かせる。** 空の印は落ちる

この 4 つは `--self-test` の陽性対照で実際に落ちることを確かめている。

## 使い方

    python3 tools/check_pass_line.py                  在庫の照合(make test から)
    python3 tools/check_pass_line.py --list           在庫を出す(名簿の導出に使う)
    python3 tools/check_pass_line.py --verify <成果物> 合否の一行を判定する(口が呼ぶ)
    python3 tools/check_pass_line.py --self-test      計器自身の陽性/陰性対照(規則 8)
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# --- 在庫。**ここだけを書き換える。** ---------------------------------------
#
# 導出のしかた(規則 13。手で数えた数字は 1 つも無い):
#     python3 tools/check_pass_line.py --list
# が (ファイル, 分類, 件数) をそのまま出す。足りない / 余っているときは
# 検査のメッセージが**次に書くべき数字**を出す。
#
# 「合否の一行を書く箇所」の見つけ方は下の `looks_like_writer` が唯一の真実源で、
# 調査 §A-0 の 3 本の grep(`==== isiki tests` / `" passed, "` / 判定側)を
# 1 つの規則にまとめたものである。
#
#        ファイル                              分類          件数  理由
WRITERS = (
    ("test/lisp/test_framework.lisp", "REAL", 1,
     "枠組みの本物の集計。これが唯一の正規の出どころ"),
    ("src/c/kernel.c", "REAL", 1,
     "9P の無い起動で C 側が 1 件ずつ数えて直列線へ出す要約"),
    ("test/lisp/qemu_boot_no_init_signal.lisp", "SELF-TALLY", 1,
     "条件クラスが 1 つも無い状態を要件とする段。枠組みが使えない"),
    ("test/lisp/qemu_boot_m1_cost.lisp", "SELF-TALLY", 1,
     "同梱の費用を測る段。枠組みを読むと測る対象が変わる"),
    ("tools/mandel_run.lisp", "PROBE", 1,
     "計測プローブ。旧 main と同一のドライバを使う制約で枠組みを読めない"),
    ("tools/gen_jit_nest_probe.py", "PROBE", 1,
     "計測プローブの生成器。生成した Lisp に一行を書かせる"),
    ("tools/run_qemu_milestone.sh", "FIXTURE", 7,
     "判定役の自己試験。7 経路を発火させるための偽の成果物"
     "(#197 で読み込みの打ち切りの配線に 3 本足した)"),
    ("tools/jit_dump_identity.sh", "FIXTURE", 1,
     "比較器の駆動側の自己試験が作る偽のシリアルログ"),
    ("tools/check_jit_dump_identity.py", "FIXTURE", 2,
     "比較器本体の自己試験(雑音耐性・ガード 1)の資料"),
    ("tools/check_test_count_ratchet.py", "FIXTURE", 8,
     "件数照合器(#204)の自己試験が合成するログ。本物のログは CI に無い"),
    ("tools/check_pass_line.py", "FIXTURE", 1,
     "この道具自身の自己試験が使う偽の一行。**組むのは fake_pass_line 1 箇所だけ**"),
)

# 集計を**減らす**代入(= 任意の値を書く代入)の在庫。
# 増やす向き(`(setq *isiki-test-fail* (+ *isiki-test-fail* 1))`)は数えない。
#
#        ファイル                               件数  理由
RESTORES = (
    ("test/lisp/test_framework_test.lisp", 3,
     "陰性対照が出した不合格を退避値へ戻す後始末(tf-capture)"),
    ("test/lisp/bench_interp_test.lisp", 1,
     "陽性対照が意図的に増やした 1 件を戻す後始末"),
    ("test/lisp/bench_jit_guard_test.lisp", 1,
     "同じ形の後始末"),
)
# --------------------------------------------------------------------------

CATEGORIES = ("REAL", "SELF-TALLY", "PROBE", "FIXTURE")

# 分類ごとに許す置き場所(縛り 3)。「印を付ければどこでも良い」にしない
CATEGORY_DIRS = {
    "REAL": ("test/lisp/", "src/c/"),
    "SELF-TALLY": ("test/lisp/",),
    "PROBE": ("tools/",),
    "FIXTURE": ("tools/",),
}

# 走らせる口を持っているか(縛り 2)。FIXTURE はこれを満たすファイルだけ
SELF_TEST_ENTRY = re.compile(r"--self-test|\bself-test\b")

EXTS = (".lisp", ".py", ".sh", ".c", ".h")
SKIP_DIRS = {".git", "build", "tmp", "docs", "documents", "third_party", "__pycache__"}
# トランスパイラ/道具の生成物。再生成で中身が変わるので在庫に入れない
GENERATED = {"lisp_compiled.c", "lisp_compiled_fixture.c", "disasm_symtab.c",
             "init_lisp_src.c"}

# 合否の一行の「形」。数の位置はリテラルでも書式指定でもよい。
# **正規表現のソース(`(\d+) passed`)には当たらない**(文字 `\d+` に数字が無い)ので、
# 読む側(照合器の正規表現)は自然に在庫から外れる
NUM = r"(?:\d+|~D|%d|%s|\{\})"
SHAPE = re.compile(r"%s\s+passed,\s+%s\s+failed" % (NUM, NUM))
# C 側は 1 行に組み立てない(boot_msg_append で継ぎ足す)ので別に見る
C_SHAPE = re.compile(r'"\s+passed,\s+"')
# **「吐く動詞があること」は要求しない。** 最初はそうしていたが、
# `s = "... 0 passed, 0 failed ..."` のように**組むだけの行**(書くのは別の行)が
# 検出から漏れた。漏れる検出器は検出器ではないので、
# **コメントを除いた本文に形があれば印を要求する**に統一した。
# 読む側(照合器の `re.compile(r"(\d+) passed, ...")`)は、文字 `\d+` に数字が無いので
# 形に当たらない。文書の表や散文(`<p> passed`)も当たらない

TAG = re.compile(r"PASS-LINE:\s*([A-Z-]+)[ \t]*(.*)$")
RESTORE_TAG = re.compile(r"TALLY-RESTORE:[ \t]*(.*)$")

TALLY_SETQ = re.compile(r"\(\s*setq\s+(\*isiki-test-(?:pass|fail|attempt)\*)\s+(.*)$")

# 成果物の合否の一行。**行まるごと**で見る(前後に何か付いた行は当たらない)
PASS_LINE = re.compile(r"^==== isiki tests: (\d+) passed, (\d+) failed ====$")
# 旧判定(部分一致)。(1) の陽性対照で「旧判定なら通っていた」ことを示すために残す
LEGACY_NEEDLE = " 0 failed"


# ===========================================================================
# (1) 判定 —— 行まるごとの一致にし、位置も見る
# ===========================================================================

def verify_artifact(path, out=sys.stdout):
    """成果物の合否の一行を判定する。合格なら 0、不合格なら 1 を返す。

    **部分一致をやめた**のがこの関数の要点である。見るのは次の 4 つ:

      1. `==== isiki tests: N passed, M failed ====` に**行まるごと**一致する行
      2. それが**ちょうど 1 行**であること
      3. それが**成果物の最後の非空行**であること(= あとから足されていない)
      4. `M == 0` かつ `N > 0`(**1 件も走っていない成果物を合格にしない**)
    """
    if not os.path.exists(path):
        print("NG: 成果物 %s が無い" % path, file=out)
        return 1
    with open(path, encoding="utf-8", errors="replace") as f:
        lines = f.read().split("\n")

    hits = []          # (行番号, passed, failed)
    decoys = []        # 合否の形をしているが行まるごとでは一致しない行
    for i, raw in enumerate(lines, start=1):
        line = raw.rstrip("\r").rstrip()
        m = PASS_LINE.match(line)
        if m:
            hits.append((i, int(m.group(1)), int(m.group(2))))
        elif SHAPE.search(line) or LEGACY_NEEDLE in line:
            decoys.append((i, line.strip()))

    # 最後の非空行
    last_nonempty = 0
    for i, raw in enumerate(lines, start=1):
        if raw.strip():
            last_nonempty = i

    def ng(msg):
        print("NG: %s" % msg, file=out)
        if decoys:
            print("    注記: 合否の形に似た行が %d 行ある(行 %s)。"
                  "**旧判定はこれで通っていた**"
                  % (len(decoys), ", ".join(str(d[0]) for d in decoys)), file=out)
            for lineno, text in decoys[:3]:
                print("      %d: %s" % (lineno, text[:120]), file=out)
        print("    合否の一行は `==== isiki tests: N passed, M failed ====` に"
              "**行まるごと**一致し、成果物の**最後の非空行**でなければならない。", file=out)
        print("    偽造の 3 形と塞ぎ方: documents/pass-line-forgery.md", file=out)
        return 1

    if not hits:
        return ng("合否の一行が 1 行も無い(%s)" % path)
    if len(hits) > 1:
        return ng("合否の一行が %d 行ある(行 %s)。1 周 1 本の決まりである"
                  % (len(hits), ", ".join(str(h[0]) for h in hits)))

    lineno, passed, failed = hits[0]
    if lineno != last_nonempty:
        return ng("合否の一行(行 %d)のあとに行がある(最後の非空行は %d)。"
                  "報告のあとに何か書いている" % (lineno, last_nonempty))
    if failed != 0:
        return ng("%d 件落ちている(通ったのは %d 件)" % (failed, passed))
    if passed <= 0:
        return ng("1 件も通っていない(0 passed)。検査が走っていない成果物を"
                  "合格にはしない")

    print("OK: 合否の一行は行 %d(成果物の最後の非空行)。通ったのは %d 件、"
          "落ちたのは 0 件" % (lineno, passed), file=out)
    if decoys:
        print("    注記: 合否の形に似た行が %d 行あるが、"
              "**行まるごとの一致ではないので判定に使っていない**" % len(decoys), file=out)
    return 0


def legacy_accepts(path):
    """旧判定(`grep -q " 0 failed"`)が通すかどうか。

    (1) の陽性対照で「**旧判定なら通っていた**」ことを示すためだけに残してある。
    判定には使わない。
    """
    with open(path, encoding="utf-8", errors="replace") as f:
        return LEGACY_NEEDLE in f.read()


# ===========================================================================
# 在庫の走査
# ===========================================================================

def list_source_files(root):
    """見るファイルの一覧。

    git の中なら**追跡されているファイルだけ**を見る。追跡していない一時プローブは
    在庫に入らない(入れると、各自の作業木の落書きで `make test` が落ちる)。
    **コミットされた時点で在庫に入る**ので、偽造が木に残れば必ず見える。
    自己試験の一時ディレクトリは git の外なので、そこでは単純に走査する。
    """
    try:
        out = subprocess.run(["git", "-C", root, "ls-files", "-z"],
                             capture_output=True, check=True).stdout
        names = [n.decode("utf-8") for n in out.split(b"\0") if n]
        if names:
            return sorted(n for n in names
                          if n.endswith(EXTS)
                          and os.path.basename(n) not in GENERATED
                          and n.split("/")[0] not in SKIP_DIRS)
    except (OSError, subprocess.CalledProcessError):
        pass
    found = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
        for name in filenames:
            if name.endswith(EXTS) and name not in GENERATED:
                found.append(os.path.relpath(os.path.join(dirpath, name), root))
    return sorted(found)


def lang_of(path):
    if path.endswith(".lisp"):
        return "lisp"
    if path.endswith(".py"):
        return "py"
    if path.endswith(".sh"):
        return "sh"
    return "c"


def strip_comments(text, lang):
    """コメントを空白へ潰す(行数と桁数は保つ)。文字列リテラルの中は触らない。

    **コメントアウトされた偽造を在庫に数えないため**に要る。
    印はコメントに書くので、印を読むのは生の行から行う(ここでは潰した行で
    「出力している行かどうか」だけを見る)。
    """
    out = []
    i = 0
    n = len(text)
    quote = None
    while i < n:
        c = text[i]
        if quote:
            out.append(c)
            if c == "\\" and i + 1 < n and quote != "'":
                out.append(text[i + 1])
                i += 2
                continue
            if c == quote:
                quote = None
            elif c == "\n":
                # 閉じないまま行が終わるのは壊れた入力。文字列を抜ける
                quote = None
            i += 1
            continue
        if c in ('"', "'"):
            # Lisp の `'` は引用符ではなく quote なので文字列開始と誤らない
            if lang == "lisp" and c == "'":
                out.append(c)
                i += 1
                continue
            quote = c
            out.append(c)
            i += 1
            continue
        if lang == "lisp" and text[i:i + 2] == "#\\":
            out.append(text[i:i + 3])
            i += 3
            continue
        if (lang == "lisp" and c == ";") or (lang in ("py", "sh") and c == "#"):
            while i < n and text[i] != "\n":
                out.append(" ")
                i += 1
            continue
        if lang == "c" and text[i:i + 2] == "//":
            while i < n and text[i] != "\n":
                out.append(" ")
                i += 1
            continue
        if lang == "c" and text[i:i + 2] == "/*":
            out.append("  ")
            i += 2
            while i < n and text[i:i + 2] != "*/":
                out.append("\n" if text[i] == "\n" else " ")
                i += 1
            out.append("  ")
            i += 2
            continue
        out.append(c)
        i += 1
    return "".join(out)


def looks_like_writer(code_line, path):
    """この行は合否の一行を**作っている**か(= 印が要るか)。

    見るのは形だけである。**吐く動詞は要求しない**(上の注記のとおり、
    組むだけの行が漏れたため)。C 側は 1 行に組み立てない(`boot_msg_append` で
    継ぎ足す)ので、`" passed, "` の断片を別に見る。
    """
    if SHAPE.search(code_line):
        return True
    if path.endswith((".c", ".h")) and C_SHAPE.search(code_line):
        return True
    return False


def scan(root):
    """在庫を走査する。返り値 (writers, restores)。

    writers : [(ファイル, 行番号, 分類 or None, 理由, 生の行)]
    restores: [(ファイル, 行番号, 変数, 理由 or None, 生の行)]
    """
    writers = []
    restores = []
    for rel in list_source_files(root):
        full = os.path.join(root, rel)
        try:
            with open(full, encoding="utf-8", errors="replace") as f:
                text = f.read()
        except OSError:
            continue
        lang = lang_of(rel)
        raw_lines = text.split("\n")
        code_lines = strip_comments(text, lang).split("\n")
        for i, code in enumerate(code_lines):
            raw = raw_lines[i] if i < len(raw_lines) else ""
            if looks_like_writer(code, rel):
                m = TAG.search(raw)
                if not m and i > 0 and not code_lines[i - 1].strip():
                    # **複数行にわたる式**(枠組みの format は 2 行ある)では、
                    # 式の途中へコメントを割り込ませるより**直前のコメント行**へ
                    # 置くほうが安全である。印はどちらにあってもよい
                    m = TAG.search(raw_lines[i - 1])
                if m:
                    why = m.group(2).strip().rstrip("*/").strip()
                    writers.append((rel, i + 1, m.group(1), why, raw))
                else:
                    writers.append((rel, i + 1, None, "", raw))
            if lang == "lisp":
                m = TALLY_SETQ.search(code)
                if m:
                    var, rhs = m.group(1), m.group(2)
                    # 増やす向きは数えない(安全側。調査 §A-2 の非対称性)
                    inc = re.match(r"\(\s*(?:\+|1\+)\s*" + re.escape(var), rhs.strip())
                    if inc:
                        continue
                    t = RESTORE_TAG.search(raw)
                    restores.append((rel, i + 1, var,
                                     t.group(1).strip() if t else None, raw))
    return writers, restores


# ===========================================================================
# (2)(3) 在庫の照合
# ===========================================================================

def check_inventory(root, writers_roster=WRITERS, restores_roster=RESTORES,
                    out=sys.stdout):
    """在庫を名簿と突き合わせる。合っていれば 0、違えば 1。

    **両方向で落ちる**(規則 16)。増えた = 偽造が増えた。減った = 名簿も下げろ。
    どちらでも**次に書くべき数字**を出す。
    """
    writers, restores = scan(root)
    problems = []

    # --- (2) 合否の一行を書く箇所 ---------------------------------------
    roster = {(p, c): (n, why) for (p, c, n, why) in writers_roster}
    roster_files = {p for (p, _c, _n, _w) in writers_roster}
    actual = {}
    for rel, lineno, cat, why, raw in writers:
        if cat is None:
            problems.append(
                "印の無い合否の一行がある: %s:%d\n"
                "      %s\n"
                "    合否の一行を書く箇所には**その行に**分類の印を付けること:\n"
                "      PASS-LINE: <%s> <理由>\n"
                "    印を付けても、名簿(WRITERS)に無いファイル・名簿より多い件数は"
                "通らない" % (rel, lineno, raw.strip()[:140], "|".join(CATEGORIES)))
            continue
        if cat not in CATEGORIES:
            problems.append("知らない分類 %r がある: %s:%d。使えるのは %s"
                            % (cat, rel, lineno, ", ".join(CATEGORIES)))
            continue
        if not why:
            problems.append("印に理由が無い: %s:%d。**なぜその分類なのか**を"
                            "同じ行に書くこと" % (rel, lineno))
            continue
        # 縛り 3: 置き場所で役を縛る
        dirs = CATEGORY_DIRS[cat]
        if not any(rel.startswith(d) for d in dirs):
            problems.append("分類 %s は %s の下にしか置けない: %s:%d"
                            % (cat, " / ".join(dirs), rel, lineno))
            continue
        # 縛り 2: FIXTURE は自己試験の口を持つファイルだけ
        if cat == "FIXTURE":
            with open(os.path.join(root, rel), encoding="utf-8",
                      errors="replace") as f:
                body = f.read()
            if not SELF_TEST_ENTRY.search(body):
                problems.append(
                    "FIXTURE の印は**自己試験の口を持つファイル**だけに許す: %s:%d。"
                    "試験ファイルや台本が自分を『判定役の資料』と名乗れないようにして"
                    "ある" % (rel, lineno))
                continue
        actual[(rel, cat)] = actual.get((rel, cat), 0) + 1

    for key in sorted(set(actual) | set(roster)):
        rel, cat = key
        got = actual.get(key, 0)
        want = roster.get(key, (0, ""))[0]
        if got == want:
            continue
        if want == 0:
            problems.append(
                "名簿に無い %s の箇所がある: %s(%d 件)。\n"
                "    **印を付けただけでは通らない。** 足すなら WRITERS に\n"
                '      ("%s", "%s", %d, "<理由>"),\n'
                "    を足し、**なぜ増やすのかを同じ場所に書くこと**(規則 16)"
                % (cat, rel, got, rel, cat, got))
        elif got == 0:
            problems.append(
                "名簿にある %s の箇所が消えた: %s(名簿 %d 件 → 0 件)。\n"
                "    消したのなら WRITERS の行を削ること(減ったのを黙って通すと"
                "計器が形骸化する)" % (cat, rel, want))
        else:
            problems.append(
                "%s の件数が合わない: %s(名簿 %d 件 → 実際 %d 件)。\n"
                "    **次に書く数字は %d**。増やしたのなら理由も同じ場所に書くこと"
                % (cat, rel, want, got, got))

    # --- (3) 集計を減らす代入 -------------------------------------------
    r_roster = {p: (n, why) for (p, n, why) in restores_roster}
    r_actual = {}
    for rel, lineno, var, why, raw in restores:
        if why is None:
            problems.append(
                "印の無い『集計を減らす代入』がある: %s:%d\n"
                "      %s\n"
                "    集計変数へ任意の値を書く行には**その行に**印を付けること:\n"
                "      TALLY-RESTORE: <理由>\n"
                "    増やす向き((+ %s 1))は安全側なので印は要らない"
                % (rel, lineno, raw.strip()[:140], var))
            continue
        if not why:
            problems.append("TALLY-RESTORE の印に理由が無い: %s:%d" % (rel, lineno))
            continue
        r_actual[rel] = r_actual.get(rel, 0) + 1

    for rel in sorted(set(r_actual) | set(r_roster)):
        got = r_actual.get(rel, 0)
        want = r_roster.get(rel, (0, ""))[0]
        if got == want:
            continue
        if want == 0:
            problems.append(
                "名簿に無いファイルが集計を減らしている: %s(%d 件)。\n"
                "    **印を付けただけでは通らない。** 足すなら RESTORES に\n"
                '      ("%s", %d, "<理由>"),\n'
                "    を足すこと。**減らす代入は偽造と構造的に区別できない**ので、"
                "増やすときは理由を書く" % (rel, got, rel, got))
        elif got == 0:
            problems.append(
                "名簿にある減らす代入が消えた: %s(名簿 %d 件 → 0 件)。"
                "RESTORES の行を削ること" % (rel, want))
        else:
            problems.append(
                "減らす代入の件数が合わない: %s(名簿 %d 件 → 実際 %d 件)。\n"
                "    **次に書く数字は %d**" % (rel, want, got, got))

    if problems:
        print("NG: 合否の一行の在庫が名簿と合っていない(%d 件)" % len(problems),
              file=out)
        for p in problems:
            print("  - %s" % p, file=out)
        print("  在庫の出し方: python3 tools/check_pass_line.py --list", file=out)
        print("  偽造の 3 形と塞ぎ方: documents/pass-line-forgery.md", file=out)
        return 1

    n_w = sum(n for (_p, _c, n, _w) in writers_roster)
    n_r = sum(n for (_p, n, _w) in restores_roster)
    by_cat = {}
    for (_p, c, n, _w) in writers_roster:
        by_cat[c] = by_cat.get(c, 0) + n
    print("OK: 合否の一行を書く箇所 %d(%s)/ 集計を減らす代入 %d。"
          "**どれも名簿どおり**"
          % (n_w, " ".join("%s=%d" % (c, by_cat[c]) for c in CATEGORIES
                           if c in by_cat), n_r), file=out)
    return 0


def cmd_list(root):
    writers, restores = scan(root)
    print("# 合否の一行を書く箇所(%d 件)" % len(writers))
    for rel, lineno, cat, why, raw in writers:
        print("  %-44s :%-5d %-10s %s" % (rel, lineno, cat or "(印なし)", why))
    print("# 集計を減らす代入(%d 件)" % len(restores))
    for rel, lineno, var, why, raw in restores:
        print("  %-44s :%-5d %-22s %s"
              % (rel, lineno, var, "(印なし)" if why is None else why))
    print("# 名簿に書く形(WRITERS)")
    counts = {}
    for rel, _l, cat, why, _r in writers:
        counts.setdefault((rel, cat), [0, why])
        counts[(rel, cat)][0] += 1
    for (rel, cat), (n, why) in sorted(counts.items(),
                                   key=lambda kv: (kv[0][0], kv[0][1] or "")):
        print('    ("%s", "%s", %d, "%s"),' % (rel, cat, n, why))
    print("# 名簿に書く形(RESTORES)")
    rc = {}
    for rel, _l, _v, why, _r in restores:
        rc.setdefault(rel, [0, why])
        rc[rel][0] += 1
    for rel, (n, why) in sorted(rc.items()):
        print('    ("%s", %d, "%s"),' % (rel, n, why))
    return 0


# ===========================================================================
# 自己試験(規則 8)—— 3 つの偽造それぞれに陽性対照を置く
# ===========================================================================

def _write(path, lines):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write("".join(lines))


def fake_pass_line(passed="~D", failed="~D"):
    """**偽の合否の一行をここ 1 箇所だけで組む。**

    自己試験は偽の一行を何通りも作る必要がある(それが陽性対照だから)。
    しかし作る箇所を散らすと、**この道具自身が「印の無い偽造」の巣になる。**
    だから出どころを 1 つに寄せ、印もここだけに付ける。
    在庫が `tools/check_pass_line.py` について 1 件なのはこのためである。
    """
    # PASS-LINE: FIXTURE 自己試験が使う偽の一行。**組むのはこの 1 箇所だけ**
    return "==== isiki tests: %s passed, %s failed ====" % (passed, failed)


def _genuine_artifact(passed=42, failed=0):
    """本物の成果物の形。実機 1 周の記録(T1)の並びをそのまま写した。

    `#at` / `#count` が並び、`#elapsed` が出て、空行のあとに
    合否の一行が**最後の非空行として**来る。
    """
    return [
        "#at test/lisp/isiki_test.lisp 3 ticks\n",
        "#count test/lisp/isiki_test.lisp %d\n" % passed,
        "#elapsed 20 ticks = 0 sec\n",
        "\n",
        fake_pass_line(passed, failed) + "\n",
    ]


def _lisp_writer(passed="~D", failed="~D", tag=""):
    """合成の木に置く「合否の一行を書く Lisp の行」。印は呼び元が決める。"""
    return '(format *s* "~%%%s~%%")%s\n' % (fake_pass_line(passed, failed), tag)


def self_test():
    import io

    fails = []

    def check(name, got, want):
        if got == want:
            print("OK(%s)" % name)
        else:
            print("FAIL(%s): 結果 %r(期待 %r)" % (name, got, want), file=sys.stderr)
            fails.append(name)

    box = tempfile.mkdtemp()
    try:
        # ---------------------------------------------------------------
        # (1) 判定の部分一致 —— **行まるごとの一致にした**ことの対照
        # ---------------------------------------------------------------
        print("--- (1) 判定: 行まるごとの一致と位置を見る ---")

        def verdict(lines):
            path = os.path.join(box, "art.txt")
            _write(path, lines)
            sink = io.StringIO()
            return verify_artifact(path, out=sink), sink.getvalue(), path

        rc, _txt, _p = verdict(_genuine_artifact())
        check("陰性対照・本物の形の成果物は通る", rc, 0)

        # **偽造 (1) そのもの**: 別の行に `" 0 failed"` を出し、本物の一行は不合格。
        # 調査 §A-5 の突き方([NG] 行は ~S で式をそのまま印字する)を写してある
        decoy = [
            "#at test/lisp/forge_test.lisp 3 ticks\n",
            '[NG] (f) => expected " 0 failed" but got "x"\n',
            "#count test/lisp/forge_test.lisp 6\n",
            "#elapsed 20 ticks = 0 sec\n",
            "\n",
            fake_pass_line(1, 5) + "\n",
        ]
        rc, txt, path = verdict(decoy)
        check("陽性対照(1)・別の行の部分一致では通らない", rc, 1)
        check("陽性対照(1)・5 件落ちていると言う", "5 件落ちている" in txt, True)
        # **★ だった前提をここで実測する。**
        # 調査 §9-1 / §A-5 は「部分一致の穴は通ると読めるが未検証」と書いていた。
        # 旧判定そのもの(`grep -q " 0 failed"`)にこの資料をかけると**通る**
        check("陽性対照(1)・**旧判定はこれを通していた**(部分一致の穴の実測)",
              legacy_accepts(path), True)

        # 報告のあとに書き足す形(位置を見ていなければ通ってしまう)
        rc, txt, _p = verdict(_genuine_artifact() + ["#after something\n"])
        check("陽性対照(1)・合否の一行が最後でないと通らない", rc, 1)
        check("陽性対照(1)・位置を理由に挙げる", "あとに行がある" in txt, True)

        # 行の一部として出した形(前後に何か付いている)
        rc, _txt, _p = verdict(['[NG] (f) => "%s"\n' % fake_pass_line(1, 0)])
        check("陽性対照(1)・行の一部に出しても合否の一行とは認めない", rc, 1)

        # 検査 0 件の成果物(= 偽造 (2) の最小形。**一行だけで合格を名乗る**)
        rc, txt, _p = verdict([fake_pass_line(0, 0) + "\n"])
        check("陽性対照(1)・0 passed は合格にしない", rc, 1)
        check("陽性対照(1)・1 件も走っていないと言う", "1 件も通っていない" in txt, True)

        # 合否の一行が 2 本ある(成果物を開き直した / 2 回報告した形)
        rc, _txt, _p = verdict(_genuine_artifact() + ["\n",
                                                      fake_pass_line(1, 0) + "\n"])
        check("陽性対照(1)・合否の一行が 2 本あると通らない", rc, 1)

        # 合否の一行が無い(ゲストが結果を出さずに終わった形)
        rc, txt, _p = verdict(["#at test/lisp/isiki_test.lisp 3 ticks\n"])
        check("陽性対照(1)・合否の一行が無ければ通らない", rc, 1)

        # ---------------------------------------------------------------
        # (2) 書く箇所の在庫 —— 合成の木で照合する
        # ---------------------------------------------------------------
        print("--- (2) 合否の一行を書く箇所の在庫 ---")
        tree = os.path.join(box, "tree2")
        fw = os.path.join(tree, "test/lisp/test_framework.lisp")
        _write(fw, [
            ";; 合成の枠組み\n",
            _lisp_writer(tag="  ; PASS-LINE: REAL 本物の集計"),
            ";; **コメントアウトされた偽造は数えない**(それも確かめる):\n",
            ";; " + _lisp_writer(1, 0),
        ])
        roster2 = (("test/lisp/test_framework.lisp", "REAL", 1, "合成の本物"),)
        sink = io.StringIO()
        rc = check_inventory(tree, roster2, (), out=sink)
        check("陰性対照・名簿どおりなら通る(コメントは数えない)", rc, 0)

        # 陽性対照: 印の無い偽造を 1 本足す
        forge = os.path.join(tree, "tools/forge_probe.lisp")
        _write(forge, [_lisp_writer(1, 0)])
        sink = io.StringIO()
        rc = check_inventory(tree, roster2, (), out=sink)
        check("陽性対照(2)・印の無い偽造を足すと落ちる", rc, 1)
        check("陽性対照(2)・印が無いと言う",
              "印の無い合否の一行" in sink.getvalue(), True)

        # **印の乱用を止める対照 1**: 印を付けても名簿に無いので落ちる
        _write(forge, [_lisp_writer(1, 0, "  ; PASS-LINE: PROBE 計測プローブ")])
        sink = io.StringIO()
        rc = check_inventory(tree, roster2, (), out=sink)
        check("陽性対照(2)・**印を付けても名簿に無ければ落ちる**", rc, 1)
        check("陽性対照(2)・名簿に無いと言う",
              "名簿に無い PROBE" in sink.getvalue(), True)

        # **印の乱用を止める対照 2**: 名簿にあるファイルでも件数が増えれば落ちる
        os.remove(forge)
        _write(fw, [
            _lisp_writer(tag="  ; PASS-LINE: REAL 本物の集計"),
            _lisp_writer(1, 0, "  ; PASS-LINE: REAL 2 本目"),
        ])
        sink = io.StringIO()
        rc = check_inventory(tree, roster2, (), out=sink)
        check("陽性対照(2)・**名簿にあるファイルでも件数が増えれば落ちる**", rc, 1)
        check("陽性対照(2)・次に書く数字を出す",
              "次に書く数字は 2" in sink.getvalue(), True)

        # 陽性対照: 減っても落ちる(規則 16)
        _write(fw, [";; 枠組みから合否の一行を消した\n"])
        sink = io.StringIO()
        rc = check_inventory(tree, roster2, (), out=sink)
        check("陽性対照(2)・減っても落ちる", rc, 1)
        check("陽性対照(2)・消えたと言う", "消えた" in sink.getvalue(), True)

        # **印の乱用を止める対照 3**: FIXTURE は自己試験の口を持つファイルだけ
        tree3 = os.path.join(box, "tree3")
        _write(os.path.join(tree3, "tools/not_a_detector.py"), [
            'f.write("%s\\n")  # PASS-LINE: FIXTURE 資料だと名乗ってみる\n'
            % fake_pass_line(1, 0),
        ])
        sink = io.StringIO()
        rc = check_inventory(tree3,
                             (("tools/not_a_detector.py", "FIXTURE", 1, "名乗り"),),
                             (), out=sink)
        check("陽性対照(2)・**自己試験の口が無いファイルは FIXTURE を名乗れない**",
              rc, 1)
        check("陽性対照(2)・その理由を言う",
              "自己試験の口を持つファイル" in sink.getvalue(), True)

        # **印の乱用を止める対照 4**: 置き場所の縛り(試験ファイルがプローブを名乗る)
        tree4 = os.path.join(box, "tree4")
        _write(os.path.join(tree4, "test/lisp/sneak_test.lisp"),
               [_lisp_writer(1, 0, "  ; PASS-LINE: PROBE プローブを名乗ってみる")])
        sink = io.StringIO()
        rc = check_inventory(tree4,
                             (("test/lisp/sneak_test.lisp", "PROBE", 1, "名乗り"),),
                             (), out=sink)
        check("陽性対照(2)・**置き場所の縛りで落ちる**(PROBE は tools/ だけ)", rc, 1)

        # 印に理由が無い
        tree4b = os.path.join(box, "tree4b")
        _write(os.path.join(tree4b, "test/lisp/t.lisp"),
               [_lisp_writer(1, 0, "  ; PASS-LINE: REAL")])
        sink = io.StringIO()
        rc = check_inventory(tree4b, (("test/lisp/t.lisp", "REAL", 1, "x"),),
                             (), out=sink)
        check("陽性対照(2)・**理由の無い印は落ちる**", rc, 1)

        # ---------------------------------------------------------------
        # **印を外すと落ちる** —— 現物の資料 3 本で確かめる(#205 の完了条件)
        #
        # 印を付けただけでは「効いている」と言えない。**印を外した木**を作って
        # 落ちることを見るのが、印が効いていることの対照である
        # ---------------------------------------------------------------
        print("--- 印が効いていることの対照(現物の資料 3 本) ---")
        # **件数は WRITERS から引く。** ここに数字を写すと、現物が増えたときに
        # この自己試験だけが落ちる(#197 で run_qemu_milestone.sh が 4 -> 7 に
        # なったときに実際に落ちた)。規則 13 のとおり、数字は導出する
        def _roster_count(rel):
            return [n for f, c, n, _w in WRITERS if f == rel and c == "FIXTURE"][0]

        marked = tuple(
            (rel, "FIXTURE", _roster_count(rel), why)
            for rel, why in (
                ("tools/run_qemu_milestone.sh", "判定役の自己試験"),
                ("tools/jit_dump_identity.sh", "比較器の駆動側"),
                ("tools/check_jit_dump_identity.py", "比較器本体"),
            ))
        tree5 = os.path.join(box, "tree5")
        for rel, _c, _n, _w in marked:
            dst = os.path.join(tree5, rel)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copyfile(os.path.join(REPO_ROOT, rel), dst)
        sink = io.StringIO()
        rc = check_inventory(tree5, marked, (), out=sink)
        check("陰性対照・印の付いた現物 3 本は通る", rc, 0)

        # **印だけを消す**(中身は 1 文字も変えない)
        for rel, _c, _n, _w in marked:
            dst = os.path.join(tree5, rel)
            with open(dst, encoding="utf-8") as f:
                body = f.read()
            with open(dst, "w", encoding="utf-8") as f:
                f.write(re.sub(r"PASS-LINE:\s*FIXTURE[^\n]*", "", body))
        sink = io.StringIO()
        rc = check_inventory(tree5, marked, (), out=sink)
        check("陽性対照・**印を外すと落ちる**", rc, 1)
        txt = sink.getvalue()
        check("陽性対照・3 本すべてが挙がる",
              all(rel in txt for rel, _c, _n, _w in marked), True)

        # ---------------------------------------------------------------
        # (3) 集計を減らす代入の在庫
        # ---------------------------------------------------------------
        print("--- (3) 集計を減らす代入の在庫 ---")
        tree6 = os.path.join(box, "tree6")
        tf = os.path.join(tree6, "test/lisp/guard_test.lisp")
        _write(tf, [
            ";; **増やす向きは安全側なので数えない**(調査 §A-2 の非対称性)\n",
            "(setq *isiki-test-fail* (+ *isiki-test-fail* 1))\n",
            ";; 戻す向きは印が要る\n",
            "(setq *isiki-test-fail* saved)  ; TALLY-RESTORE: 後始末\n",
        ])
        roster6 = (("test/lisp/guard_test.lisp", 1, "後始末"),)
        sink = io.StringIO()
        rc = check_inventory(tree6, (), roster6, out=sink)
        check("陰性対照・増やす向きは数えず、印の付いた戻しは通る", rc, 0)

        with open(tf, "a", encoding="utf-8") as f:
            f.write("(setq *isiki-test-fail* 0)\n")
        sink = io.StringIO()
        rc = check_inventory(tree6, (), roster6, out=sink)
        check("陽性対照(3)・印の無い『減らす代入』で落ちる", rc, 1)
        check("陽性対照(3)・印が無いと言う",
              "印の無い『集計を減らす代入』" in sink.getvalue(), True)

        # **印の乱用を止める対照 5**: 印を付けても件数が増えれば落ちる
        with open(tf, encoding="utf-8") as f:
            body = f.read()
        with open(tf, "w", encoding="utf-8") as f:
            f.write(body.replace(
                "(setq *isiki-test-fail* 0)\n",
                "(setq *isiki-test-fail* 0)  ; TALLY-RESTORE: 乱用してみる\n"))
        sink = io.StringIO()
        rc = check_inventory(tree6, (), roster6, out=sink)
        check("陽性対照(3)・**印を付けても件数が増えれば落ちる**", rc, 1)
        check("陽性対照(3)・次に書く数字を出す",
              "次に書く数字は 2" in sink.getvalue(), True)

        # 減っても落ちる
        _write(tf, ["(setq *isiki-test-fail* (+ *isiki-test-fail* 1))\n"])
        sink = io.StringIO()
        rc = check_inventory(tree6, (), roster6, out=sink)
        check("陽性対照(3)・減っても落ちる", rc, 1)

        # ---------------------------------------------------------------
        # 現物の在庫(= make test から走るもの)が名簿どおりであること
        # ---------------------------------------------------------------
        print("--- 現物の在庫 ---")
        sink = io.StringIO()
        rc = check_inventory(REPO_ROOT, out=sink)
        check("陰性対照・現物の在庫が名簿どおり", rc, 0)
        print("  " + sink.getvalue().strip())
    finally:
        shutil.rmtree(box, ignore_errors=True)

    if fails:
        print("self-test 失敗: %s" % ", ".join(fails), file=sys.stderr)
        return 1
    print("self-test 成功: **偽造の 3 形それぞれに陽性対照があり、"
          "印を外すと落ちることも確かめた**")
    return 0


def main():
    ap = argparse.ArgumentParser(description="合否の一行が信用できるかを見る")
    ap.add_argument("--verify", metavar="成果物",
                    help="合否の一行を判定する(行まるごと一致・位置・件数)")
    ap.add_argument("--list", action="store_true", help="在庫を出す")
    ap.add_argument("--self-test", action="store_true", help="計器自身の対照")
    args = ap.parse_args()

    if args.verify:
        return verify_artifact(args.verify)
    if args.list:
        return cmd_list(REPO_ROOT)
    if args.self_test:
        return self_test()
    return check_inventory(REPO_ROOT)


if __name__ == "__main__":
    sys.exit(main())
