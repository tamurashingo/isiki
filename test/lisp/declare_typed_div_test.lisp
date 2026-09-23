;; test/lisp/declare_typed_div_test.lisp
;;
;; declare の型を使った / の特化と、GENERIC 側の fixnum 除算の改善(改善C)。
;; documents/declare-typed-arith.md §9
;;
;; この PR は 2 つの変更を含む。
;;   1. GENERIC の高速路(負数対応 + 内側 cons の除去)。宣言なしにも効く
;;   2. 宣言による特化
;;
;; **/ だけが持つ性質が 2 つある。**
;;   - 桁溢れしない。fixnum ÷ fixnum が bignum へ昇格することはない
;;   - **ゼロ検査は宣言が正しくても外せない。** x86-64 の div は除数 0 で #DE を
;;     出し、freestanding ではハンドラが無いので機械が止まる。
;;     **このファイルの「宣言ありでゼロ除算」は、止まらないことの確認である。**

(defun dtd-pr (x) (let ((s (create-string-output-stream))) (format s "~S" x) (get-output-stream-string s)))

(defun dtd-has-callee (name callee)
  (let ((items (disasm-items (%%disasm-code-base name) (%%disasm-code-len name)))
        (found nil))
    (while (not (null items))
      (if (string= callee (disasm-item-comment (cdr (car items))))
          (setq found t)
        nil)
      (setq items (cdr items)))
    found))

(defun div-generic (x y) (/ x y))
(defun div-fixnum (x y)
  (declare (type <fixnum> x)) (declare (type <fixnum> y)) (/ x y))
(defun div-single (x y)
  (declare (type <single-float> x)) (declare (type <single-float> y)) (/ x y))
(defun div-double (x y)
  (declare (type <double-float> x)) (declare (type <double-float> y)) (/ x y))
(defun div-integer (x y)
  (declare (type <integer> x)) (declare (type <integer> y)) (/ x y))
(defun div-mixed (x y)
  (declare (type <fixnum> x)) (declare (type <single-float> y)) (/ x y))
(defun div3-fixnum (x y z)
  (declare (type <fixnum> x)) (declare (type <fixnum> y)) (declare (type <fixnum> z))
  (/ x y z))

(assert-equal t (%%za-compiled-p (function div-generic)))
(assert-equal t (%%za-compiled-p (function div-fixnum)))
(assert-equal t (%%za-compiled-p (function div-single)))
;; **3 引数の / は、そもそも JIT に乗らない。**(/ a b c) は一般呼び出しへ落ち、
;; za はその関数まるごとコンパイルを諦める(documents/divide-direct-call.md §3-1)。
;; 本作業で変えていない既存の性質で、ここでは記録として確認する
(assert-equal nil (%%za-compiled-p (function div3-fixnum)))

