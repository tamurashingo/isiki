#!/bin/bash
# [二重定義マクロの照合] src/lisp/init.lisp の defmacro と
# src/lisp/transpile.lisp の *macro-expanders* が、同じフォームを同じ形に
# 展開するかを突き合わせる(documents/for-expansion.md §3)。
#
# **なぜ要るか。**
# transpile.lisp:721 のコメントは for/while について「init.lisp の defmacro と
# **同じ展開規則**」と書いていた。**それが嘘になっていた。** for だけ片方
# (AOT 側)が「ループ本体の let 廃止」で作り直され、init.lisp は旧展開形のまま
# だった。**2 つの実装が同じであると主張して、照合するものが何も無かった。**
#
# 判定は tools/macro_parity_expected.tsv に書いてある:
#   same      gensym 名を除いて一致すること。**違ったら落ちる**
#   cosmetic  違うが意味は同じ。**同じになったら落ちる**
#   DIVERGENT 意味が違う既知の不一致。**同じになったら落ちる**
#             (直したのなら same へ変え、記録と文書を更新する)
#
# QEMU を 1 回起動するので make test には入れない(make test-qemu-macro-parity)。
set -eu

FORMS=tools/macro_parity_forms.sexp
EXPECTED=tools/macro_parity_expected.tsv
HOST_OUT=tmp/macro_parity_host.txt
GUEST_OUT=tmp/macro_parity_guest.txt

mkdir -p tmp
python3 tools/gen_macro_parity_drivers.py

echo "--- host 側(transpile.lisp の展開関数)---"
docker run --rm --user "$(id -u):$(id -g)" --entrypoint bash -v "$PWD":/workspace isiki-builder \
    -c 'ros run --load tmp/macro_parity_host.lisp --quit' 2>&1 | grep '^#HOST ' > "$HOST_OUT"

echo "--- guest 側(init.lisp の macroexpand-1)---"
make test-qemu-milestone MILESTONE=tmp/macro_parity_guest.lisp > /dev/null
grep '^#GUEST ' test-results.txt > "$GUEST_OUT"

python3 - "$HOST_OUT" "$GUEST_OUT" "$EXPECTED" "$FORMS" << 'PYEOF'
import re
import sys

host_path, guest_path, expected_path, forms_path = sys.argv[1:5]


def load(path, tag):
    out = {}
    for ln in open(path, encoding="utf-8"):
        m = re.match(r"^#" + tag + r" (\S+) :: (.*)$", ln.rstrip("\n"))
        if m:
            out[m.group(1)] = m.group(2)
    return out


def normalise(s):
    """gensym 名を均す。host は #:G498、guest は G7 のような名前になる。
    **名前の違いは意味の違いではない**ので、両方 GENSYM へ畳んでから比べる。"""
    s = re.sub(r"#:G\d+", "GENSYM", s)
    s = re.sub(r"\bG\d+\b", "GENSYM", s)
    return " ".join(s.split())


expected = {}
for ln in open(expected_path, encoding="utf-8"):
    ln = ln.rstrip("\n")
    if not ln.strip() or ln.lstrip().startswith("#"):
        continue
    cols = ln.split("\t")
    expected[cols[0]] = (cols[1], cols[2] if len(cols) > 2 else "")

host = load(host_path, "HOST")
guest = load(guest_path, "GUEST")

names = [ln.split("\t")[0].strip() for ln in open(forms_path, encoding="utf-8")
         if ln.strip() and not ln.lstrip().startswith(";")]

ng = []
print()
print("| マクロ | 判定 | host と guest | 説明 |")
print("|---|---|---|---|")
for name in names:
    h, g = host.get(name), guest.get(name)
    if h is None or g is None:
        ng.append((name, f"展開を取得できなかった(host={h is not None} guest={g is not None})"))
        continue
    same = normalise(h) == normalise(g)
    verdict, note = expected.get(name, (None, ""))
    if verdict is None:
        ng.append((name, "tools/macro_parity_expected.tsv に行が無い"))
        continue
    if verdict == "same" and not same:
        ng.append((name, "**一致するはずが違う。** 片方だけ直された疑い\n"
                         f"        host : {h}\n        guest: {g}"))
    elif verdict in ("cosmetic", "DIVERGENT") and same:
        ng.append((name, f"**一致した。** 判定 {verdict} の理由が消えている。"
                         f"tools/macro_parity_expected.tsv を same へ直し、"
                         f"記録と文書を更新すること"))
    print(f"| {name} | {verdict} | {'一致' if same else '**違う**'} | {note} |")

if ng:
    print()
    print("=== 照合が落ちました ===")
    for name, msg in ng:
        print(f"[NG] {name}: {msg}")
    print("""
**これは「2 つの実装が同じである」という主張が崩れたという報告である。**
どちらが正しいかはこの検査では決まらない。
  - 片方を意図して直したのなら、tools/macro_parity_expected.tsv の判定を
    更新し、**両方の経路で意味論を実測してから**記録を直すこと
    (documents/for-expansion.md の並列代入の確認が手本)
  - 意図していないのなら、それが見つけたかった不一致である
""")
    sys.exit(1)
print()
print("二重定義マクロの照合: すべて期待どおりです。")
PYEOF
