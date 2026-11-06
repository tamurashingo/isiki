;; test/lisp/compare_fixnum_test.lisp
;;
;; [#128 / M7] **型特化した比較(fixnum × fixnum)** の試験。
;; documents/compare-type-specialized.md。
;;
;; ## 符号の組み合わせを全部踏む
;;
;; **fixnum は符号＋絶対値である。** 生のタグ付き値を比べると負値は bit63 が
;; 立つので**巨大な正の値に見える**。`(declare (type <fixnum>))` は
;; **非負を意味しない**ので、特化版でも符号の判定は外せない。
;;
;; したがって 6 述語 × **4 つの符号の組**(++ / +- / -+ / --)を全部見る。
;; 0 も踏む(**-0 は存在しない**)。
;;
;; 答えはすべて**宣言なしの版と一致すること**で縛る(汎用は number_compare4 を
;; 通るので、独立した 2 つの経路が同じ答えを出すことになる)。

;;; --- 宣言なし(対照群)---
(defun cf-lt-gen (x y) (< x y))
(defun cf-gt-gen (x y) (> x y))
(defun cf-le-gen (x y) (<= x y))
(defun cf-ge-gen (x y) (>= x y))
(defun cf-eq-gen (x y) (= x y))
(defun cf-ne-gen (x y) (/= x y))

;;; --- 宣言つき(特化版が選ばれる)---
(defun cf-lt (x y) (declare (type <fixnum> x)) (declare (type <fixnum> y)) (< x y))
(defun cf-gt (x y) (declare (type <fixnum> x)) (declare (type <fixnum> y)) (> x y))
(defun cf-le (x y) (declare (type <fixnum> x)) (declare (type <fixnum> y)) (<= x y))
(defun cf-ge (x y) (declare (type <fixnum> x)) (declare (type <fixnum> y)) (>= x y))
(defun cf-eq (x y) (declare (type <fixnum> x)) (declare (type <fixnum> y)) (= x y))
(defun cf-ne (x y) (declare (type <fixnum> x)) (declare (type <fixnum> y)) (/= x y))

;;; --- 1. 前提: JIT に乗っている ---
(assert-equal t (%%za-compiled-p (function cf-lt)))
(assert-equal t (%%za-compiled-p (function cf-gt)))
(assert-equal t (%%za-compiled-p (function cf-le)))
(assert-equal t (%%za-compiled-p (function cf-ge)))
(assert-equal t (%%za-compiled-p (function cf-eq)))
(assert-equal t (%%za-compiled-p (function cf-ne)))

;;; --- 2. 4 つの符号の組で、宣言なしと一致する ---
;; ++(両方非負。常道)
(assert-equal (cf-lt-gen 3 5) (cf-lt 3 5))
(assert-equal (cf-lt-gen 5 3) (cf-lt 5 3))
(assert-equal (cf-gt-gen 3 5) (cf-gt 3 5))
(assert-equal (cf-le-gen 3 3) (cf-le 3 3))
(assert-equal (cf-ge-gen 3 3) (cf-ge 3 3))
(assert-equal (cf-eq-gen 3 3) (cf-eq 3 3))
(assert-equal (cf-ne-gen 3 5) (cf-ne 3 5))
;; +-(左が非負、右が負)
(assert-equal (cf-lt-gen 3 -5) (cf-lt 3 -5))
(assert-equal (cf-gt-gen 3 -5) (cf-gt 3 -5))
(assert-equal (cf-le-gen 3 -5) (cf-le 3 -5))
(assert-equal (cf-ge-gen 3 -5) (cf-ge 3 -5))
(assert-equal (cf-eq-gen 3 -5) (cf-eq 3 -5))
(assert-equal (cf-ne-gen 3 -5) (cf-ne 3 -5))
;; -+(左が負、右が非負)
(assert-equal (cf-lt-gen -3 5) (cf-lt -3 5))
(assert-equal (cf-gt-gen -3 5) (cf-gt -3 5))
(assert-equal (cf-le-gen -3 5) (cf-le -3 5))
(assert-equal (cf-ge-gen -3 5) (cf-ge -3 5))
(assert-equal (cf-eq-gen -3 5) (cf-eq -3 5))
(assert-equal (cf-ne-gen -3 5) (cf-ne -3 5))
;; --(両方負。**絶対値の大小が逆になる**)
(assert-equal (cf-lt-gen -5 -3) (cf-lt -5 -3))
(assert-equal (cf-lt-gen -3 -5) (cf-lt -3 -5))
(assert-equal (cf-gt-gen -5 -3) (cf-gt -5 -3))
(assert-equal (cf-le-gen -3 -3) (cf-le -3 -3))
(assert-equal (cf-ge-gen -3 -3) (cf-ge -3 -3))
(assert-equal (cf-eq-gen -3 -3) (cf-eq -3 -3))
(assert-equal (cf-ne-gen -5 -3) (cf-ne -5 -3))

;;; --- 3. 絶対値でも固定する(一致だけだと両方壊れても通る)---
;; **両方負のとき、絶対値が大きいほど小さい**
(assert-equal t   (cf-lt -5 -3))
(assert-equal nil (cf-lt -3 -5))
(assert-equal t   (cf-gt -3 -5))
(assert-equal nil (cf-gt -5 -3))
(assert-equal t   (cf-le -5 -3))
(assert-equal t   (cf-le -3 -3))
(assert-equal t   (cf-ge -3 -5))
(assert-equal t   (cf-eq -3 -3))
(assert-equal nil (cf-eq -3 -5))
(assert-equal t   (cf-ne -3 -5))
(assert-equal nil (cf-ne -3 -3))
;; 符号が違えば負のほうが小さい
(assert-equal t   (cf-lt -1 0))
(assert-equal t   (cf-lt -1 1))
(assert-equal nil (cf-lt 1 -1))
(assert-equal t   (cf-gt 1 -1))
;; **-0 は存在しない**ので 0 と 0 は等しい
(assert-equal t   (cf-eq 0 0))
(assert-equal nil (cf-ne 0 0))
(assert-equal nil (cf-lt 0 0))
(assert-equal t   (cf-le 0 0))
;; 大きい絶対値
(assert-equal t   (cf-lt -1000000 -999999))
(assert-equal t   (cf-gt 1000000 999999))
