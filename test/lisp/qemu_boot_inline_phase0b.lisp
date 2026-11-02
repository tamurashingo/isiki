;; test/lisp/qemu_boot_inline_phase0b.lisp
;;
;; Phase 0 の追測(レビュー指摘 1〜3)。
;;   1. single の「宣言あり 24.9 が 4 演算とも一致」は計算が消えていないか
;;   2. double の「宣言あり/なしが完全一致」は宣言が届いていないのではないか
;;   3. * / の「宣言ありの方が速い」の中身は何か
;;
;; **結果を累積して最後に出力する形**でも測り、値が正しいことを確認する。

(load "src/lisp/init.lisp")
(load "test/lisp/test_framework.lisp")
(format *isiki-test-stream* "#gate ~A~%" (%%gate-plugin-args))
(finish-output *isiki-test-stream*)

(defun pb-callee (name callee)
  (let ((items (disasm-items (%%disasm-code-base name) (%%disasm-code-len name)))
        (found nil))
    (while (not (null items))
      (if (string= callee (disasm-item-comment (cdr (car items)))) (setq found t) nil)
      (setq items (cdr items)))
    found))

(defun pb-agen (x y) (+ x y))
(defun pb-mgen (x y) (* x y))
(defun pb-afix (x y) (declare (type <fixnum> x)) (declare (type <fixnum> y)) (+ x y))
(defun pb-asgl (x y) (declare (type <single-float> x)) (declare (type <single-float> y)) (+ x y))
(defun pb-msgl (x y) (declare (type <single-float> x)) (declare (type <single-float> y)) (* x y))
(defun pb-dsgl (x y) (declare (type <single-float> x)) (declare (type <single-float> y)) (/ x y))
(defun pb-adbl (x y) (declare (type <double-float> x)) (declare (type <double-float> y)) (+ x y))
(defun pb-mdbl (x y) (declare (type <double-float> x)) (declare (type <double-float> y)) (* x y))
(defun pb-id  (x y) x)

;;; --- 2. double の宣言はどこへ行っているか(生成コードで見る)---
(format *isiki-test-stream* "=== 呼び先(シンボル名)===~%")
(defun pb-show (name)
  (format *isiki-test-stream* "  ~A len=~D  generic=~S fix=~S sgl=~S dbl-special=~S~%"
          name (%%disasm-code-len name)
          (pb-callee name "primitive_add2")
          (pb-callee name "primitive_add2_fixnum")
          (pb-callee name "primitive_add2_single")
          nil)
  (finish-output *isiki-test-stream*))
