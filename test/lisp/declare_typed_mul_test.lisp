;; test/lisp/declare_typed_mul_test.lisp
;;
;; declare の型を使った * の特化と、GENERIC 側の高速路の改善(改善B)。
;; documents/declare-typed-arith.md
;;
;; この PR は 2 つの変更を含む。
;;   1. GENERIC の高速路を 128bit の積 + 符号対応に直した(宣言なしにも効く)
;;   2. 宣言による特化(宣言した人に効く)
;; **どちらも「結果が変わらないこと」が最優先**である。1 は負数と桁溢れの境界、
;; 2 は宣言あり/なしの突き合わせで見る。

(defun dtm-pr (x) (let ((s (create-string-output-stream))) (format s "~S" x) (get-output-stream-string s)))

(defun dtm-has-callee (name callee)
  (let ((items (disasm-items (%%disasm-code-base name) (%%disasm-code-len name)))
        (found nil))
    (while (not (null items))
      (if (string= callee (disasm-item-comment (cdr (car items))))
          (setq found t)
        nil)
      (setq items (cdr items)))
    found))

(defun mul-generic (x y) (* x y))
(defun mul-fixnum (x y)
  (declare (type <fixnum> x)) (declare (type <fixnum> y)) (* x y))
(defun mul-single (x y)
  (declare (type <single-float> x)) (declare (type <single-float> y)) (* x y))
(defun mul-double (x y)
  (declare (type <double-float> x)) (declare (type <double-float> y)) (* x y))
(defun mul-integer (x y)
  (declare (type <integer> x)) (declare (type <integer> y)) (* x y))
(defun mul-mixed (x y)
  (declare (type <fixnum> x)) (declare (type <single-float> y)) (* x y))
(defun mul3-fixnum (x y z)
  (declare (type <fixnum> x)) (declare (type <fixnum> y)) (declare (type <fixnum> z))
  (* x y z))

(assert-equal t (%%za-compiled-p (function mul-generic)))
(assert-equal t (%%za-compiled-p (function mul-fixnum)))
(assert-equal t (%%za-compiled-p (function mul-single)))
(assert-equal t (%%za-compiled-p (function mul3-fixnum)))

