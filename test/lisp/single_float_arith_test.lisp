;; test/lisp/single_float_arith_test.lisp
;;
;; single-float の算術と比較を、C への call を挟まずに処理する
;; (documents/single-float-arith.md)。
;;
;; **確認の主眼は「C 経由と JIT 経由がビット単位で一致すること」**である。
;; 同じ式をインタプリタ(C 経由)と JIT コンパイル済み関数の両方で評価し、
;; 印字結果を突き合わせる。±0.0 / 無限大 / NaN / 非正規化数を含む。
;;
;; NaN の規則は PR #85(C 側)で確定している。ucomiss は非順序を正しく
;; 区別できるので、**両経路が一致すること**がそのまま正しさの確認になる。



(defun pr (x) (let ((s (create-string-output-stream))) (format s "~S" x) (get-output-stream-string s)))

(defglobal *nan-s* (/ 0.0f0 0.0f0))
(defglobal *inf-s* (/ 1.0f0 0.0f0))
(defglobal *ninf-s* (/ -1.0f0 0.0f0))

;;; JIT 経由の比較(single 同士なので ucomiss 経路に入る)
(defun c-lt (a b) (< a b))
(defun c-gt (a b) (> a b))
(defun c-le (a b) (<= a b))
(defun c-ge (a b) (>= a b))
(defun c-eq (a b) (= a b))
(defun c-ne (a b) (/= a b))
(assert-equal t (%%za-compiled-p (function c-lt)))
(assert-equal t (%%za-compiled-p (function c-eq)))
(assert-equal t (%%za-compiled-p (function c-ne)))

;;; --- 通常の値: JIT 経由 = インタプリタ経由 ---
(assert-equal (< 1.0f0 2.0f0) (c-lt 1.0f0 2.0f0))
(assert-equal (< 2.0f0 1.0f0) (c-lt 2.0f0 1.0f0))
(assert-equal (< 1.0f0 1.0f0) (c-lt 1.0f0 1.0f0))
(assert-equal (> 2.0f0 1.0f0) (c-gt 2.0f0 1.0f0))
(assert-equal (<= 1.0f0 1.0f0) (c-le 1.0f0 1.0f0))
(assert-equal (<= 2.0f0 1.0f0) (c-le 2.0f0 1.0f0))
(assert-equal (>= 1.0f0 1.0f0) (c-ge 1.0f0 1.0f0))
(assert-equal (= 1.0f0 1.0f0) (c-eq 1.0f0 1.0f0))
(assert-equal (= 1.0f0 2.0f0) (c-eq 1.0f0 2.0f0))
(assert-equal (/= 1.0f0 2.0f0) (c-ne 1.0f0 2.0f0))
(assert-equal (/= 1.0f0 1.0f0) (c-ne 1.0f0 1.0f0))
;; 具体値でも固定
(assert-equal t (c-lt 1.0f0 2.0f0))
(assert-equal nil (c-lt 2.0f0 1.0f0))
(assert-equal t (c-ge 1.0f0 1.0f0))
(assert-equal t (c-eq 1.0f0 1.0f0))
(assert-equal nil (c-ne 1.0f0 1.0f0))

;;; --- NaN: /= だけが真。JIT 経由とインタプリタ経由が一致すること ---
(assert-equal nil (c-lt *nan-s* 1.0f0))
(assert-equal nil (c-lt 1.0f0 *nan-s*))
(assert-equal nil (c-gt *nan-s* 1.0f0))
(assert-equal nil (c-gt 1.0f0 *nan-s*))
(assert-equal nil (c-le *nan-s* 1.0f0))
(assert-equal nil (c-le 1.0f0 *nan-s*))
(assert-equal nil (c-ge *nan-s* 1.0f0))
(assert-equal nil (c-ge 1.0f0 *nan-s*))
(assert-equal nil (c-eq *nan-s* *nan-s*))
(assert-equal nil (c-eq *nan-s* 1.0f0))
(assert-equal t (c-ne *nan-s* *nan-s*))
(assert-equal t (c-ne *nan-s* 1.0f0))
(assert-equal t (c-ne 1.0f0 *nan-s*))
;; インタプリタ経由と突き合わせ
(assert-equal (= *nan-s* *nan-s*) (c-eq *nan-s* *nan-s*))
(assert-equal (/= *nan-s* *nan-s*) (c-ne *nan-s* *nan-s*))
(assert-equal (< *nan-s* 1.0f0) (c-lt *nan-s* 1.0f0))
(assert-equal (<= *nan-s* 1.0f0) (c-le *nan-s* 1.0f0))

;;; --- 無限大は順序づけられる ---
(assert-equal t (c-lt 1.0f0 *inf-s*))
(assert-equal t (c-eq *inf-s* *inf-s*))
(assert-equal t (c-lt *ninf-s* *inf-s*))
(assert-equal nil (c-eq *inf-s* *ninf-s*))
(assert-equal nil (c-lt *inf-s* *nan-s*))
(assert-equal t (c-ne *inf-s* *nan-s*))

;;; --- 混在・整数はフォールバック(C 経由)で従来どおり ---
(assert-equal t (c-lt 1.0f0 2.0d0))
(assert-equal t (c-lt 1 2))
(assert-equal nil (c-eq 0.1f0 0.1d0))
(assert-equal t (c-eq 1 1))
(assert-equal t (c-ne 1 2))
(assert-equal nil (c-ne 1 1))

;;; --- ±0.0 ---
(assert-equal t (c-eq 0.0f0 -0.0f0))
(assert-equal nil (c-ne 0.0f0 -0.0f0))

