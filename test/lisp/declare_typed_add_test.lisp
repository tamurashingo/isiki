;; test/lisp/declare_typed_add_test.lisp
;;
;; declare の型を使った + の特化(documents/declare-typed-add.md)。
;;
;; **最重要は「宣言の有無で結果が変わらないこと」**である。型特化は最適化であり、
;; 意味論を変えない。宣言ありと宣言なしを同じ入力で突き合わせる。

(defun dta-pr (x) (let ((s (create-string-output-stream))) (format s "~S" x) (get-output-stream-string s)))

(defun add-generic (x y) (+ x y))
(defun add-fixnum (x y)
  (declare (type <fixnum> x)) (declare (type <fixnum> y)) (+ x y))
(defun add-single (x y)
  (declare (type <single-float> x)) (declare (type <single-float> y)) (+ x y))
(defun add-double (x y)
  (declare (type <double-float> x)) (declare (type <double-float> y)) (+ x y))
(defun add-mixed (x y)
  (declare (type <fixnum> x)) (declare (type <single-float> y)) (+ x y))
(defun add3-fixnum (x y z)
  (declare (type <fixnum> x)) (declare (type <fixnum> y)) (declare (type <fixnum> z))
  (+ x y z))

(assert-equal t (%%za-compiled-p (function add-generic)))
(assert-equal t (%%za-compiled-p (function add-fixnum)))
(assert-equal t (%%za-compiled-p (function add-single)))
(assert-equal t (%%za-compiled-p (function add3-fixnum)))

;;; --- 5-1. 宣言の有無で結果が変わらないこと ---
;; fixnum: 小さい値、2^24 前後、2^59 前後、負数
(assert-equal (add-generic 1 2) (add-fixnum 1 2))
(assert-equal (add-generic 16777216 1) (add-fixnum 16777216 1))
(assert-equal (add-generic 16777217 1) (add-fixnum 16777217 1))
(assert-equal (add-generic -7 3) (add-fixnum -7 3))
(assert-equal (add-generic 7 -3) (add-fixnum 7 -3))
(assert-equal (add-generic -7 -3) (add-fixnum -7 -3))
(assert-equal 3 (add-fixnum 1 2))
(assert-equal -10 (add-fixnum -7 -3))
;; **fixnum の桁溢れ → bignum へ昇格。GENERIC と一致すること**
(defglobal *maxfix* *most-positive-fixnum*)
(assert-equal (add-generic *maxfix* 1) (add-fixnum *maxfix* 1))
(assert-equal t (bignump (add-fixnum *maxfix* 1)))
(assert-equal (add-generic *maxfix* *maxfix*) (add-fixnum *maxfix* *maxfix*))

;; single: 0.0 / -0.0 / 非正規化数 / 無限大 / NaN
(defglobal *inf-s* (/ 1.0f0 0.0f0))
(defglobal *nan-s* (/ 0.0f0 0.0f0))
(defglobal *den-s* 1.0f-40)
(assert-equal (dta-pr (add-generic 1.5f0 2.25f0)) (dta-pr (add-single 1.5f0 2.25f0)))
(assert-equal (dta-pr (add-generic 0.0f0 -0.0f0)) (dta-pr (add-single 0.0f0 -0.0f0)))
(assert-equal (dta-pr (add-generic -0.0f0 -0.0f0)) (dta-pr (add-single -0.0f0 -0.0f0)))
(assert-equal (dta-pr (add-generic *den-s* *den-s*)) (dta-pr (add-single *den-s* *den-s*)))
(assert-equal (dta-pr (add-generic *inf-s* 1.0f0)) (dta-pr (add-single *inf-s* 1.0f0)))
(assert-equal (dta-pr (add-generic *nan-s* 1.0f0)) (dta-pr (add-single *nan-s* 1.0f0)))
(assert-equal '<single-float> (%%class-name (class-of (add-single 1.0f0 2.0f0))))

;; double は GENERIC のまま(対照)
(assert-equal (dta-pr (add-generic 1.5d0 2.25d0)) (dta-pr (add-double 1.5d0 2.25d0)))
(assert-equal '<double-float> (%%class-name (class-of (add-double 1.0d0 2.0d0))))

;; **混在は GENERIC へ落ちる**(本作業では特化しない)
(assert-equal (dta-pr (add-generic 1000 1.0f0)) (dta-pr (add-mixed 1000 1.0f0)))
(assert-equal (dta-pr (add-generic 16777217 1.0f0)) (dta-pr (add-mixed 16777217 1.0f0)))
(assert-equal '<single-float> (%%class-name (class-of (add-mixed 1000 1.0f0))))

;; **引数 3 つは型が分かっていても GENERIC**
(assert-equal (+ 1 2 3) (add3-fixnum 1 2 3))
(assert-equal 6 (add3-fixnum 1 2 3))
(assert-equal (+ *maxfix* 1 1) (add3-fixnum *maxfix* 1 1))

;;; --- let 束縛変数の宣言でも効くこと ---
(defun add-let (n)
  (let ((a 10) (b 20))
    (declare (type <fixnum> a)) (declare (type <fixnum> b))
    (+ a b)))
(assert-equal t (%%za-compiled-p (function add-let)))
(assert-equal 30 (add-let 0))

;;; --- 片方が式なら GENERIC(§3-4: 型の出所は declare のみ)---
(defun add-expr (x y)
  (declare (type <fixnum> x)) (declare (type <fixnum> y))
  (+ x (+ y 1)))
(assert-equal t (%%za-compiled-p (function add-expr)))
(assert-equal 4 (add-expr 1 2))
(assert-equal (+ 1 (+ 2 1)) (add-expr 1 2))
