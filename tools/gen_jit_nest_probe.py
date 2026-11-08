#!/usr/bin/env python3
"""入れ子 tagbody の深さを振って JIT の断念を測る題材を作る(issue #142)。

使い方:
    python3 tools/gen_jit_nest_probe.py 30 > tmp/probe.lisp
    make test-qemu-milestone MILESTONE=tmp/probe.lisp \\
         QEMU_EXTRA_FLAGS="-serial file:$PWD/tmp/serial.txt"

題材は `test/lisp/jit_limits_test.lisp` の `jlm-trunc` と同じ形である:
    (progn (tagbody ... (let ((a 1) (b 2) (c 3) (d 4) (e 5)) (+ a b c d e)) ... t))
を N 段。**let の束縛は 5 つ**で ZA_MAX_LOCALS_PER_LET(4)を超えるので、
正常なら `ZA_MAX_LOCALS_PER_LET` の断念になる。**形の検査が先に出たら破壊である。**

30 段以降を見るには `ZA_MAX_COMPILE_NEST` を一時的に上げる必要がある
(既定の 60 は 1 段 2 単位で約 29 段で尽きる)。**上げたら必ず戻すこと。**
詳細と実測の階段は documents/142-shape-bail-investigation.md にある。

**入れ子は再帰で組み立てる。** 閉じ括弧を手で並べると
`t` が `let` の本体に入る間違いをする(実際にやった)。
"""
import sys

LET = "(let ((a 1) (b 2) (c 3) (d 4) (e 5)) (+ a b c d e))"


def probe_form(n):
    s = LET
    for _ in range(n):
        s = "(progn (tagbody " + s + " t))"
    form = "(defun f%d (x) %s)" % (n, s)
    # 数は機械で確かめる(段数のずれで 1 度嘘の表を作った)
    assert form.count("(") == form.count(")"), "括弧が合わない"
    assert form.count("(tagbody") == n, "段数が合わない"
    return form


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: gen_jit_nest_probe.py <段数>")
    n = int(sys.argv[1])
    print('(defglobal *o* (open-output-file "/9p/tmp/jit_nest_probe.txt"))')
    print(probe_form(n))
    print('(format *o* "#probe %d compiled=~S bail-at0=~D bail-at1=~D count=~D~%%" '
          '(%%%%za-compiled-p (function f%d)) (%%%%diag-za-bail-at 0) '
          '(%%%%diag-za-bail-at 1) (%%%%diag-za-bail-count))' % (n, n))
    print('(finish-output *o*) (close *o*)')
    # ミルストンの判定("0 failed" を grep される)を満たすために書く
    print('(defglobal *r* (open-output-file "/9p/test-results.txt"))')
    print('(format *r* "~%==== isiki tests: 1 passed, 0 failed ====~%")')
    print('(finish-output *r*) (close *r*)')


if __name__ == "__main__":
    main()