;;; --- 非正規化数(MXCSR の FTZ/DAZ が効いていると C 経由と食い違う) ---
;;; single の正規化最小は約 1.18f-38。**1.0f-40 は非正規化数**である。
;;; 指数マーカーは f を使うこと(1.0e38f0 のような書き方は PR #79 以降
;;; 曖昧で、意図した値にならない)。
(defglobal *denorm* 1.0f-40)
(defun sf-mul2 (a b) (* a b))
(defun sf-add2 (a b) (+ a b))
(format *isiki-test-stream* "[SFA] denorm=~A  x2=~A~%" (pr *denorm*) (pr (sf-add2 *denorm* *denorm*)))
(finish-output *isiki-test-stream*)
;; 0 に潰れていない = DAZ も FTZ も効いていない
(assert-equal t (c-ne *denorm* 0.0f0))
;; JIT 経由(XMM)と C 経由(double 経由)がビット単位で一致すること
(assert-equal (pr (+ *denorm* *denorm*)) (pr (sf-add2 *denorm* *denorm*)))
(assert-equal (pr (* *denorm* 2.0f0)) (pr (sf-mul2 *denorm* 2.0f0)))
(assert-equal (pr (* *denorm* 0.5f0)) (pr (sf-mul2 *denorm* 0.5f0)))
;; 非正規化数どうしの比較も一致
(assert-equal (< *denorm* (* *denorm* 2.0f0)) (c-lt *denorm* (* *denorm* 2.0f0)))
(assert-equal t (c-lt *denorm* (sf-mul2 *denorm* 2.0f0)))


;;; --- 除算 / quotient / reciprocal(PR #87 追補) ---
;;; ISLisp に / は無く、quotient の実装(init.lisp の %quotient2)が内部で使う経路。
;;; **二項の / だけが専用経路に乗る。** (/ a b c) は一般呼び出しのまま。
(defun d-div (a b) (/ a b))
(defun d-quo (a b) (quotient a b))
(defun d-rec (x) (reciprocal x))
(assert-equal t (%%za-compiled-p (function d-div)))

;; C 経由と JIT 経由がビット単位で一致すること
(assert-equal (pr (/ 1.0f0 3.0f0)) (pr (d-div 1.0f0 3.0f0)))
(assert-equal (pr (/ 1.5f0 2.5f0)) (pr (d-div 1.5f0 2.5f0)))
(assert-equal (pr (/ 7.0f0 3.0f0)) (pr (d-div 7.0f0 3.0f0)))
(assert-equal (pr (/ -1.0f0 3.0f0)) (pr (d-div -1.0f0 3.0f0)))
;; 型が保たれること
(assert-equal '<single-float> (%%class-name (class-of (d-div 1.0f0 3.0f0))))
(assert-equal '<double-float> (%%class-name (class-of (d-div 1.0f0 3.0d0))))  ; 混在は C 経由
(assert-equal '<single-float> (%%class-name (class-of (d-div 1 3.0f0))))      ; 整数混在も single
;; ゼロ除算は IEEE どおり inf/nan(/ の側。quotient とは別)
(assert-equal (pr (/ 1.0f0 0.0f0)) (pr (d-div 1.0f0 0.0f0)))
(assert-equal (pr (/ 0.0f0 0.0f0)) (pr (d-div 0.0f0 0.0f0)))
;; 整数どうしは従来どおり(single 経路に入らない)
(assert-equal 2 (d-div 4 2))

;;; quotient: 両方整数で割り切れれば整数、でなければ float
(assert-equal 2 (d-quo 4 2))
(assert-equal t (floatp (d-quo 1 2)))
(assert-equal '<single-float> (%%class-name (class-of (d-quo 1.0f0 3.0f0))))
(assert-equal (pr (quotient 1.0f0 3.0f0)) (pr (d-quo 1.0f0 3.0f0)))
(assert-equal (pr (quotient 1.5f0 2.5f0)) (pr (d-quo 1.5f0 2.5f0)))
;; quotient のゼロ除算は <division-by-zero>(/ の inf とは異なる)
(assert-error (quotient 1.0f0 0.0f0))
(assert-error (quotient 1 0))

;;; reciprocal: (quotient 1 x) なので整数 1 と x の混在になる
(assert-equal '<single-float> (%%class-name (class-of (d-rec 2.0f0))))
(assert-equal (pr (reciprocal 2.0f0)) (pr (d-rec 2.0f0)))
(assert-equal (pr (reciprocal 3.0f0)) (pr (d-rec 3.0f0)))
(assert-equal 0.5f0 (d-rec 2.0f0))   ; **接尾辞を落とすと double で読まれて不一致になる**
(assert-error (reciprocal 0))

;;; --- 二重丸めの確認 ---
;;; **single どうしの除算は single で割る。** double で割ってから single へ丸めると
;;; 2 回丸めることになり、divss と食い違いうる(documents/single-float-arith.md §5)。
;;; 下は「C 経由(primitive_divide2)と JIT 経由(divss)が一致する」ことの確認。
(defun dd (a b) (/ a b))
(assert-equal (pr (/ 1.0f0 49.0f0)) (pr (dd 1.0f0 49.0f0)))
(assert-equal (pr (/ 1.0f0 7.0f0)) (pr (dd 1.0f0 7.0f0)))
(assert-equal (pr (/ 16777215.0f0 16777213.0f0)) (pr (dd 16777215.0f0 16777213.0f0)))
(format *isiki-test-stream* "[SFA] 1/3=~A 1/49=~A~%" (pr (dd 1.0f0 3.0f0)) (pr (dd 1.0f0 49.0f0)))
(finish-output *isiki-test-stream*)
