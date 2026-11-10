;; test/lisp/qemu_boot_declare_bench.lisp
;;
;; 型宣言の費用をコンパイル時と実行時に分けて測る計器の入口
;; (documents/declare-types.md)。本体は test/lisp/declare_types_bench.lisp。
;;
;; [#208] **自動の周回に入れた。消すのをやめた。** 経緯を残す:
;;
;; #208 の設計は「前提が逆転して**嘘を固定している**計器だから消す」としていた。
;; 根拠は `declare_types_bench.lisp:140-142` の「宣言の有無で生成コードの長さが
;; 一致する」3 件で、#128 の作業(比較の型特化 / `*` `/` の宣言制御インライン展開)で
;; その前提は意図的に逆転させた、という読みである。
;;
;; **実物を走らせたら 19 件すべて通った**(2026-11-09。単一段を 1 回。82 秒):
;;
;;     [DTBENCH] code-len fixnum=2432 single=2436 double=2536 (宣言あり/なしで一致)
;;     ==== isiki tests: 19 passed, 0 failed ====
;;
;; 逆転しなかった理由は `src/c/za.c` の `za_operand_decl_type` である。
;; 特化版/展開が選ばれるのは**両方のオペランドの型が宣言から分かるとき**だけで、
;; この計器の演算は必ず片側がリテラルか式である((= n 0) / (- n 1) / (* n 3) /
;; (+ acc (* n 3)))。だから特化版は選ばれず、長さは今も一致する。
;;
;; つまりこの 3 件は嘘ではなく、**「片側がリテラル/式なら宣言は生成コードを変えない」
;; という境界を押さえている現役の不変条件**である。そして常時走る段に同等物が無い
;; (`test/lisp/inline_decl_test.lisp:240-243` と
;;  `test/lisp/inline_muldiv_test.lisp:44-45` は**両オペランド宣言で差が出る**ほう、
;;  つまり境界の反対側を固定している)。
;; **覆われていないものを消すのは、検出器を減らすことである。**
;;
;; [#203] 本体は**素の load ではなく isiki-test-load で読む**(件数の行を出すため)

(load "test/lisp/test_framework.lisp")
(isiki-test-load "test/lisp/declare_types_bench.lisp")

(isiki-test-report)
(close *isiki-test-stream*)
