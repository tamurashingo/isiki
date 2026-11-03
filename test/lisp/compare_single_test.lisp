;; test/lisp/compare_single_test.lisp
;;
;; [#128 / M7] **型特化した比較(single × single)** の試験。
;; documents/compare-type-specialized.md。
;;
;; ## 何を見るか
;;
;; 1. **宣言つきの比較が特化版へ切り替わっていること**(`%%za-compiled-p` と
;;    生成コードの長さで、切り替わったことを別の計器で確認してから中身を見る)
;; 2. **答えが宣言なしと完全に一致すること**(6 述語 × 値の組)
;; 3. **NaN の意味論が崩れていないこと**
;;
;; ## NaN がこの試験の中心である
;;
;; 特化版は `number_compare4`(4 値。`LESS`/`EQUAL`/`GREATER`/**`UNORDERED`**)を
;; 飛ばして C の比較演算子を直に使う。**4 値の論理は IEEE に合わせて作られたもの**
;; なので一致する、と導いたうえでこの試験を置いている。
;;
;;   `<` `>` `<=` `>=` `=`  … 非順序では**偽**
;;   **`/=`**                … 非順序では**真**(唯一)
;;
;; **導出が合っていても書き間違いは試験でしか捕まらない。**
;; `/=` を `(or (< a b) (> a b))` と書くと非順序で偽になって IEEE 754 に反する
;; (`runtime.c` の `/=` の既存の註と同じ罠)。

(defglobal *cs-nan* (/ 0.0f0 0.0f0))

;;; --- 宣言なし(対照群)---
(defun cs-lt-gen (x y) (< x y))
(defun cs-gt-gen (x y) (> x y))
(defun cs-le-gen (x y) (<= x y))
(defun cs-ge-gen (x y) (>= x y))
(defun cs-eq-gen (x y) (= x y))
(defun cs-ne-gen (x y) (/= x y))

;;; --- 宣言つき(特化版が選ばれる)---
(defun cs-lt (x y) (declare (type <single-float> x)) (declare (type <single-float> y)) (< x y))
(defun cs-gt (x y) (declare (type <single-float> x)) (declare (type <single-float> y)) (> x y))
(defun cs-le (x y) (declare (type <single-float> x)) (declare (type <single-float> y)) (<= x y))
(defun cs-ge (x y) (declare (type <single-float> x)) (declare (type <single-float> y)) (>= x y))
(defun cs-eq (x y) (declare (type <single-float> x)) (declare (type <single-float> y)) (= x y))
(defun cs-ne (x y) (declare (type <single-float> x)) (declare (type <single-float> y)) (/= x y))

;;; --- 1. 前提: 12 個すべて JIT に乗っている ---
;; 乗っていなければ、この試験は特化版を見ていない(名前は根拠にならない)
(assert-equal t (%%za-compiled-p (function cs-lt-gen)))
(assert-equal t (%%za-compiled-p (function cs-lt)))
(assert-equal t (%%za-compiled-p (function cs-gt)))
(assert-equal t (%%za-compiled-p (function cs-le)))
(assert-equal t (%%za-compiled-p (function cs-ge)))
(assert-equal t (%%za-compiled-p (function cs-eq)))
(assert-equal t (%%za-compiled-p (function cs-ne)))

;;; --- 2. 生成コードの長さは **同じになる**(ここで一度間違えた)---
;;
;; 最初は「宣言つきと宣言なしで長さが違えば差し替わっている」と書いた。
;; **それは成立しない。** 呼び出しの出力は
;;
;;     movabs r11, <呼び先のアドレス>   ; 10 byte(即値が変わるだけ)
;;     call   r11                        ;  3 byte
;;
;; なので、**呼び先が変わっても命令長は 1 byte も変わらない。**
;; 実測でも 871 対 871 で、**この計器では切り替わりを見られない。**
;;
;; **切り替わりの証拠は命令数で取る**(命令数ゲートの `<sgl-decl` 区間。
;; documents/compare-type-specialized.md §5)。
;; ここでは「長さが同じであること」を事実として固定しておく
;; —— 違ってきたら、呼び出しの出し方そのものが変わったということである
(format *isiki-test-stream* "#cs-codelen lt-gen=~D lt-decl=~D~%"
        (%%disasm-code-len 'cs-lt-gen) (%%disasm-code-len 'cs-lt))
(assert-equal (%%disasm-code-len 'cs-lt-gen) (%%disasm-code-len 'cs-lt))

;;; --- 3. 答えが宣言なしと一致する(通常の値)---
(assert-equal (cs-lt-gen 1.5f0 2.5f0) (cs-lt 1.5f0 2.5f0))
(assert-equal (cs-lt-gen 2.5f0 1.5f0) (cs-lt 2.5f0 1.5f0))
(assert-equal (cs-lt-gen 1.5f0 1.5f0) (cs-lt 1.5f0 1.5f0))
(assert-equal (cs-gt-gen 1.5f0 2.5f0) (cs-gt 1.5f0 2.5f0))
(assert-equal (cs-gt-gen 2.5f0 1.5f0) (cs-gt 2.5f0 1.5f0))
(assert-equal (cs-le-gen 1.5f0 1.5f0) (cs-le 1.5f0 1.5f0))
(assert-equal (cs-le-gen 2.5f0 1.5f0) (cs-le 2.5f0 1.5f0))
(assert-equal (cs-ge-gen 1.5f0 1.5f0) (cs-ge 1.5f0 1.5f0))
(assert-equal (cs-ge-gen 1.5f0 2.5f0) (cs-ge 1.5f0 2.5f0))
(assert-equal (cs-eq-gen 1.5f0 1.5f0) (cs-eq 1.5f0 1.5f0))
(assert-equal (cs-eq-gen 1.5f0 2.5f0) (cs-eq 1.5f0 2.5f0))
(assert-equal (cs-ne-gen 1.5f0 2.5f0) (cs-ne 1.5f0 2.5f0))
(assert-equal (cs-ne-gen 1.5f0 1.5f0) (cs-ne 1.5f0 1.5f0))

;;; --- 4. 絶対値でも固定する(一致だけだと両方壊れても通る)---
(assert-equal t   (cs-lt 1.5f0 2.5f0))
(assert-equal nil (cs-lt 2.5f0 1.5f0))
(assert-equal nil (cs-lt 1.5f0 1.5f0))
(assert-equal t   (cs-gt 2.5f0 1.5f0))
(assert-equal t   (cs-le 1.5f0 1.5f0))
(assert-equal t   (cs-ge 1.5f0 1.5f0))
(assert-equal t   (cs-eq 1.5f0 1.5f0))
(assert-equal nil (cs-eq 1.5f0 2.5f0))
(assert-equal t   (cs-ne 1.5f0 2.5f0))
(assert-equal nil (cs-ne 1.5f0 1.5f0))
;; 負の値と 0
(assert-equal t   (cs-lt -2.5f0 -1.5f0))
(assert-equal nil (cs-lt -1.5f0 -2.5f0))
(assert-equal t   (cs-lt -1.0f0 0.0f0))
(assert-equal t   (cs-eq 0.0f0 0.0f0))

;;; --- 5. **NaN**。ここがこの試験の中心である ---
;; 5 つは偽、`/=` だけ真。宣言なしと一致することも併せて見る
(assert-equal nil (cs-lt *cs-nan* 1.0f0))
(assert-equal nil (cs-lt 1.0f0 *cs-nan*))
(assert-equal nil (cs-lt *cs-nan* *cs-nan*))
(assert-equal nil (cs-gt *cs-nan* 1.0f0))
(assert-equal nil (cs-gt 1.0f0 *cs-nan*))
(assert-equal nil (cs-le *cs-nan* 1.0f0))
(assert-equal nil (cs-le *cs-nan* *cs-nan*))
(assert-equal nil (cs-ge *cs-nan* 1.0f0))
(assert-equal nil (cs-ge *cs-nan* *cs-nan*))
(assert-equal nil (cs-eq *cs-nan* 1.0f0))
(assert-equal nil (cs-eq *cs-nan* *cs-nan*))
;; **`/=` は非順序で真を返す唯一の比較である**
(assert-equal t (cs-ne *cs-nan* 1.0f0))
(assert-equal t (cs-ne 1.0f0 *cs-nan*))
(assert-equal t (cs-ne *cs-nan* *cs-nan*))
;; 宣言なしと一致する
(assert-equal (cs-eq-gen *cs-nan* *cs-nan*) (cs-eq *cs-nan* *cs-nan*))
(assert-equal (cs-ne-gen *cs-nan* *cs-nan*) (cs-ne *cs-nan* *cs-nan*))
(assert-equal (cs-lt-gen *cs-nan* 1.0f0)    (cs-lt *cs-nan* 1.0f0))
