;; test/lisp/for_expansion_test.lisp
;;
;; [性能測定] forマクロの展開形を「ループ本体で毎回letを評価する方式」から
;; 「一時変数をループ外で一度だけ束縛しsetqだけで更新する方式」へ変更した際の
;; 意味論の回帰テスト(documents/performance-measurement.md
;; 「forのtagbody直行化」節)。
;;
;; 変更で最も壊しやすいのは並列束縛の意味論(全step式を評価しきってから
;; 各束縛変数へ書き戻す)なので、互いの旧値を参照するstepを最初に確認する。


;; --- 並列束縛の意味論(最重要) ---
;; (a 0 b) (b 1 (+ a b)) は互いの「旧値」を参照する。
;; 並列なら 0,1 -> 1,1 -> 1,2 -> 2,3 -> 3,5 -> 5,8 -> 8,13 -> 13,21 で a=13。
;; 逐次に壊れていると a=16 になる。
(defun %%for-parallel-check ()
  (for ((a 0 b) (b 1 (+ a b))) ((> a 10) a)))
(assert-equal 13 (%%for-parallel-check))

;; --- step式を省略した束縛は変化しない ---
(defun %%for-nostep-check ()
  (for ((i 0 (+ i 1)) (k 42)) ((>= i 3) k)))
(assert-equal 42 (%%for-nostep-check))

;; --- 既存の基本ケース(init_test.lispと同じ) ---
(defun %%for-basic-check ()
  (for ((i 1 (+ i 1)) (sum 0 (+ sum i))) ((> i 5) sum)))
(assert-equal 15 (%%for-basic-check))

;; --- 本体が空・結果式が複数 ---
(defun %%for-multi-result-check ()
  (for ((i 0 (+ i 1))) ((>= i 3) 99 (* i 10))))
(assert-equal 30 (%%for-multi-result-check))

;; --- 入れ子のfor(一時変数名が衝突しないこと) ---
(defun %%for-nested-check ()
  (for ((i 0 (+ i 1)) (total 0 (+ total (for ((j 0 (+ j 1)) (s 0 (+ s j))) ((>= j 3) s)))))
       ((>= i 2) total)))
(assert-equal 6 (%%for-nested-check))

;; --- GCと併走しても結果が壊れないこと(ide.lispが記録していた既知バグの確認) ---
(defun %%for-gc-check (n)
  (for ((i 0 (+ i 1)) (sum 0 (+ sum i)) (junk nil (create-vector 500 0)))
       ((>= i n) sum)))
(assert-equal 499500 (%%for-gc-check 1000))

;; --- Immobilized Spaceを消費しないこと ---
(defglobal fb0 (%%imm-space-used-bytes))
(defglobal fr (%%for-gc-check 2000))
(defglobal fb1 (%%imm-space-used-bytes))
(assert-equal 1999000 fr)
(assert-equal 0 (- fb1 fb0))
