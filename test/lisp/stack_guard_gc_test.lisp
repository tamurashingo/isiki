;; test/lisp/stack_guard_gc_test.lisp
;;
;; [GCデバッグ] **深い再帰から一気に巻き戻しても GC ルートの LIFO が壊れないこと。**
;; issue #110 / documents/stack-guard.md §6-2。
;; **GC_DEBUG ビルドでしか意味が無い**ので、通常ビルドで走ったら落ちる
;; (test/lisp/gc_debug_guard.lisp)。
;;
;; なぜ要るか:
;;   `<storage-exhausted>` は**深さ 27 付近で signal される。** そこから
;;   トップレベルまで戻るので、**GC_PROTECT の cleanup が一気に大量に走る。**
;;   JIT フレームは 1 段ごとに env / args / 引数スロットを GC ルートへ link
;;   しているので、巻き戻しの順序が崩れると `g_gc_lifo_violations` が立つ。
;;
;;   `%%DIAG-GC-LIFO-VIOLATIONS` は**解放の順序が後入れ先出しでなかった回数**。
;;   0 以外なら、ルートチェーンが壊れた状態で GC が走りうる。

(if (require-gc-debug-build "stack_guard_gc_test")
    (progn
      (defun sggc-rec (n) (if (= n 0) 0 (+ n (sggc-rec (- n 1)))))
      (assert-equal t (%%za-compiled-p (function sggc-rec)))

      ;; **強度の選び方(実測で決めた)**
      ;;   %%DIAG-GC-STRESS 1(確保ごとに GC)は**使えない。** 25 分で終わらなかった。
      ;;   signal 経路は make-instance / signal-condition / ハンドラで数百回確保し、
      ;;   そのたびに init.lisp を読み込んだヒープ全体をコピーすることになる
      ;;   (test/lisp/for_gc_stress_test.lisp が 1 を使えるのは、対象が
      ;;    for ループ 500 反復という小さい区間だから)。
      ;;   **設定はそのブート全体に効く**ので、使い終わったらすぐ 0 へ戻す。
      (%%diag-gc-stress 20)
      (assert-error-class '<storage-exhausted> (sggc-rec 1000))
      (%%diag-gc-stress 0)
      (assert-equal 210 (sggc-rec 20))

      (%%diag-gc-stress 5)
      (assert-error-class '<storage-exhausted> (sggc-rec 1000))
      (%%diag-gc-stress 0)
      (assert-equal 210 (sggc-rec 20))

      ;; **巻き戻しが安全だったこと。** 0 以外ならルートチェーンが壊れている。
      ;; 陽性対照: この期待値を 999 にすると落ちる(確認済み。規則 8)
      (assert-equal 0 (%%diag-gc-lifo-violations))

      ;; signal の打ち切りにも当たっていないこと
      (assert-equal 0 (%%diag-signal-overflows))

      (format *isiki-test-stream* "#stack-guard-gc lifo=~D signal-ovf=~D gc=~D~%"
              (%%diag-gc-lifo-violations) (%%diag-signal-overflows) (%%gc-collect-count))
      (finish-output *isiki-test-stream*))
  nil)