;;; --- 6-1. 呼び先はシンボル名で確認する(code-len では判別できない)---
(assert-equal t (dtd-has-callee 'div-generic "primitive_divide2"))
(assert-equal t (dtd-has-callee 'div-fixnum "primitive_divide2_fixnum"))
(assert-equal t (dtd-has-callee 'div-single "primitive_divide2_single"))
(assert-equal nil (dtd-has-callee 'div-fixnum "primitive_divide2_single"))
(assert-equal nil (dtd-has-callee 'div-single "primitive_divide2_fixnum"))
;; 他の演算の特化を呼んでいないこと
(assert-equal nil (dtd-has-callee 'div-fixnum "primitive_multiply2_fixnum"))
(assert-equal nil (dtd-has-callee 'div-fixnum "primitive_add2_fixnum"))
;; 対照群は GENERIC のまま
(assert-equal t (dtd-has-callee 'div-double "primitive_divide2"))
(assert-equal t (dtd-has-callee 'div-integer "primitive_divide2"))
(assert-equal t (dtd-has-callee 'div-mixed "primitive_divide2"))

;;; --- 4-1. 割り切れないときの戻り値(**現状の記録。変えていない**)---
;; ratio は未実装(タグ 0xD は予約のまま)。整数どうしは商の整数部を返す
(assert-equal 3 (/ 7 2))
(assert-equal 0 (/ 1 3))
(assert-equal 3 (div-fixnum 7 2))
(assert-equal 0 (div-fixnum 1 3))

;;; --- 6-2. 符号の全組み合わせ。**切り捨てはゼロ方向** ---
(assert-equal 2 (div-fixnum 6 3))
(assert-equal -2 (div-fixnum 6 -3))
(assert-equal -2 (div-fixnum -6 3))
(assert-equal 2 (div-fixnum -6 -3))
;; **床方向なら -4 になる。ゼロ方向であることの確認**
(assert-equal 3 (div-fixnum 7 2))
(assert-equal -3 (div-fixnum 7 -2))
(assert-equal -3 (div-fixnum -7 2))
(assert-equal 3 (div-fixnum -7 -2))
;; 宣言なしでも同じ
(assert-equal (div-generic -7 2) (div-fixnum -7 2))
(assert-equal (div-generic 7 -2) (div-fixnum 7 -2))
(assert-equal (div-generic -7 -2) (div-fixnum -7 -2))
(assert-equal (div-generic -6 3) (div-fixnum -6 3))
;; **-0 を作らないこと**
(assert-equal 0 (div-fixnum 0 5))
(assert-equal 0 (div-fixnum 0 -5))
(assert-equal (dtd-pr 0) (dtd-pr (div-fixnum 0 -5)))
(assert-equal (dtd-pr (div-generic 0 -5)) (dtd-pr (div-fixnum 0 -5)))
;; 商が 0 になる場合も同じ(-1/5 = 0、-0 ではない)
(assert-equal 0 (div-fixnum -1 5))
(assert-equal (dtd-pr 0) (dtd-pr (div-fixnum -1 5)))
;; 符号が後段へ漏れないこと
(assert-equal 1 (quotient 1 (+ (div-fixnum 0 -5) 1)))

;;; --- 6-3. ゼロ除算 ---
;; [P4-1] 整数のゼロ除算は <division-by-zero> を signal するようになった。
;; 以前は EVAL-ERROR を**値として**返しており、<division-by-zero> を出すのは
;; quotient 側だけの責務だった。**/ は ISLisp 仕様に無い実装独自の名前**なので
;; error-id の指定は無いが、同じ演算である quotient(spec:4365)と div(spec:4889)に
;; 揃えた(documents/error-unwind-survey.md §A-4)。
;;
;; **宣言あり/なし・特化の有無で挙動が変わらないことが要点。** 型宣言つきの
;; 高速路(div-fixnum)も、汎用路と同じく signal しなければならない
;; (特化版 primitive_divide2_fixnum は 0 のとき n 項版 primitive_divide へ
;;  委譲しているので、経路は 1 つに合流する)
(assert-error-class '<division-by-zero> (/ 1 0))
(assert-error-class '<division-by-zero> (/ -1 0))
(assert-error-class '<division-by-zero> (div-generic 1 0))
(assert-error-class '<division-by-zero> (div-fixnum 1 0))
(assert-error-class '<division-by-zero> (div-fixnum -1 0))
(assert-error-class '<division-by-zero> (div-fixnum 0 0))
;; float は IEEE 754 どおり(MXCSR=0x1F80 で例外はマスクされている)
(assert-equal (dtd-pr (div-generic 1.0f0 0.0f0)) (dtd-pr (div-single 1.0f0 0.0f0)))
(assert-equal (dtd-pr (div-generic -1.0f0 0.0f0)) (dtd-pr (div-single -1.0f0 0.0f0)))
(assert-equal (dtd-pr (div-generic 0.0f0 0.0f0)) (dtd-pr (div-single 0.0f0 0.0f0)))
(assert-equal (dtd-pr (div-generic 1.0f0 -0.0f0)) (dtd-pr (div-single 1.0f0 -0.0f0)))
(assert-equal (dtd-pr (div-generic 1.0d0 0.0d0)) (dtd-pr (div-double 1.0d0 0.0d0)))

;;; --- 6-4. 境界。**#DE で止まらないこと** ---
;; 2 の補数なら INT64_MIN / -1 は商が表現できず #DE。符号マグニチュードでは起きない
(assert-equal *most-positive-fixnum* (div-fixnum *most-negative-fixnum* -1))
(assert-equal (div-generic *most-negative-fixnum* -1) (div-fixnum *most-negative-fixnum* -1))
(assert-equal *most-positive-fixnum* (div-fixnum *most-positive-fixnum* 1))
(assert-equal *most-negative-fixnum* (div-fixnum *most-negative-fixnum* 1))
(assert-equal -1 (div-fixnum *most-negative-fixnum* *most-positive-fixnum*))
(assert-equal 1 (div-fixnum *most-negative-fixnum* *most-negative-fixnum*))
;; 桁溢れしないこと(商は必ず fixnum に収まる)
(assert-equal nil (bignump (div-fixnum *most-negative-fixnum* -1)))
(assert-equal nil (bignump (div-fixnum *most-positive-fixnum* 1)))

;;; --- single ---
(assert-equal (dtd-pr (div-generic 3.5f0 1.25f0)) (dtd-pr (div-single 3.5f0 1.25f0)))
(assert-equal (dtd-pr (div-generic -3.5f0 1.25f0)) (dtd-pr (div-single -3.5f0 1.25f0)))
;; 割り切れない商(二重丸めの経路。**GENERIC と一致すること**が要点)
(assert-equal (dtd-pr (div-generic 1.0f0 3.0f0)) (dtd-pr (div-single 1.0f0 3.0f0)))
(assert-equal (dtd-pr (div-generic 1.0f0 49.0f0)) (dtd-pr (div-single 1.0f0 49.0f0)))
(assert-equal (dtd-pr (div-generic 2.0f0 3.0f0)) (dtd-pr (div-single 2.0f0 3.0f0)))
(assert-equal (dtd-pr (div-generic 1.0f-40 3.0f0)) (dtd-pr (div-single 1.0f-40 3.0f0)))
(assert-equal '<single-float> (%%class-name (class-of (div-single 3.0f0 2.0f0))))

;;; --- double / 混在 / 3 引数(対照)---
(assert-equal (dtd-pr (div-generic 3.5d0 1.25d0)) (dtd-pr (div-double 3.5d0 1.25d0)))
(assert-equal '<double-float> (%%class-name (class-of (div-double 3.0d0 2.0d0))))
;;; 6-5. 混在は GENERIC へ
(assert-equal (dtd-pr (div-generic 1 2.0f0)) (dtd-pr (div-mixed 1 2.0f0)))
(assert-equal '<single-float> (%%class-name (class-of (div-mixed 1 2.0f0))))
(assert-equal (dtd-pr (/ 1.0f0 2.0d0)) (dtd-pr (div-generic 1.0f0 2.0d0)))
(assert-equal '<double-float> (%%class-name (class-of (/ 1.0f0 2.0d0))))
;; 3 引数は JIT に乗らないが(上記)、値は左畳み込みで正しいこと
(assert-equal (/ 100 5 2) (div3-fixnum 100 5 2))
(assert-equal 10 (div3-fixnum 100 5 2))
(assert-equal (/ -100 5 2) (div3-fixnum -100 5 2))
(assert-equal -10 (div3-fixnum -100 5 2))
;; 対照群 <integer> が嘘でないこと
(defglobal *dtd-big* (* *most-positive-fixnum* 4))
(assert-equal (div-generic *dtd-big* 3) (div-integer *dtd-big* 3))
(assert-equal (div-generic *dtd-big* -3) (div-integer *dtd-big* -3))
(assert-equal t (bignump (div-integer *dtd-big* 3)))

;;; --- 改善C: n 項版の高速路も負数に対応した ---
(assert-equal -10 (/ -100 5 2))
(assert-equal 10 (/ -100 -5 2))
(assert-equal -10 (/ 100 -5 2))
(assert-equal 0 (/ 0 -5 2))
(assert-equal (dtd-pr 0) (dtd-pr (/ 0 -5 2)))
;; [P4-1] n 項版も同じく <division-by-zero>(高速路・bignum 路の両方)
(assert-error-class '<division-by-zero> (/ 100 0 2))
(assert-error-class '<division-by-zero> (/ -100 0 2))
(assert-error-class '<division-by-zero> (/ -100 5 0))

;;; --- quotient は変わらないこと(**/ を経由していない**)---
;; %quotient2 は整数どうしなら mod / div(**床除算**)を使う。/ ではない。
;;
;; **この処理系には切り捨て方向の違う除算が 2 つある。**
;;   / と primitive_divide2_fixnum : ゼロ方向   (/ -7 2)   = -3
;;   div / mod (floor_divmod)      : 床方向     (div -7 2) = -4
;;
;; それでも quotient が壊れないのは、**%quotient2 が div を使うのが
;; 「(mod dividend divisor) が 0 のとき」に限られる**からである。
;; 割り切れる領域では床と切り捨てが一致するので、方向の違いが表に出ない。
;; 割り切れないときは float を返す(ISLisp §19 の quotient の規定どおり)。
;;
;; **この mod = 0 のガードが正しさを支えている。** ここを「最適化」で
;; 外すと負数で -4 と -3 が食い違う。以下はそれを固定するテストである。
(assert-equal 2 (quotient 6 3))
(assert-equal -2 (quotient -6 3))
(assert-equal -2 (quotient 6 -3))
(assert-equal 2 (quotient -6 -3))
;; 割り切れる領域では / と quotient が一致すること
(assert-equal (/ -6 3) (quotient -6 3))
(assert-equal (/ 6 -3) (quotient 6 -3))
(assert-equal (/ -6 -3) (quotient -6 -3))
;; **床除算そのものは確かに方向が違う。**(この 2 行が食い違いの存在を記録する)
(assert-equal -3 (/ -7 2))
(assert-equal -4 (div -7 2))
(assert-equal 1 (mod -7 2))
(assert-equal -4 (div 7 -2))
;; 割り切れないときは float になる(これも / ではなく (float x) 経由)。
;; **-4 ではなく -3.5 が返る**ので、床除算が表に出ていないことが分かる
(assert-equal (dtd-pr (quotient 7 2)) (dtd-pr (/ (float 7) (float 2))))
(assert-equal (dtd-pr (quotient -7 2)) (dtd-pr (/ (float -7) (float 2))))
(assert-equal (dtd-pr -3.5) (dtd-pr (quotient -7 2)))
(assert-equal (dtd-pr -3.5) (dtd-pr (quotient 7 -2)))
(assert-equal (dtd-pr 3.5) (dtd-pr (quotient -7 -2)))
(assert-equal 1 (reciprocal 1))
(assert-equal -1 (reciprocal -1))

;;; --- let 束縛変数の宣言でも効くこと ---
(defun div-let (n)
  (let ((a 84) (b 2))
    (declare (type <fixnum> a)) (declare (type <fixnum> b))
    (/ a b)))
(assert-equal t (%%za-compiled-p (function div-let)))
(assert-equal 42 (div-let 0))
(assert-equal t (dtd-has-callee 'div-let "primitive_divide2_fixnum"))
