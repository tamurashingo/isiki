;; test/lisp/declare_typed_sub_test.lisp
;;
;; declare の型を使った - の特化(documents/declare-typed-arith.md)。
;;
;; **最重要は「宣言の有無で結果が変わらないこと」**である。型特化は最適化であり、
;; 意味論を変えない。宣言ありと宣言なしを同じ入力で突き合わせる。
;;
;; `-` は非可換なので、`+` のテストに無い観点として**オペランドの順序**を見る
;; ((- 7 3) と (- 3 7) の両方)。

(defun dts-pr (x) (let ((s (create-string-output-stream))) (format s "~S" x) (get-output-stream-string s)))

;; 注釈にシンボル名が出ることを見る(PR #90)。名前 + オフセットは出ないので完全一致でよい
(defun dts-has-callee (name callee)
  (let ((items (disasm-items (%%disasm-code-base name) (%%disasm-code-len name)))
        (found nil))
    (while (not (null items))
      (if (string= callee (disasm-item-comment (cdr (car items))))
          (setq found t)
        nil)
      (setq items (cdr items)))
    found))

(defun sub-generic (x y) (- x y))
(defun sub-fixnum (x y)
  (declare (type <fixnum> x)) (declare (type <fixnum> y)) (- x y))
(defun sub-single (x y)
  (declare (type <single-float> x)) (declare (type <single-float> y)) (- x y))
(defun sub-double (x y)
  (declare (type <double-float> x)) (declare (type <double-float> y)) (- x y))
(defun sub-mixed (x y)
  (declare (type <fixnum> x)) (declare (type <single-float> y)) (- x y))
(defun sub3-fixnum (x y z)
  (declare (type <fixnum> x)) (declare (type <fixnum> y)) (declare (type <fixnum> z))
  (- x y z))
;; 単項の - は本作業の対象外(za_compile_fold は引数 2 つ未満では呼ばれない)
(defun neg-fixnum (x) (declare (type <fixnum> x)) (- x))

(assert-equal t (%%za-compiled-p (function sub-generic)))
(assert-equal t (%%za-compiled-p (function sub-fixnum)))
(assert-equal t (%%za-compiled-p (function sub-single)))
(assert-equal t (%%za-compiled-p (function sub3-fixnum)))
(assert-equal t (%%za-compiled-p (function neg-fixnum)))

;;; --- 5-2. 振り分けが正しいこと(シンボル名で直接確認する)---
(assert-equal t (dts-has-callee 'sub-generic "primitive_subtract2"))
(assert-equal t (dts-has-callee 'sub-fixnum "primitive_subtract2_fixnum"))
(assert-equal t (dts-has-callee 'sub-single "primitive_subtract2_single"))
;; 取り違えていないこと(fixnum 版が single 版を呼んでいない、など)
(assert-equal nil (dts-has-callee 'sub-fixnum "primitive_subtract2_single"))
(assert-equal nil (dts-has-callee 'sub-single "primitive_subtract2_fixnum"))
;; + の特化を呼んでしまっていないこと
(assert-equal nil (dts-has-callee 'sub-fixnum "primitive_add2_fixnum"))
;; double / 混在 / 3 引数 / 単項は GENERIC のまま(対照群)
(assert-equal t (dts-has-callee 'sub-double "primitive_subtract2"))
(assert-equal t (dts-has-callee 'sub-mixed "primitive_subtract2"))
(assert-equal t (dts-has-callee 'sub3-fixnum "primitive_subtract2"))
(assert-equal nil (dts-has-callee 'sub3-fixnum "primitive_subtract2_fixnum"))
(assert-equal nil (dts-has-callee 'neg-fixnum "primitive_subtract2_fixnum"))

;;; --- 5-1. 宣言の有無で結果が変わらないこと ---
;; fixnum: 小さい値、2^24 前後、負数、**順序の入れ替え**
(assert-equal (sub-generic 7 3) (sub-fixnum 7 3))
(assert-equal (sub-generic 3 7) (sub-fixnum 3 7))
(assert-equal (sub-generic 16777216 1) (sub-fixnum 16777216 1))
(assert-equal (sub-generic 16777217 1) (sub-fixnum 16777217 1))
(assert-equal (sub-generic -7 3) (sub-fixnum -7 3))
(assert-equal (sub-generic 7 -3) (sub-fixnum 7 -3))
(assert-equal (sub-generic -7 -3) (sub-fixnum -7 -3))
(assert-equal (sub-generic -3 -7) (sub-fixnum -3 -7))
(assert-equal 4 (sub-fixnum 7 3))
(assert-equal -4 (sub-fixnum 3 7))
(assert-equal 10 (sub-fixnum 7 -3))
(assert-equal -4 (sub-fixnum -7 -3))
;; 0 になる場合(-0 を作っていないこと。符号マグニチュード表現の罠)
(assert-equal 0 (sub-fixnum 5 5))
(assert-equal 0 (sub-fixnum -5 -5))
(assert-equal (dts-pr (sub-generic 5 5)) (dts-pr (sub-fixnum 5 5)))
(assert-equal (dts-pr (sub-generic -5 -5)) (dts-pr (sub-fixnum -5 -5)))

;; **fixnum の桁溢れ → bignum へ昇格。GENERIC と一致すること**
(defglobal *dts-maxfix* *most-positive-fixnum*)
(defglobal *dts-minfix* *most-negative-fixnum*)
(assert-equal (sub-generic *dts-maxfix* -1) (sub-fixnum *dts-maxfix* -1))
(assert-equal t (bignump (sub-fixnum *dts-maxfix* -1)))
(assert-equal (sub-generic *dts-minfix* 1) (sub-fixnum *dts-minfix* 1))
(assert-equal t (bignump (sub-fixnum *dts-minfix* 1)))
(assert-equal (sub-generic *dts-maxfix* *dts-minfix*) (sub-fixnum *dts-maxfix* *dts-minfix*))
;; 桁溢れしない境界(ちょうど収まる)
(assert-equal (sub-generic *dts-maxfix* 0) (sub-fixnum *dts-maxfix* 0))
(assert-equal t (null (bignump (sub-fixnum *dts-maxfix* 0))))
(assert-equal (sub-generic 0 *dts-maxfix*) (sub-fixnum 0 *dts-maxfix*))
(assert-equal t (null (bignump (sub-fixnum 0 *dts-maxfix*))))

;; single: 0.0 / -0.0 / 非正規化数 / 無限大 / NaN
(defglobal *dts-inf-s* (/ 1.0f0 0.0f0))
(defglobal *dts-nan-s* (/ 0.0f0 0.0f0))
(defglobal *dts-den-s* 1.0f-40)
(assert-equal (dts-pr (sub-generic 2.25f0 1.5f0)) (dts-pr (sub-single 2.25f0 1.5f0)))
(assert-equal (dts-pr (sub-generic 1.5f0 2.25f0)) (dts-pr (sub-single 1.5f0 2.25f0)))
;; **0.0 - 0.0 は +0.0、0.0 - -0.0 も +0.0、-0.0 - 0.0 は -0.0**(IEEE 754)
(assert-equal (dts-pr (sub-generic 0.0f0 0.0f0)) (dts-pr (sub-single 0.0f0 0.0f0)))
(assert-equal (dts-pr (sub-generic 0.0f0 -0.0f0)) (dts-pr (sub-single 0.0f0 -0.0f0)))
(assert-equal (dts-pr (sub-generic -0.0f0 0.0f0)) (dts-pr (sub-single -0.0f0 0.0f0)))
(assert-equal (dts-pr (sub-generic -0.0f0 -0.0f0)) (dts-pr (sub-single -0.0f0 -0.0f0)))
(assert-equal (dts-pr (sub-generic *dts-den-s* *dts-den-s*)) (dts-pr (sub-single *dts-den-s* *dts-den-s*)))
(assert-equal (dts-pr (sub-generic *dts-inf-s* 1.0f0)) (dts-pr (sub-single *dts-inf-s* 1.0f0)))
;; inf - inf は NaN
(assert-equal (dts-pr (sub-generic *dts-inf-s* *dts-inf-s*)) (dts-pr (sub-single *dts-inf-s* *dts-inf-s*)))
(assert-equal (dts-pr (sub-generic *dts-nan-s* 1.0f0)) (dts-pr (sub-single *dts-nan-s* 1.0f0)))
(assert-equal (dts-pr (sub-generic 1.0f0 *dts-nan-s*)) (dts-pr (sub-single 1.0f0 *dts-nan-s*)))
(assert-equal '<single-float> (%%class-name (class-of (sub-single 2.0f0 1.0f0))))

;; double は GENERIC のまま(対照)
(assert-equal (dts-pr (sub-generic 2.25d0 1.5d0)) (dts-pr (sub-double 2.25d0 1.5d0)))
(assert-equal '<double-float> (%%class-name (class-of (sub-double 2.0d0 1.0d0))))

;; **混在は GENERIC へ落ちる**(本作業では特化しない)
(assert-equal (dts-pr (sub-generic 1000 1.0f0)) (dts-pr (sub-mixed 1000 1.0f0)))
(assert-equal (dts-pr (sub-generic 16777217 1.0f0)) (dts-pr (sub-mixed 16777217 1.0f0)))
(assert-equal '<single-float> (%%class-name (class-of (sub-mixed 1000 1.0f0))))

;; **引数 3 つは型が分かっていても GENERIC**
(assert-equal (- 10 2 3) (sub3-fixnum 10 2 3))
(assert-equal 5 (sub3-fixnum 10 2 3))

;; 単項の - は従来どおり(対象外)
(assert-equal -5 (neg-fixnum 5))
(assert-equal 5 (neg-fixnum -5))
(assert-equal 0 (neg-fixnum 0))

;;; --- let 束縛変数の宣言でも効くこと ---
(defun sub-let (n)
  (let ((a 30) (b 20))
    (declare (type <fixnum> a)) (declare (type <fixnum> b))
    (- a b)))
(assert-equal t (%%za-compiled-p (function sub-let)))
(assert-equal 10 (sub-let 0))
(assert-equal t (dts-has-callee 'sub-let "primitive_subtract2_fixnum"))

;;; --- 片方が式なら GENERIC(§3-4: 型の出所は declare のみ)---
(defun sub-expr (x y)
  (declare (type <fixnum> x)) (declare (type <fixnum> y))
  (- x (- y 1)))
(assert-equal t (%%za-compiled-p (function sub-expr)))
(assert-equal 2 (sub-expr 3 2))
(assert-equal (- 3 (- 2 1)) (sub-expr 3 2))
