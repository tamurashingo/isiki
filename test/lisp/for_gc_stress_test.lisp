;; test/lisp/for_gc_stress_test.lisp
;;
;; [GCデバッグ] **GC を強制しても `for` の結果が壊れないこと。**
;; documents/for-expansion.md §1。**GC_DEBUG ビルドでしか意味が無い**ので、
;; 通常ビルドで走ったら落ちる(test/lisp/gc_debug_guard.lisp)。
;;
;; 由来: `src/lisp/ide.lisp` が「`for` マクロは GC が特定のタイミングで走ると
;; 以後永久に(そのループ内での)結果が壊れる」と記録していた。
;; **根拠として挙げられていた documents/partition.md はどのブランチにも存在せず、
;; 再現手順も残っていない。** このファイルは、その主張に対して残せる唯一の形
;; (「少なくともこの条件では壊れない」)である。
;;
;; **これは「再現しない」ことの証明ではない。** %%DIAG-GC-STRESS は
;; os_alloc_bytes の中でしか GC を起こさないので、確保を含まない区間には
;; GC を置けない。その区間については構造の議論で閉じている
;; (GC の起動点は確保の中 / REPL のセーフポイント / 診断組み込みの 3 つだけ)。

(if (require-gc-debug-build "for_gc_stress_test")
    (progn
      ;; 期待値: n=2000 なら 0+1+...+1999 = 1999000 / cons 版は長さ 2000
      (%%diag-gc-stress 10)
      (assert-equal 1999000 (for ((i 0 (+ i 1)) (acc 0 (+ acc i))) ((>= i 2000) acc)))
      (assert-equal 1999000 (%%bench-jit-for 2000))
      (assert-equal 1999000 (%%bench-aot-for 2000))
      (assert-equal 2000 (for ((i 0 (+ i 1)) (acc nil (cons i acc))) ((>= i 2000) (length acc))))
      ;; 確保ごとに GC(いちばん強い)。n は小さくする
      (%%diag-gc-stress 1)
      (assert-equal 124750 (for ((i 0 (+ i 1)) (acc 0 (+ acc i))) ((>= i 500) acc)))
      (assert-equal 500 (for ((i 0 (+ i 1)) (acc nil (cons i acc))) ((>= i 500) (length acc))))
      (assert-equal 124750 (%%bench-jit-for 500))
      (assert-equal 124750 (%%bench-aot-for 500))
      ;; 並列代入も GC 強制下で崩れないこと(3 経路)
      (assert-equal 10 (for ((i 0 j) (j 1 i) (k 0 (+ k 1))) ((>= k 1) (+ (* i 10) j))))
      (assert-equal 10 (%%bench-jit-for-swap 1))
      (assert-equal 10 (%%bench-aot-for-swap 1))
      ;; **GC が実際に走ったことを確かめる。**走っていなければこのテストは無意味
      (assert-equal t (if (> (%%gc-collect-count) 100) t nil))
      (%%diag-gc-stress 0)
      ;; stale ポインタの読み出しが 0 件であること。
      ;; **GC_PAINT を併用しないと trap は常に 0 なので、その場合この行は
      ;;   「壊れていない」の証拠にはならない**(GC_PAINT=1 のときだけ意味を持つ)
      (assert-equal 0 (%%diag-gc-trap-hits)))
  nil)