;;; --- 振り分け(シンボル名で直接確認する)---
(assert-equal t (dtm-has-callee 'mul-generic "primitive_multiply2"))
(assert-equal t (dtm-has-callee 'mul-fixnum "primitive_multiply2_fixnum"))
(assert-equal t (dtm-has-callee 'mul-single "primitive_multiply2_single"))
;; 取り違えていないこと
(assert-equal nil (dtm-has-callee 'mul-fixnum "primitive_multiply2_single"))
(assert-equal nil (dtm-has-callee 'mul-single "primitive_multiply2_fixnum"))
(assert-equal nil (dtm-has-callee 'mul-fixnum "primitive_add2_fixnum"))
(assert-equal nil (dtm-has-callee 'mul-fixnum "primitive_subtract2_fixnum"))
;; 対照群は GENERIC のまま。**<integer> は特化表に載らないので GENERIC、かつ嘘でない**
(assert-equal t (dtm-has-callee 'mul-double "primitive_multiply2"))
(assert-equal t (dtm-has-callee 'mul-integer "primitive_multiply2"))
(assert-equal t (dtm-has-callee 'mul-mixed "primitive_multiply2"))
(assert-equal t (dtm-has-callee 'mul3-fixnum "primitive_multiply2"))
(assert-equal nil (dtm-has-callee 'mul3-fixnum "primitive_multiply2_fixnum"))

;;; --- 5-2. 負数の全組み合わせ(符号が neg_a XOR neg_b で決まること)---
(assert-equal 12 (mul-fixnum 3 4))
(assert-equal -12 (mul-fixnum 3 -4))
(assert-equal -12 (mul-fixnum -3 4))
(assert-equal 12 (mul-fixnum -3 -4))
(assert-equal (mul-generic 3 -4) (mul-fixnum 3 -4))
(assert-equal (mul-generic -3 4) (mul-fixnum -3 4))
(assert-equal (mul-generic -3 -4) (mul-fixnum -3 -4))
;; **-0 を作らないこと**(fixnum は符号マグニチュード表現)
(assert-equal 0 (mul-fixnum 0 -1))
(assert-equal 0 (mul-fixnum -1 0))
(assert-equal 0 (mul-fixnum -0 -1))
(assert-equal (dtm-pr (mul-generic 0 -1)) (dtm-pr (mul-fixnum 0 -1)))
(assert-equal (dtm-pr (mul-generic -1 0)) (dtm-pr (mul-fixnum -1 0)))
;; 0 の符号が後段へ漏れないこと(負のゼロがあれば割ると -inf 側になる)
(assert-equal 1 (quotient 1 (+ (mul-fixnum 0 -1) 1)))

;;; --- 5-1 / 5-3. 桁溢れの境界 ---
;; **境界は定数から導く。直書きしない**(タグ幅が変われば追随する)。
;; いまの *most-positive-fixnum* は 2^59-1 だが、テストはその値に依存しない
(defglobal *dtm-maxfix* *most-positive-fixnum*)
;; (maxfix-1)/2。2 倍すると maxfix-1 でちょうど収まり、+1 してから 2 倍すると
;; maxfix+1 で**ちょうど 1 だけ溢れる**
(defglobal *dtm-half* (quotient (- *dtm-maxfix* 1) 2))
(defglobal *dtm-half+* (+ *dtm-half* 1))
;; 平方根の床。2 乗は収まり、+1 の 2 乗は溢れる
(defglobal *dtm-root* 759250124)
(defglobal *dtm-root+* 759250125)

;; 2 * (maxfix-1)/2 = maxfix-1。**収まる**
(assert-equal (- *dtm-maxfix* 1) (mul-fixnum 2 *dtm-half*))
(assert-equal (mul-generic 2 *dtm-half*) (mul-fixnum 2 *dtm-half*))
(assert-equal nil (bignump (mul-fixnum 2 *dtm-half*)))
(assert-equal nil (bignump (mul-fixnum *dtm-half* 2)))   ; 順序を入れ替えても同じ
;; 2 * ((maxfix-1)/2 + 1) = maxfix+1。**ちょうど 1 だけ超えて bignum へ**
(assert-equal (mul-generic 2 *dtm-half+*) (mul-fixnum 2 *dtm-half+*))
(assert-equal t (bignump (mul-fixnum 2 *dtm-half+*)))
(assert-equal (+ *dtm-maxfix* 1) (mul-fixnum 2 *dtm-half+*))
;; 平方根の床の 2 乗は収まり、その次の 2 乗は溢れる
(assert-equal nil (bignump (mul-fixnum *dtm-root* *dtm-root*)))
(assert-equal (mul-generic *dtm-root* *dtm-root*) (mul-fixnum *dtm-root* *dtm-root*))
(assert-equal t (bignump (mul-fixnum *dtm-root+* *dtm-root+*)))
(assert-equal (mul-generic *dtm-root+* *dtm-root+*) (mul-fixnum *dtm-root+* *dtm-root+*))
;; 根の選び方自体の確認(root^2 <= maxfix < (root+1)^2 であること)
(assert-equal t (<= (mul-fixnum *dtm-root* *dtm-root*) *dtm-maxfix*))
(assert-equal t (> (mul-fixnum *dtm-root+* *dtm-root+*) *dtm-maxfix*))
;; maxfix * 1 は収まり、maxfix * 2 は溢れる
(assert-equal *dtm-maxfix* (mul-fixnum *dtm-maxfix* 1))
(assert-equal nil (bignump (mul-fixnum *dtm-maxfix* 1)))
(assert-equal (mul-generic *dtm-maxfix* 2) (mul-fixnum *dtm-maxfix* 2))
(assert-equal t (bignump (mul-fixnum *dtm-maxfix* 2)))
;; 0 を掛けると、相手がどれだけ大きくても溢れない
(assert-equal 0 (mul-fixnum *dtm-maxfix* 0))
(assert-equal 0 (mul-fixnum 0 *dtm-maxfix*))

;;; --- 5-3. 負数での桁溢れ(符号とマグニチュードの両方)---
(defglobal *dtm-nroot+* (- 0 *dtm-root+*))
;; 負 × 負 = **正**の bignum
(assert-equal (mul-generic *dtm-nroot+* *dtm-nroot+*) (mul-fixnum *dtm-nroot+* *dtm-nroot+*))
(assert-equal t (bignump (mul-fixnum *dtm-nroot+* *dtm-nroot+*)))
(assert-equal t (> (mul-fixnum *dtm-nroot+* *dtm-nroot+*) 0))
;; 負 × 正 = **負**の bignum
(assert-equal (mul-generic *dtm-nroot+* *dtm-root+*) (mul-fixnum *dtm-nroot+* *dtm-root+*))
(assert-equal t (bignump (mul-fixnum *dtm-nroot+* *dtm-root+*)))
(assert-equal t (< (mul-fixnum *dtm-nroot+* *dtm-root+*) 0))
;; 負の境界ちょうど(収まる側)。-(maxfix-1) が bignum にならないこと
(assert-equal (- 0 (- *dtm-maxfix* 1)) (mul-fixnum -2 *dtm-half*))
(assert-equal nil (bignump (mul-fixnum -2 *dtm-half*)))
(assert-equal (- *dtm-maxfix* 1) (mul-fixnum -2 (- 0 *dtm-half*)))
(assert-equal nil (bignump (mul-fixnum -2 (- 0 *dtm-half*))))

;;; --- 改善B: GENERIC(宣言なし)でも負数が正しいこと ---
;; **以前は負数が 1 つでも絡むと bignum 機構を通っていた。** 答えは同じだが経路が変わる
(assert-equal -12 (mul-generic -3 4))
(assert-equal 12 (mul-generic -3 -4))
(assert-equal 0 (mul-generic -5 0))
(assert-equal (dtm-pr 0) (dtm-pr (mul-generic -5 0)))
;; n 項版も同じコアを使う
(assert-equal -24 (* -2 3 4))
(assert-equal 24 (* -2 3 -4))
(assert-equal 24 (* -2 -3 4 1))
(assert-equal 0 (* -2 0 4))
(assert-equal 1 (*))
(assert-equal -5 (* -5))
;; n 項でも桁溢れは bignum へ
(assert-equal t (bignump (* *dtm-root+* *dtm-root+* 2)))
(assert-equal (* *dtm-root+* *dtm-root+* 2) (* 2 *dtm-root+* *dtm-root+*))
(assert-equal (* -1 *dtm-root+* *dtm-root+*) (- 0 (* *dtm-root+* *dtm-root+*)))

;;; --- single ---
(defglobal *dtm-inf-s* (/ 1.0f0 0.0f0))
(defglobal *dtm-nan-s* (/ 0.0f0 0.0f0))
(defglobal *dtm-den-s* 1.0f-40)
(assert-equal (dtm-pr (mul-generic 1.5f0 2.25f0)) (dtm-pr (mul-single 1.5f0 2.25f0)))
(assert-equal (dtm-pr (mul-generic -1.5f0 2.25f0)) (dtm-pr (mul-single -1.5f0 2.25f0)))
(assert-equal (dtm-pr (mul-generic 0.0f0 -1.0f0)) (dtm-pr (mul-single 0.0f0 -1.0f0)))
(assert-equal (dtm-pr (mul-generic -0.0f0 -1.0f0)) (dtm-pr (mul-single -0.0f0 -1.0f0)))
(assert-equal (dtm-pr (mul-generic *dtm-den-s* *dtm-den-s*)) (dtm-pr (mul-single *dtm-den-s* *dtm-den-s*)))
(assert-equal (dtm-pr (mul-generic *dtm-inf-s* 2.0f0)) (dtm-pr (mul-single *dtm-inf-s* 2.0f0)))
(assert-equal (dtm-pr (mul-generic *dtm-inf-s* 0.0f0)) (dtm-pr (mul-single *dtm-inf-s* 0.0f0)))
(assert-equal (dtm-pr (mul-generic *dtm-nan-s* 2.0f0)) (dtm-pr (mul-single *dtm-nan-s* 2.0f0)))
(assert-equal '<single-float> (%%class-name (class-of (mul-single 2.0f0 3.0f0))))

;;; --- double / 混在 / 3 引数(対照)---
(assert-equal (dtm-pr (mul-generic 1.5d0 2.25d0)) (dtm-pr (mul-double 1.5d0 2.25d0)))
(assert-equal '<double-float> (%%class-name (class-of (mul-double 2.0d0 3.0d0))))
(assert-equal (dtm-pr (mul-generic 1000 2.0f0)) (dtm-pr (mul-mixed 1000 2.0f0)))
(assert-equal (* 2 3 4) (mul3-fixnum 2 3 4))
(assert-equal 24 (mul3-fixnum 2 3 4))
(assert-equal (* -2 3 4) (mul3-fixnum -2 3 4))
;; 対照群 <integer> が嘘でないこと(bignum を渡しても GENERIC と一致する)
(defglobal *dtm-big* (* *dtm-maxfix* 4))
(assert-equal (mul-generic *dtm-big* 3) (mul-integer *dtm-big* 3))
(assert-equal (mul-generic *dtm-big* -3) (mul-integer *dtm-big* -3))

;;; --- let 束縛変数の宣言でも効くこと ---
(defun mul-let (n)
  (let ((a 6) (b 7))
    (declare (type <fixnum> a)) (declare (type <fixnum> b))
    (* a b)))
(assert-equal t (%%za-compiled-p (function mul-let)))
(assert-equal 42 (mul-let 0))
(assert-equal t (dtm-has-callee 'mul-let "primitive_multiply2_fixnum"))
