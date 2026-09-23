;; test/lisp/qemu_boot_inline_phase3.lisp
;;
;; Phase 3: `+` `-` fixnum の展開の効果。documents/inline-arith.md §9
;; 期待値は実装前に PR #94 へ宣言済み(規則 11)。
;;
;;   +fix-decl + (declare (inline +))  62.9 -> 21.9  (-41.0)
;;   -fix-decl + (declare (inline -))  75.9 -> 22.9  (-53.0)
;;   inline 宣言なしの経路はすべて ±0(門番は加算的)
;;
;; make test-qemu-instcount-gate MILESTONE=test/lisp/qemu_boot_inline_phase3.lisp

(load "src/lisp/init.lisp")
(load "test/lisp/test_framework.lisp")
(format *isiki-test-stream* "#gate ~A~%" (%%gate-plugin-args))
(finish-output *isiki-test-stream*)

(defun p3-id (x y) x)
(defun p3-agen (x y) (+ x y))
(defun p3-sgen (x y) (- x y))
(defun p3-afix (x y) (declare (type <fixnum> x)) (declare (type <fixnum> y)) (+ x y))
(defun p3-sfix (x y) (declare (type <fixnum> x)) (declare (type <fixnum> y)) (- x y))
(defun p3-ainl (x y)
  (declare (type <fixnum> x)) (declare (type <fixnum> y)) (declare (inline +)) (+ x y))
(defun p3-sinl (x y)
  (declare (type <fixnum> x)) (declare (type <fixnum> y)) (declare (inline -)) (- x y))
;; 対照: 展開の対象外。Phase 0 から動いてはいけない
(defun p3-mfix (x y) (declare (type <fixnum> x)) (declare (type <fixnum> y)) (* x y))
(defun p3-dfix (x y) (declare (type <fixnum> x)) (declare (type <fixnum> y)) (/ x y))
(defun p3-asgl (x y) (declare (type <single-float> x)) (declare (type <single-float> y)) (+ x y))
(defun p3-adbl (x y) (declare (type <double-float> x)) (declare (type <double-float> y)) (+ x y))

(defun p3-loop (f a b n)
  (let ((i 0) (r nil)) (while (< i n) (setq r (funcall f a b)) (setq i (+ i 1))) r))
(defglobal *p3-n* 200000)
(defun p3-warm (f a b) (p3-loop f a b 2000))
(p3-warm (function p3-id) 1 2)
(p3-warm (function p3-agen) 12345 6789) (p3-warm (function p3-afix) 12345 6789)
(p3-warm (function p3-ainl) 12345 6789)
(p3-warm (function p3-sgen) 12345 6789) (p3-warm (function p3-sfix) 12345 6789)
(p3-warm (function p3-sinl) 12345 6789)
(p3-warm (function p3-mfix) 12345 6789) (p3-warm (function p3-dfix) 1000003 7)
(p3-warm (function p3-asgl) 1.5f0 2.25f0) (p3-warm (function p3-adbl) 1.5d0 2.25d0)

;; **切り替わったことを別の計器で確認してから測る**(完全一致が予測されるため)
(format *isiki-test-stream* "#codelen agen=~D afix=~D ainl=~D | sgen=~D sfix=~D sinl=~D~%"
        (%%disasm-code-len 'p3-agen) (%%disasm-code-len 'p3-afix) (%%disasm-code-len 'p3-ainl)
        (%%disasm-code-len 'p3-sgen) (%%disasm-code-len 'p3-sfix) (%%disasm-code-len 'p3-sinl))
(format *isiki-test-stream* "#inline-of ainl=~S sinl=~S afix=~S~%"
        (%%inline-of 'p3-ainl) (%%inline-of 'p3-sinl) (%%inline-of 'p3-afix))
(finish-output *isiki-test-stream*)
(assert-equal 735 (%%disasm-code-len 'p3-ainl))
(assert-equal 738 (%%disasm-code-len 'p3-sinl))
(assert-equal 690 (%%disasm-code-len 'p3-afix))
(assert-equal 19134 (p3-ainl 12345 6789))
(assert-equal 5556 (p3-sinl 12345 6789))

(defun p3-force-gc ()
  (let ((c (%%gc-collect-count)))
    (while (= (%%gc-collect-count) c) (cons 1 2))
    (%%gc-collect-count)))
(defun p3-seg (label f a b)
  (p3-force-gc)
  (let ((g0 (%%gc-collect-count)) (t0 (get-internal-real-time)))
    (%%gate-open) (p3-loop f a b *p3-n*) (%%gate-close)
    (format *isiki-test-stream* "#seg ~A gc=~D ticks=~D~%"
            label (- (%%gc-collect-count) g0) (- (get-internal-real-time) t0))
    (finish-output *isiki-test-stream*)))

(let ((g0 (%%gc-collect-count)))
  (%%gate-open) (%%gate-close)
  (format *isiki-test-stream* "#seg empty-gate gc=~D ticks=0~%" (- (%%gc-collect-count) g0))
  (finish-output *isiki-test-stream*))
(p3-seg "f-id"          (function p3-id)   1 2)
(p3-seg "+fix-nodecl"   (function p3-agen) 12345 6789)
(p3-seg "+fix-decl"     (function p3-afix) 12345 6789)
(p3-seg "+fix-inline"   (function p3-ainl) 12345 6789)
(p3-seg "-fix-nodecl"   (function p3-sgen) 12345 6789)
(p3-seg "-fix-decl"     (function p3-sfix) 12345 6789)
(p3-seg "-fix-inline"   (function p3-sinl) 12345 6789)
(p3-seg "*fix-decl"     (function p3-mfix) 12345 6789)
(p3-seg "/fix-decl"     (function p3-dfix) 1000003 7)
(p3-seg "+sgl-decl"     (function p3-asgl) 1.5f0 2.25f0)
(p3-seg "+dbl-decl"     (function p3-adbl) 1.5d0 2.25d0)

(defglobal *p3-bn* 10000)
(defun p3-bytes (label f a b)
  (let ((h0 (%%heap-used-bytes)) (g0 (%%gc-collect-count)))
    (p3-loop f a b *p3-bn*)
    (format *isiki-test-stream* "#bytes ~A per-call=~D gc=~D~%"
            label (div (- (%%heap-used-bytes) h0) *p3-bn*) (- (%%gc-collect-count) g0))
    (finish-output *isiki-test-stream*)))
(p3-bytes "f-id       " (function p3-id)   1 2)
(p3-bytes "+fix-decl  " (function p3-afix) 12345 6789)
(p3-bytes "+fix-inline" (function p3-ainl) 12345 6789)
(p3-bytes "-fix-inline" (function p3-sinl) 12345 6789)

(assert-equal 12 (car (%%gate-hits)))
(isiki-test-report)
(close *isiki-test-stream*)
