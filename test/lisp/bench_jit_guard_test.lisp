;; test/lisp/bench_jit_guard_test.lisp
;;
;; [性能測定] test/lisp/bench_jit_guard.lisp の**陰性対照と陽性対照**。
;; documents/performance-measurement.md「JIT 経路の基準値」節、規則 8。
;;
;; 「JIT に乗っていることを assert した」と書くだけでは足りない。
;; **わざと JIT に乗らない形を作って、検査が落ちることを見る。**
;; PR #90 と PR #94 で検出器自体が壊れていた例が 2 件ある。

;;; --- 陰性対照: JIT に乗っている関数では guard が t を返す ---
(assert-equal t (bench-jit-guard "%%bench-jit-loop" (function %%bench-jit-loop)))
(assert-equal 0 *bench-jit-guard-ng*)
(assert-equal t (bench-jit-guard-gate))

;;; --- 陽性対照: わざと JIT に乗らない形 ---
;; %%bench-jit-loop と**同じ計算**を、パラメータ n へ直接 setq する形で書く。
;; パラメータへの setq は JIT 非対応(documents/control-transfer-survey.md:485)。
;;
;; 対照群は嘘をついていてはならない(規則 5)。この関数は JIT に乗らないだけで、
;; インタプリタでは正しい答えを返す。「壊れた関数だから落ちた」ではないことを
;; 戻り値で確かめる。
;;
;; **なぜ labels / tagbody を使わなかったか(否定の結果、規則 6):**
;; 作業指示書は「labels を使う(以前インタプリタに落ちると確認済み)」と
;; 書いていたが、2026-10-02 に実測したところ labels も tagbody/go も
;; %%za-compiled-p が T になる(JIT が対応するようになっている)。
;; 候補 4 つを 1 ブートで測った実測:
;;     tagbody=T  labels=T  パラメータへの setq=NIL  素の let+while=T
;; 以前の記録(documents/bench-pinned-nil.md:149)は現状ではない。
(defun bench-jit-not-compiled-loop (n)
  (progn
    (while (> n 0)
      (setq n (- n 1)))
    n))

;; 前提: この形は JIT に乗らない
(assert-equal nil (%%za-compiled-p (function bench-jit-not-compiled-loop)))
;; 前提: それでも計算は正しい(対照群が嘘をついていない)
(assert-equal 0 (bench-jit-not-compiled-loop 2000))
(assert-equal (%%bench-jit-loop 2000) (bench-jit-not-compiled-loop 2000))

;; 本題: guard がこれを nil と判定し、件数を数えること
(assert-equal nil (bench-jit-guard "bench-jit-not-compiled-loop"
                                   (function bench-jit-not-compiled-loop)))
(assert-equal 1 *bench-jit-guard-ng*)

;; 本題: gate が nil を返し、failed を 1 増やして make を落とすこと
(defglobal bench-jit-fail-before *isiki-test-fail*)
(assert-equal nil (bench-jit-guard-gate))
(defglobal bench-jit-fail-delta (- *isiki-test-fail* bench-jit-fail-before))

;; 後始末。陽性対照で**意図的に**増やした failed を戻す。
;; 戻さないとこのテストファイル自身が " 0 failed" を壊す
(setq *isiki-test-fail* bench-jit-fail-before)
(setq *bench-jit-guard-ng* 0)

;; 増えたのがちょうど 1 件だったことを、戻した後に確認する
(assert-equal 1 bench-jit-fail-delta)
;; 後始末が効いていること
(assert-equal t (bench-jit-guard-gate))
