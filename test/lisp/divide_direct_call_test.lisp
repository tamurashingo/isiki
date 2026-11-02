;; test/lisp/divide_direct_call_test.lisp
;;
;; / を za_syms_t へ登録し、二項の直接 call 経路に乗せる
;; (documents/divide-direct-call.md)。
;;
;; **ISLisp に / は無い。** quotient の実装(init.lisp の %quotient2)が内部で
;; 使う経路である。本作業は経路を変えるだけで、**意味論は一切変えない。**

(defun dd-div (a b) (/ a b))
(defun dd-quo (a b) (quotient a b))
(defun dd-rec (x) (reciprocal x))
(assert-equal t (%%za-compiled-p (function dd-div)))
(assert-equal t (%%za-compiled-p (function dd-quo)))

;;; --- 意味論が変わらないこと(§5-2) ---
(assert-equal 2 (dd-div 6 3))
(assert-equal 3 (dd-div 7 2))        ; ゼロ方向への切り捨て
(assert-equal -3 (dd-div -7 2))      ; **床関数なら -4。ゼロ方向である**
(assert-equal 2 (dd-div -6 -3))
(assert-equal -2 (dd-div 6 -3))
;; n 項は従来どおり(二項経路に乗らない)
(assert-equal 0 (/ 1 2 3))
(assert-equal 2 (/ 12 3 2))
;; 型昇格(PR #80)
(assert-equal '<single-float> (%%class-name (class-of (dd-div 1.0f0 2.0f0))))
(assert-equal '<double-float> (%%class-name (class-of (dd-div 1.0f0 2.0d0))))
(assert-equal '<single-float> (%%class-name (class-of (dd-div 1 2.0f0))))
(assert-equal t (integerp (dd-div 6 3)))
;; ゼロ除算は IEEE どおり(quotient とは別。下で確認する)
(defun dd-pr (x) (let ((s (create-string-output-stream))) (format s "~S" x) (get-output-stream-string s)))
(assert-equal "INF" (dd-pr (dd-div 1.0d0 0.0d0)))
(assert-equal "-INF" (dd-pr (dd-div -1.0d0 0.0d0)))
(assert-equal "NAN" (dd-pr (dd-div 0.0d0 0.0d0)))
(assert-equal "INF" (dd-pr (dd-div 1.0f0 0.0f0)))

;;; --- 確保する結果でも正しいこと(§5-1) ---
;;; **za_compile_binary はこれまで比較(確保しない)しか扱っていなかった。**
;;; bignum と double は確保するので、そこが壊れていないことを見る。
(assert-equal 422550200076076467165567735125 (dd-div (expt 2 100) 3))
(assert-equal t (> (dd-div (expt 2 200) 7) (expt 2 190)))
(assert-equal "0.3333333333333333" (dd-pr (dd-div 1.0d0 3.0d0)))
(assert-equal "0.33333334f0" (dd-pr (dd-div 1.0f0 3.0f0)))
;; GC を跨いで繰り返しても正しいこと
(defun dd-bignum-loop (n acc)
  (if (= n 0) acc (dd-bignum-loop (- n 1) (dd-div (expt 2 100) 3))))
(assert-equal 422550200076076467165567735125 (dd-bignum-loop 200 0))
(defun dd-double-loop (n acc)
  (if (= n 0) acc (dd-double-loop (- n 1) (dd-div 1.0d0 3.0d0))))
(assert-equal "0.3333333333333333" (dd-pr (dd-double-loop 200 0.0d0)))
;; JIT の即値焼き込み監査(GC で動く領域を指す即値が 0 であること)
(assert-equal 0 (%%za-heap-imm-count))

;;; --- quotient / reciprocal が変わっていないこと ---
(assert-equal 2 (dd-quo 4 2))
(assert-equal 3.5 (dd-quo 7 2))       ; 割り切れないので float
(assert-equal -3.5 (dd-quo -7 2))
(assert-equal t (floatp (dd-quo 1 2)))
(assert-equal '<single-float> (%%class-name (class-of (dd-quo 1.0f0 3.0f0))))
;; quotient のゼロ除算は <division-by-zero>(/ の INF とは異なる)
(assert-error (quotient 1 0))
(assert-error (quotient 1.0f0 0.0f0))
(assert-error (reciprocal 0))
(assert-equal 0.5 (dd-rec 2.0d0))
(assert-equal '<single-float> (%%class-name (class-of (dd-rec 2.0f0))))
