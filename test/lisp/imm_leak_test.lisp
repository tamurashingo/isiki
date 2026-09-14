;; test/lisp/imm_leak_test.lisp
;;
;; [性能測定] Phase1の受け入れ条件(documents/performance-measurement.md
;; 「letのImmobilized Spaceリーク」節)。
;;
;; 修正前は、letを1回評価するたびにos_make_lifted_closureが za_fn_meta_t を
;; Immobilized Space(4MB固定・GC非対象・解放手段なし)から32byte確保しており、
;; ループ内の5変数let*は約24,300反復でこれを使い切ってOSを停止させていた。
;; 修正後はmetaを呼び出し箇所ごとのC静的変数として持つため、実行回数に比例した
;; 消費はゼロになる。
;;
;; %%imm-space-used-bytes はPhase1でページ粒度からバイト粒度へ修正済み
;; (旧実装では4096byte未満の消費が「0 byte」に見え、この回帰を検出できなかった)。

;;; --- 実行回数に比例した消費がゼロであること ---

;; ループ内の5変数let*(修正前に停止を引き起こしていた当のコード)
(defglobal imm-before-let (%%imm-space-used-bytes))
(defglobal imm-let-result (%%bench-aot-let 50000))
(defglobal imm-after-let (%%imm-space-used-bytes))
(assert-equal 0 (- imm-after-let imm-before-let))
;; 計算結果も正しいこと(0+1+...+49999 に 4*50000 を足したもの)
(assert-equal 1250175000 imm-let-result)

;; forマクロ(letと同じくlambdaを展開形に含む)
(defglobal imm-before-for (%%imm-space-used-bytes))
(defglobal imm-for-result (%%bench-aot-for 50000))
(defglobal imm-after-for (%%imm-space-used-bytes))
(assert-equal 0 (- imm-after-for imm-before-for))
(assert-equal 1249975000 imm-for-result)

;; letを使わず、ループ内でlambdaを値として生成するケース。
;; letのインライン化(Phase2)で隠蔽されていないことの確認として必須。
(defun %%imm-leak-make-closure (x)
  (lambda (y) (+ x y)))

(defun %%imm-leak-closure-loop (n)
  (let ((i 0) (acc 0))
    (progn
      (while (< i n)
        (progn
          (setq acc (funcall (%%imm-leak-make-closure i) 1))
          (setq i (+ i 1))))
      acc)))


;; この2関数はトップレベルdefunなのでza.cにJITコンパイルされる。AOT経路
;; (bench_aot.lisp由来の%%bench-aot-let)だけでなくJIT経路も確認するため、
;; 実際にJIT済みであることを明示的に確認してから計測する
(assert-equal t (%%za-compiled-p (function %%imm-leak-make-closure)))
(assert-equal t (%%za-compiled-p (function %%imm-leak-closure-loop)))

(defglobal imm-before-closure (%%imm-space-used-bytes))
(defglobal imm-closure-result (%%imm-leak-closure-loop 20000))
(defglobal imm-after-closure (%%imm-space-used-bytes))
(assert-equal 20000 imm-closure-result)
(assert-equal 0 (- imm-after-closure imm-before-closure))

;;; --- 各クロージャが自分のcaptured_envを正しく参照すること ---
;; metaをfnptr単位で共有しても、捕捉環境はインスタンス側(word3)に入るため
;; 混線しないことの直接確認。
(defglobal imm-c1 (%%imm-leak-make-closure 10))
(defglobal imm-c2 (%%imm-leak-make-closure 20))
(defglobal imm-c3 (%%imm-leak-make-closure 30))
(assert-equal 11 (funcall imm-c1 1))
(assert-equal 21 (funcall imm-c2 1))
(assert-equal 31 (funcall imm-c3 1))
;; 生成順と逆順に呼んでも独立していること
(assert-equal 35 (funcall imm-c3 5))
(assert-equal 15 (funcall imm-c1 5))

;;; --- GCと併走しても壊れないこと ---
;; クロージャ生成とGC誘発を交互に行い、捕捉した値が化けないことを確認する。
(defun %%imm-leak-churn (n)
  (let ((i 0) (junk nil))
    (progn
      (while (< i n)
        (progn
          (setq junk (create-vector 2000 0))
          (setq i (+ i 1))))
      (length junk))))

(defglobal imm-c-gc (%%imm-leak-make-closure 777))
(defglobal imm-gc-junk (%%imm-leak-churn 300))
(assert-equal 2000 imm-gc-junk)
(assert-equal 778 (funcall imm-c-gc 1))
