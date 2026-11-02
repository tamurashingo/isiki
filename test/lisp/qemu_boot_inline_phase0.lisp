;; test/lisp/qemu_boot_inline_phase0.lisp
;;
;; 算術演算のインライン化 Phase 0: **変更前の基準値**を命令数ゲートで記録する。
;; documents/inline-arith.md
;;
;; 使い方:
;;   make test-qemu-instcount-gate MILESTONE=test/lisp/qemu_boot_inline_phase0.lisp
;;
;; 区間の並びは Makefile の補正表と 1 対 1 で対応する。**並べ替えたら表が狂う。**

(load "src/lisp/init.lisp")
(load "test/lisp/test_framework.lisp")

(format *isiki-test-stream* "#gate ~A~%" (%%gate-plugin-args))
(finish-output *isiki-test-stream*)

(defun p0-id (x y) x)                       ; primitive を一切呼ばない = 呼び出しの床

(defun p0-agen (x y) (+ x y))
(defun p0-sgen (x y) (- x y))
(defun p0-mgen (x y) (* x y))
(defun p0-dgen (x y) (/ x y))

(defun p0-afix (x y) (declare (type <fixnum> x)) (declare (type <fixnum> y)) (+ x y))
(defun p0-sfix (x y) (declare (type <fixnum> x)) (declare (type <fixnum> y)) (- x y))
(defun p0-mfix (x y) (declare (type <fixnum> x)) (declare (type <fixnum> y)) (* x y))
(defun p0-dfix (x y) (declare (type <fixnum> x)) (declare (type <fixnum> y)) (/ x y))

(defun p0-asgl (x y) (declare (type <single-float> x)) (declare (type <single-float> y)) (+ x y))
(defun p0-ssgl (x y) (declare (type <single-float> x)) (declare (type <single-float> y)) (- x y))
(defun p0-msgl (x y) (declare (type <single-float> x)) (declare (type <single-float> y)) (* x y))
(defun p0-dsgl (x y) (declare (type <single-float> x)) (declare (type <single-float> y)) (/ x y))

(defun p0-adbl (x y) (declare (type <double-float> x)) (declare (type <double-float> y)) (+ x y))
(defun p0-sdbl (x y) (declare (type <double-float> x)) (declare (type <double-float> y)) (- x y))
(defun p0-mdbl (x y) (declare (type <double-float> x)) (declare (type <double-float> y)) (* x y))
(defun p0-ddbl (x y) (declare (type <double-float> x)) (declare (type <double-float> y)) (/ x y))

(defun p0-loop (f a b n)
  (let ((i 0) (r nil))
    (while (< i n) (setq r (funcall f a b)) (setq i (+ i 1)))
    r))

(defglobal *p0-n* 200000)
(defglobal *p0-warm* 2000)

;; **ウォームアップはゲートの外で。** JIT コンパイルとコードページの確保が
;; 区間に入ると命令数に乗る
(defun p0-warm (f a b) (p0-loop f a b *p0-warm*))
(p0-warm (function p0-id)   1 2)
(p0-warm (function p0-agen) 12345 6789) (p0-warm (function p0-afix) 12345 6789)
(p0-warm (function p0-sgen) 12345 6789) (p0-warm (function p0-sfix) 12345 6789)
(p0-warm (function p0-mgen) 12345 6789) (p0-warm (function p0-mfix) 12345 6789)
(p0-warm (function p0-dgen) 1000003 7)  (p0-warm (function p0-dfix) 1000003 7)
(p0-warm (function p0-agen) 1.5f0 2.25f0) (p0-warm (function p0-asgl) 1.5f0 2.25f0)
(p0-warm (function p0-sgen) 3.5f0 1.25f0) (p0-warm (function p0-ssgl) 3.5f0 1.25f0)
(p0-warm (function p0-mgen) 1.5f0 2.25f0) (p0-warm (function p0-msgl) 1.5f0 2.25f0)
(p0-warm (function p0-dgen) 3.5f0 1.25f0) (p0-warm (function p0-dsgl) 3.5f0 1.25f0)
(p0-warm (function p0-agen) 1.5d0 2.25d0) (p0-warm (function p0-adbl) 1.5d0 2.25d0)
(p0-warm (function p0-sgen) 3.5d0 1.25d0) (p0-warm (function p0-sdbl) 3.5d0 1.25d0)
(p0-warm (function p0-mgen) 1.5d0 2.25d0) (p0-warm (function p0-mdbl) 1.5d0 2.25d0)
(p0-warm (function p0-dgen) 3.5d0 1.25d0) (p0-warm (function p0-ddbl) 3.5d0 1.25d0)

;; JIT に乗っていることを確認してから測る
(assert-equal t (%%za-compiled-p (function p0-id)))
(assert-equal t (%%za-compiled-p (function p0-afix)))
(assert-equal t (%%za-compiled-p (function p0-ddbl)))
;; **inline 宣言は現状どれも効いていない**(Phase 1 の記録)
(assert-equal nil (%%current-inline))

;; **区間の中で GC が走ると命令数にそれが乗る。** 実測で gc=1 の区間だけ
;; 端数(15 万〜210 万命令)が出た。ゲートに入る前に GC を起こして
;; 半空間を空にしておけば、1 区間ぶんの確保(最大 16MB)は収まる
(defun p0-force-gc ()
  (let ((c (%%gc-collect-count)))
    (while (= (%%gc-collect-count) c) (cons 1 2))
    (%%gc-collect-count)))

