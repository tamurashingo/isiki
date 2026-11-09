;; test/lisp/qemu_boot_m1_cost.lisp
;;
;; [M1-T6 / issue #185] **同梱した基盤ライブラリを起動ごとに読む費用を測る。**
;; 記録と予測は documents/boot-embedded-init-cost.md。
;;
;; **これは計測であって合否の判定ではない。** 合格線は決めない(決定 D5 / D6)。
;; ここで固定するのは「計器が値を返すこと」と「不変条件(使用量 < 総量)」だけで、
;; **費用そのものに閾値は置かない**。
;;
;; ## なぜ 1 本の台本で両方の状態を測れるのか
;;
;; この台本は **同梱あり / 同梱なし(NO_EMBEDDED_INIT=1)の両方で走る**。
;; そのために、基盤ライブラリ(init.lisp)が読まれていない状態 = 「状態(a)」でも
;; 使える形だけで書いてある:
;;   - let / while / backquote といった init.lisp のマクロは**使わない**
;;   - 条件システム(assert-* / test_framework.lisp)は**使わない**
;;     (あちらは init.lisp を前提にしており、状態(a)では黙って何も数えない)
;; 前例は test/lisp/qemu_boot_no_init_signal.lisp。あちらと同じ作法である。
;;
;; ## なぜ先頭で採取するのか
;;
;; kernel.c の run_boot_entry は「同梱 → 台本」の順に実行する。
;; したがって**この台本の最初のフォームが評価される時点**が
;; 「ブートから対話を受け付けられるようになった時点」である
;; (素の起動では、ここで REPL が最初のプロンプトを出す)。
;; 採取を先頭に置かないと、台本自身の defun が JIT され、
;; imm space とヒープに台本のぶんが混ざる。
;;
;; **既知の偏り(規則 10)**: 9P を開いて台本の 1 フォーム目を読むぶんは
;; この採取に含まれる。素の REPL 起動には無い費用である。
;; 同梱あり / なしの**両方に同じだけ乗る**ので、差には効かない。

;;; ======================= 採取(ここが最初の 3 フォーム) =======================
(defglobal *m1-tick* (get-internal-real-time))
(defglobal *m1-heap* (%%heap-used-bytes))
(defglobal *m1-imm* (%%imm-space-used-bytes))

;;; 解釈に必要な周辺の量。**%%HEAP-USED-BYTES は From 空間の bump なので
;;; GC で巻き戻る。** GC 回数が 0 でなければ、この値は「累積確保量」ではなく
;;; 「最後の GC 以後の量 + 生存集合」である(規則 10)
(defglobal *m1-gc* (%%gc-collect-count))
(defglobal *m1-fncells* (%%diag-fn-cell-count))
(defglobal *m1-heap-total* (%%heap-total-bytes))
(defglobal *m1-imm-total* (%%imm-space-total-bytes))
(defglobal *m1-boot-alloc* (%%boot-alloc-used-bytes))

;;; ============================== 出力と不変条件 ==============================
(defglobal *s* (open-output-file "/9p/test-results.txt"))
(defglobal *pass* 0)
(defglobal *fail* 0)

(defun ck (label ok)
  (if ok
      (setq *pass* (+ *pass* 1))
    (progn
      (setq *fail* (+ *fail* 1))
      (format *s* "[NG] ~A~%" label)))
  (format *s* "#ck ~A~%" label)
  (finish-output *s*))

;;; **数字は 1 行 1 項目で出す。** ホスト側(tools/boot_cost_report.sh)が
;;; grep して表に組む。固定値は書かない(規則 13)
(format *s* "#m1 tick=~D~%" *m1-tick*)
(format *s* "#m1 heap-used=~D~%" *m1-heap*)
(format *s* "#m1 heap-total=~D~%" *m1-heap-total*)
(format *s* "#m1 imm-used=~D~%" *m1-imm*)
(format *s* "#m1 imm-total=~D~%" *m1-imm-total*)
(format *s* "#m1 gc-count=~D~%" *m1-gc*)
(format *s* "#m1 fn-cells=~D~%" *m1-fncells*)
(format *s* "#m1 boot-alloc-used=~D~%" *m1-boot-alloc*)
(finish-output *s*)

;;; 不変条件のみ。**費用に閾値は置かない**(D5 / D6)
(ck "tick-nonneg" (>= *m1-tick* 0))
(ck "heap-used-positive" (> *m1-heap* 0))
(ck "heap-within-total" (< *m1-heap* *m1-heap-total*))
(ck "imm-used-positive" (> *m1-imm* 0))
(ck "imm-within-total" (< *m1-imm* *m1-imm-total*))
(ck "fn-cells-positive" (> *m1-fncells* 0))

(format *s* "~%==== isiki tests: ~D passed, ~D failed ====~%" *pass* *fail*)  ;; PASS-LINE: SELF-TALLY 同梱の費用を測る段。枠組みを読むと測る対象が変わる
(close *s*)
