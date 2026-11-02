;; test/lisp/bench_jit_eval_control.lisp
;;
;; [性能測定] **os_eval ゲートの陽性対照。**
;; documents/performance-measurement.md「インタプリタ落ちの検出器」節、規則 8。
;;
;; PR #115 で分かったこと:
;;   **%%za-compiled-p が T でも足りなかった。**
;;   あれは「この defun が JIT コンパイルされたか」に答えるだけで、
;;   「本体が全部ネイティブで走るか」には答えない。
;;
;; そこで「os_eval の命令数が N に対して増えないこと」をゲートに足した。
;; **足した検査が本当に働くかは、わざと落ちる形を作って見るまで分からない**
;; (PR #90 と PR #94 で検出器自体が壊れていた例が 2 件ある)。
;;
;; ここに置く対照は、**ゲート1 を通ってゲート2 で落ちる**形である。
;; それでなければ「%%za-compiled-p では足りない」ことの対照にならない。
;;
;;   %%bench-jit-evalfall   外側。%%za-compiled-p = T(JIT に乗る)
;;   %%bench-eval-callee    呼び先。%%za-compiled-p = NIL(インタプリタ)
;;                          パラメータへの setq は JIT 非対応
;;                          (documents/jit-unsupported-syntax.md)
;;
;; 外側は JIT なのに、ループ本体が毎反復インタプリタの関数を呼ぶので
;; os_eval へ落ちる。使い方:
;;
;;   BENCH_PATHS=jit BENCH_CASES=evalfall BENCH_REPEAT=1 \
;;     BENCH_NO_CHECK=1 make test-qemu-jit-bench     -> ゲート2 で落ちる
;;
;; 対照群は嘘をついていてはならない(規則 5)。この 2 つは JIT に乗る/乗らないが
;; 違うだけで、計算は %%bench-jit-funcall と同じ(acc を n 回 1 増やす)である。

;; 呼び先。**パラメータ x へ直接 setq するので JIT に乗らない。**
;; インタプリタで評価されるので、呼ばれるたびに os_eval を通る
(defun %%bench-eval-callee (x)
  (progn
    (setq x (+ x 1))
    x))

;; 外側。形は %%bench-jit-funcall と同じで、呼び先だけ差し替えてある
(defun %%bench-jit-evalfall (n)
  (let ((acc 0) (i 0))
    (progn
      (while (< i n)
        (progn
          (setq acc (%%bench-eval-callee acc))
          (setq i (+ i 1))))
      acc)))