(defun p0-seg (label f a b)
  (p0-force-gc)
  (let ((g0 (%%gc-collect-count)) (t0 (get-internal-real-time)))
    (%%gate-open)
    (p0-loop f a b *p0-n*)
    (%%gate-close)
    (format *isiki-test-stream* "#seg ~A gc=~D ticks=~D~%"
            label (- (%%gc-collect-count) g0) (- (get-internal-real-time) t0))
    (finish-output *isiki-test-stream*)))

;; --- 床 ---
(let ((g0 (%%gc-collect-count)) (t0 (get-internal-real-time)))
  (%%gate-open) (%%gate-close)
  (format *isiki-test-stream* "#seg empty-gate gc=~D ticks=~D~%"
          (- (%%gc-collect-count) g0) (- (get-internal-real-time) t0))
  (finish-output *isiki-test-stream*))
(p0-seg "f-id" (function p0-id) 1 2)

;; --- fixnum ---
(p0-seg "+fix-nodecl" (function p0-agen) 12345 6789)
(p0-seg "+fix-decl"   (function p0-afix) 12345 6789)
(p0-seg "-fix-nodecl" (function p0-sgen) 12345 6789)
(p0-seg "-fix-decl"   (function p0-sfix) 12345 6789)
(p0-seg "*fix-nodecl" (function p0-mgen) 12345 6789)
(p0-seg "*fix-decl"   (function p0-mfix) 12345 6789)
(p0-seg "/fix-nodecl" (function p0-dgen) 1000003 7)
(p0-seg "/fix-decl"   (function p0-dfix) 1000003 7)

;; --- single-float ---
(p0-seg "+sgl-nodecl" (function p0-agen) 1.5f0 2.25f0)
(p0-seg "+sgl-decl"   (function p0-asgl) 1.5f0 2.25f0)
(p0-seg "-sgl-nodecl" (function p0-sgen) 3.5f0 1.25f0)
(p0-seg "-sgl-decl"   (function p0-ssgl) 3.5f0 1.25f0)
(p0-seg "*sgl-nodecl" (function p0-mgen) 1.5f0 2.25f0)
(p0-seg "*sgl-decl"   (function p0-msgl) 1.5f0 2.25f0)
(p0-seg "/sgl-nodecl" (function p0-dgen) 3.5f0 1.25f0)
(p0-seg "/sgl-decl"   (function p0-dsgl) 3.5f0 1.25f0)

;; --- double-float(確保が伴うので gc を必ず見ること)---
(p0-seg "+dbl-nodecl" (function p0-agen) 1.5d0 2.25d0)
(p0-seg "+dbl-decl"   (function p0-adbl) 1.5d0 2.25d0)
(p0-seg "-dbl-nodecl" (function p0-sgen) 3.5d0 1.25d0)
(p0-seg "-dbl-decl"   (function p0-sdbl) 3.5d0 1.25d0)
(p0-seg "*dbl-nodecl" (function p0-mgen) 1.5d0 2.25d0)
(p0-seg "*dbl-decl"   (function p0-mdbl) 1.5d0 2.25d0)
(p0-seg "/dbl-nodecl" (function p0-dgen) 3.5d0 1.25d0)
(p0-seg "/dbl-decl"   (function p0-ddbl) 3.5d0 1.25d0)

;; --- 確保量(byte/call)。ゲートとは独立に測る ---
(defglobal *p0-bn* 10000)
(defun p0-bytes (label f a b)
  (let ((h0 (%%heap-used-bytes)) (g0 (%%gc-collect-count)))
    (p0-loop f a b *p0-bn*)
    (format *isiki-test-stream* "#bytes ~A per-call=~D gc=~D~%"
            label (div (- (%%heap-used-bytes) h0) *p0-bn*) (- (%%gc-collect-count) g0))
    (finish-output *isiki-test-stream*)))
(p0-bytes "f-id       " (function p0-id)   1 2)
(p0-bytes "+fix-nodecl" (function p0-agen) 12345 6789)
(p0-bytes "+fix-decl  " (function p0-afix) 12345 6789)
(p0-bytes "/fix-nodecl" (function p0-dgen) 1000003 7)
(p0-bytes "/fix-decl  " (function p0-dfix) 1000003 7)
(p0-bytes "+sgl-decl  " (function p0-asgl) 1.5f0 2.25f0)
(p0-bytes "/sgl-decl  " (function p0-dsgl) 3.5f0 1.25f0)
(p0-bytes "+dbl-decl  " (function p0-adbl) 1.5d0 2.25d0)

;; --- code-len(展開が起きているかの静的な指標)---
(format *isiki-test-stream* "#codelen + ~D / ~D   - ~D / ~D   * ~D / ~D   / ~D / ~D  (nodecl/fix-decl)~%"
        (%%disasm-code-len 'p0-agen) (%%disasm-code-len 'p0-afix)
        (%%disasm-code-len 'p0-sgen) (%%disasm-code-len 'p0-sfix)
        (%%disasm-code-len 'p0-mgen) (%%disasm-code-len 'p0-mfix)
        (%%disasm-code-len 'p0-dgen) (%%disasm-code-len 'p0-dfix))
(finish-output *isiki-test-stream*)

(assert-equal 26 (car (%%gate-hits)))
(assert-equal 26 (cdr (%%gate-hits)))
(isiki-test-report)
(close *isiki-test-stream*)
