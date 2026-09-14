#!/bin/bash
# [性能測定] AOTのletインライナが実際に発火していることを、生成コードを見て確認する。
#
# インライナ(transpile.lispのtranspile-inline-immediate-lambda)は、expand-letが
# 出力する「即時lambda呼び出し」の形にパターンマッチしている。マクロ側の出力形が
# 変わるとマッチしなくなり、**黙って発火しなくなる**。エラーにもならずテストも通り、
# letの命令数だけが355から2,945へ戻る(documents/pitfalls.md 原則6の形)。
#
# 発火していれば生成コードに __inl_ が現れ、フォールバック経路の
# os_make_lifted_closure / primitive_funcall は現れない。
set -eu
F=src/c/lisp_compiled.c
python3 - "$F" <<'PYEOF'
import sys
src = open(sys.argv[1], encoding='utf-8').read()
# letベンチマーク(5束縛のlet*をループ内で回すだけの関数)を代表として見る
head = 'tco_result_t lisp_ll_bench_aot_let__step_fixed(lisp_val_t env, lisp_val_t __arg0) {'
if head not in src:
    sys.exit('ERROR: %%bench-aot-let の生成コードが見つかりません(関数名が変わった?)')
i = src.index(head)
body = src[i:src.index('\n}\n', i)]
inl = body.count('__inl_')
lifted = body.count('os_make_lifted_closure')
fc = body.count('primitive_funcall')
print(f'%%bench-aot-let: __inl_={inl} os_make_lifted_closure={lifted} primitive_funcall={fc}')
bad = []
if inl == 0:
    bad.append('__inl_ が1つも無い = letインライナが発火していない')
if lifted:
    bad.append(f'os_make_lifted_closure が {lifted} 箇所 = フォールバック経路に落ちている')
if fc:
    bad.append(f'primitive_funcall が {fc} 箇所 = フォールバック経路に落ちている')
if bad:
    print('NG: ' + ' / '.join(bad))
    sys.exit(1)
print('OK: letインライナは発火している')
PYEOF