(pb-show 'pb-agen) (pb-show 'pb-afix) (pb-show 'pb-asgl) (pb-show 'pb-adbl)
;; **double は宣言しても GENERIC のまま。** 宣言自体は記録されている
(format *isiki-test-stream* "  %%declared-types-of pb-adbl = ~S~%" (%%declared-types-of 'pb-adbl))
(format *isiki-test-stream* "  pb-agen と pb-adbl の code-len = ~D / ~D~%"
        (%%disasm-code-len 'pb-agen) (%%disasm-code-len 'pb-adbl))
(finish-output *isiki-test-stream*)
;; double 専用の特化版はそもそも存在しない(呼び先が GENERIC で一致する)
(assert-equal t (pb-callee 'pb-adbl "primitive_add2"))
(assert-equal t (pb-callee 'pb-agen "primitive_add2"))
(assert-equal nil (pb-callee 'pb-adbl "primitive_add2_fixnum"))
(assert-equal nil (pb-callee 'pb-adbl "primitive_add2_single"))

;;; --- 1. 計算が消えていないか。**結果を累積して出力する** ---
(defun pb-acc-f (f a b n z)
  (let ((i 0) (s z))
    (while (< i n) (setq s (+ s (funcall f a b))) (setq i (+ i 1)))
    s))
(defun pb-plain (f a b n)
  (let ((i 0) (r nil)) (while (< i n) (setq r (funcall f a b)) (setq i (+ i 1))) r))

(defglobal *pb-n* 200000)
(defglobal *pb-warm* 2000)
(defun pb-warm (f a b z) (pb-acc-f f a b *pb-warm* z) (pb-plain f a b *pb-warm*))
(pb-warm (function pb-id)   1 2 0)
(pb-warm (function pb-afix) 3 4 0)
(pb-warm (function pb-asgl) 1.5f0 2.25f0 0.0f0)
(pb-warm (function pb-msgl) 1.5f0 2.0f0  0.0f0)
(pb-warm (function pb-dsgl) 3.0f0 2.0f0  0.0f0)
(pb-warm (function pb-adbl) 1.5d0 2.25d0 0.0d0)
(pb-warm (function pb-agen) 1.5f0 2.25f0 0.0f0)
(pb-warm (function pb-mgen) 1.5f0 2.0f0  0.0f0)

(format *isiki-test-stream* "=== 累積した値(計算が消えていないことの確認)===~%")
(format *isiki-test-stream* "  single + の累積 = ~S  (1 回 3.75、~D 回)~%"
        (pb-acc-f (function pb-asgl) 1.5f0 2.25f0 *pb-n* 0.0f0) *pb-n*)
(format *isiki-test-stream* "  single * の累積 = ~S  (1 回 3.0)~%"
        (pb-acc-f (function pb-msgl) 1.5f0 2.0f0 *pb-n* 0.0f0))
(format *isiki-test-stream* "  single / の累積 = ~S  (1 回 1.5)~%"
        (pb-acc-f (function pb-dsgl) 3.0f0 2.0f0 *pb-n* 0.0f0))
(format *isiki-test-stream* "  fixnum + の累積 = ~S  (1 回 7)~%"
        (pb-acc-f (function pb-afix) 3 4 *pb-n* 0))
(format *isiki-test-stream* "  double + の累積 = ~S  (1 回 3.75)~%"
        (pb-acc-f (function pb-adbl) 1.5d0 2.25d0 *pb-n* 0.0d0))
(finish-output *isiki-test-stream*)

(defun pb-force-gc ()
  (let ((c (%%gc-collect-count)))
    (while (= (%%gc-collect-count) c) (cons 1 2))
    (%%gc-collect-count)))
(defun pb-seg-plain (label f a b)
  (pb-force-gc)
  (let ((g0 (%%gc-collect-count)) (t0 (get-internal-real-time)))
    (%%gate-open) (pb-plain f a b *pb-n*) (%%gate-close)
    (format *isiki-test-stream* "#seg ~A gc=~D ticks=~D~%"
            label (- (%%gc-collect-count) g0) (- (get-internal-real-time) t0))
    (finish-output *isiki-test-stream*)))
(defun pb-seg-acc (label f a b z)
  (pb-force-gc)
  (let ((g0 (%%gc-collect-count)) (t0 (get-internal-real-time)))
    (%%gate-open) (pb-acc-f f a b *pb-n* z) (%%gate-close)
    (format *isiki-test-stream* "#seg ~A gc=~D ticks=~D~%"
            label (- (%%gc-collect-count) g0) (- (get-internal-real-time) t0))
    (finish-output *isiki-test-stream*)))

(let ((g0 (%%gc-collect-count)))
  (%%gate-open) (%%gate-close)
  (format *isiki-test-stream* "#seg empty-gate gc=~D ticks=0~%" (- (%%gc-collect-count) g0))
  (finish-output *isiki-test-stream*))
;; 捨てる版(Phase 0 と同じ形)
(pb-seg-plain "plain-f-id"   (function pb-id)   1 2)
(pb-seg-plain "plain-+fix"   (function pb-afix) 3 4)
(pb-seg-plain "plain-+sgl"   (function pb-asgl) 1.5f0 2.25f0)
(pb-seg-plain "plain-*sgl"   (function pb-msgl) 1.5f0 2.0f0)
(pb-seg-plain "plain-/sgl"   (function pb-dsgl) 3.0f0 2.0f0)
(pb-seg-plain "plain-+dbl"   (function pb-adbl) 1.5d0 2.25d0)
;; 累積する版(結果が必ず使われる)
(pb-seg-acc "acc-f-id"  (function pb-id)   1 2 0)
(pb-seg-acc "acc-+fix"  (function pb-afix) 3 4 0)
(pb-seg-acc "acc-+sgl"  (function pb-asgl) 1.5f0 2.25f0 0.0f0)
(pb-seg-acc "acc-*sgl"  (function pb-msgl) 1.5f0 2.0f0  0.0f0)
(pb-seg-acc "acc-/sgl"  (function pb-dsgl) 3.0f0 2.0f0  0.0f0)
(pb-seg-acc "acc-+dbl"  (function pb-adbl) 1.5d0 2.25d0 0.0d0)

(assert-equal 13 (car (%%gate-hits)))
(isiki-test-report)
(close *isiki-test-stream*)
