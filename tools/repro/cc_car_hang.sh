#!/bin/bash
# cc_car/cc_cdrインライン化ハングの再現(tools/repro/README.md 参照)
set -eu
cd "$(dirname "$0")/../.."
MEM="${QEMU_MEM:-256M}"

if ! git diff --quiet src/c/lisp.h src/c/lisp.c src/c/za.c; then
    echo "ERROR: src/c/lisp.h / lisp.c / za.c に未コミットの変更がある。先に退避すること" >&2
    exit 1
fi
trap 'git checkout -- src/c/lisp.h src/c/lisp.c src/c/za.c; echo "(復元した)"' EXIT

echo "--- 1. cc_car/cc_cdr を static inline 化 ---"
git apply tools/repro/cc_car_inline.patch
python3 - <<'PY'
import re
p='src/c/lisp.c'; s=open(p).read()
s=re.sub(r'lisp_val_t cc_car\(lisp_val_t obj\) \{.*?\n\}\n\nlisp_val_t cc_cdr\(lisp_val_t obj\) \{.*?\n\}\n', '', s, flags=re.S)
open(p,'w').write(s)
PY

echo "--- 2. za.c の上限コードを無効化(これをしないと再現しない) ---"
python3 - <<'PY'
import re
p='src/c/za.c'; s=open(p).read()
# 上限の3定数を実質無限にし、カウンタのインクリメントを消す
s=s.replace('#define ZA_MAX_MACROEXPAND_ITER 1000','#define ZA_MAX_MACROEXPAND_ITER 0xFFFFFFFFFFFFFFFFULL')
s=s.replace('#define ZA_MAX_ANALYZE_STEPS 200000','#define ZA_MAX_ANALYZE_STEPS 0xFFFFFFFFFFFFFFFFULL')
s=s.replace('#define ZA_MAX_BODY_FORMS_WALK 10000','#define ZA_MAX_BODY_FORMS_WALK 0xFFFFFFFFFFFFFFFFULL')
s=s.replace('#define ZA_MX(site, form, env) (g_za_mx_calls[site]++, za_macroexpand((form), (env)))',
            '#define ZA_MX(site, form, env) (za_macroexpand((form), (env)))')
s=s.replace('if (g_za_analyze_steps++ >= ZA_MAX_ANALYZE_STEPS) {','if (0) {')
# Floyd検出器も外す。**これを残すと再現しない**(検出器のコードが実行されるだけで
# 症状が消える。摂動に敏感という性質そのもの)
s=s.replace('if (body_walk > 0 && (body_walk & 1) == 0 && (tortoise & TAG_MASK) == TAG_CONS) {','if (0) {')
s=s.replace('if (body_walk > 0 && rest == tortoise) {','if (0) {')
s=s.replace('if (++body_walk > ZA_MAX_BODY_FORMS_WALK) {','if (0) {')
open(p,'w').write(s)
PY

echo "--- 3. QEMU_MEM=$MEM で全件を流す(ハングすれば再現) ---"
rm -f test-results.txt
if timeout 600 make test-qemu QEMU_MEM="$MEM" >/dev/null 2>&1; then
    echo "結果: 完走した(再現せず)。$(tail -1 test-results.txt)"
else
    echo "結果: **ハングした(再現)**"
fi
