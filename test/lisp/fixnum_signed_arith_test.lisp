;; test/lisp/fixnum_signed_arith_test.lisp
;;
;; 改善A(負の fixnum が bignum 機構を通る問題の修正)の回帰テスト。
;; documents/fixnum-signed-fastpath.md
;;
;; fixnum は **符号マグニチュード表現**(bit63=符号 / bit3〜62=絶対値60bit)であり
;; 2 の補数ではない。以前は `+` / `-` の高速 path が「両方とも非負 fixnum」に
;; 限られていたため、`(+ -1 1)` のように両方 fixnum で結果も fixnum の式が
;; decompose → limb_alloc → mag_sub → os_make_integer という bignum 用の機構を
;; 丸ごと通っていた。
;;
;; ここで見るのは **値の正しさ**である。符号の組み合わせ・0 の扱い・60bit 境界の
;; 昇格/降格が、高速 path を広げても従来どおりであることを確認する。
;;
;; **`-0` が生まれないこと**が特に重要で、fixnum は即値なので `-0` ができると
;; `(eq 0 ...)` が偽になり、`eq` で 0 を判定している既存コードが静かに壊れる。

;;; --- 1. 符号の組み合わせ(加算) ---
(assert-equal 2    (+ 1 1))
(assert-equal 0    (+ 1 -1))
(assert-equal 0    (+ -1 1))
(assert-equal -2   (+ -1 -1))
(assert-equal 2    (+ 5 -3))
(assert-equal -2   (+ 3 -5))
(assert-equal -2   (+ -5 3))
(assert-equal 2    (+ -3 5))

;;; --- 2. 符号の組み合わせ(減算) ---
(assert-equal 0    (- 1 1))
(assert-equal 2    (- 1 -1))
(assert-equal -2   (- -1 1))
(assert-equal 0    (- -1 -1))
(assert-equal 2    (- 5 3))
(assert-equal -2   (- 3 5))
(assert-equal -8   (- -5 3))
(assert-equal 8    (- 5 -3))
(assert-equal -2   (- -5 -3))
(assert-equal 2    (- -3 -5))

;;; --- 3. 0 が絡む組み合わせ ---
(assert-equal 0    (+ 0 0))
(assert-equal -1   (+ 0 -1))
(assert-equal -1   (+ -1 0))
(assert-equal 0    (- 0 0))
(assert-equal 1    (- 0 -1))
(assert-equal -1   (- 0 1))
(assert-equal -1   (- -1 0))

;;; --- 4. -0 が生まれないこと ---
;; fixnum は即値なので、-0(符号ビットだけ立った 0)ができると eq が偽になる。
;; os_make_fixnum_signed がマグニチュード 0 のとき符号を落とすことを確認する。
(assert-equal t (eq 0 (+ -1 1)))
(assert-equal t (eq 0 (+ 1 -1)))
(assert-equal t (eq 0 (- 1 1)))
(assert-equal t (eq 0 (- -1 -1)))
(assert-equal t (eq 0 (+ 0 0)))
(assert-equal t (eq 0 (- 0 0)))
(assert-equal t (eq 0 (+ -5 5)))
(assert-equal t (eq 0 (- -5 -5)))
(assert-equal t (eq 0 (- 0 0 0)))
;; 単項マイナスでも -0 を作らない
(assert-equal t (eq 0 (- 0)))
(assert-equal t (fixnump (- 0)))
;; 0 との比較・述語
(assert-equal t (= 0 (+ -1 1)))
(assert-equal t (fixnump (+ -1 1)))
(assert-equal t (fixnump (- 1 2)))

;;; --- 5. 単項マイナスと多項 ---
(assert-equal -1   (- 1))
(assert-equal 1    (- -1))
(assert-equal 0    (- 0))
(assert-equal -6   (- 0 1 2 3))
(assert-equal 6    (- 0 -1 -2 -3))
(assert-equal 0    (+))
(assert-equal 5    (+ 5))
(assert-equal -5   (+ -5))
(assert-equal 0    (+ 1 2 3 -6))
(assert-equal 0    (+ -1 -2 -3 6))
(assert-equal -4   (+ 1 -2 3 -6))
;; 途中で符号がまたぐ多項(高速 path のアキュムレータが符号を持ち越せること)
(assert-equal 1    (+ 10 -20 30 -19))
(assert-equal 1    (- 10 20 -30 19))

;;; --- 6. fixnum 境界: 昇格 ---
;; 境界は *most-positive-fixnum* から取る。この定数はC側の FIXNUM_MAGNITUDE_MASK
;; 由来なので(init.lisp)、タグ幅や FIXNUM_VALUE_SHIFT を変えても自動で追随する。
;; (expt 2 60) を直書きしていたが、タグ4bit化で上限が 2^59-1 になり
;; fixnump 系6件が落ちた。ここは「上限ちょうど」であることが試験の主旨なので、
;; 値を書き換えるのではなく導出に変える。
(defglobal *max-fix* *most-positive-fixnum*)
(assert-equal t (fixnump *max-fix*))
(assert-equal t (bignump (+ *max-fix* 1)))          ; 正側の昇格
(assert-equal t (bignump (+ (- *max-fix*) -1)))     ; 負側の昇格
(assert-equal t (bignump (- (- *max-fix*) 1)))      ; 減算からの負側の昇格
(assert-equal t (bignump (- *max-fix* -1)))         ; 減算からの正側の昇格
(assert-equal t (fixnump (+ *max-fix* 0)))          ; 境界ちょうどは fixnum のまま
(assert-equal t (fixnump (- (- *max-fix*) 0)))

;;; --- 7. 60bit 境界: 降格 ---
(assert-equal t (fixnump (- (+ *max-fix* 1) 1)))    ; bignum → fixnum
(assert-equal t (fixnump (+ (+ *max-fix* 1) -1)))
(assert-equal *max-fix* (- (+ *max-fix* 1) 1))
(assert-equal *max-fix* (+ (+ *max-fix* 1) -1))
;; 異符号で打ち消して 0 へ(結果は fixnum の 0、かつ -0 ではない)
(assert-equal 0 (+ *max-fix* (- *max-fix*)))
(assert-equal t (fixnump (+ *max-fix* (- *max-fix*))))
(assert-equal t (eq 0 (+ *max-fix* (- *max-fix*))))
(assert-equal 0 (- *max-fix* *max-fix*))
(assert-equal t (eq 0 (- *max-fix* *max-fix*)))

;;; --- 7-2. 多項で途中経過が 60bit を超える場合 ---
;; 高速 path は左から順に畳むので、**途中で溢れたら最終結果が fixnum に収まっていても**
;; 一般パスへ落ちる(この順序依存は改善A の前後で変わらない)。
;; 落ちた先の一般パスが正しい答えを返すことを確認する。
(assert-equal 0 (+ *max-fix* *max-fix* (- *max-fix*) (- *max-fix*)))
(assert-equal t (fixnump (+ *max-fix* *max-fix* (- *max-fix*) (- *max-fix*))))
(assert-equal t (eq 0 (+ *max-fix* *max-fix* (- *max-fix*) (- *max-fix*))))
(assert-equal 0 (- 0 *max-fix* *max-fix* (- *max-fix*) (- *max-fix*)))
(assert-equal t (bignump (- 0 *max-fix* *max-fix*)))

;;; --- 8. 内部表現は class-of から見える(single-float導入で仕様を変更) ---
;;; 以前は境界の前後どちらも <INTEGER> だった(ISLisp準拠)。
;;; 内部表現を型階層へ公開する方針に変えたため、fixnum/bignum が直接返る。
;;; <integer> との関係は typep / subclassp 側で保たれている(下の2件)。
(assert-equal '<fixnum> (%%class-name (class-of 1)))
(assert-equal '<fixnum> (%%class-name (class-of -1)))
(assert-equal '<fixnum> (%%class-name (class-of *max-fix*)))
(assert-equal '<bignum> (%%class-name (class-of (+ *max-fix* 1))))
(assert-equal '<bignum> (%%class-name (class-of (- (- *max-fix*) 1))))
;;; 境界の前後で <integer> であることは変わらない
(assert-equal t (typep *max-fix* '<integer>))
(assert-equal t (typep (+ *max-fix* 1) '<integer>))

;;; --- 9. bignum が絡むケースは従来どおり ---
(defglobal *big* (expt 2 100))
(assert-equal t (bignump *big*))
(assert-equal t (bignump (+ *big* 1)))
(assert-equal t (bignump (+ *big* -1)))
(assert-equal t (bignump (- *big* 1)))
(assert-equal 0 (- *big* *big*))
(assert-equal t (eq 0 (- *big* *big*)))
(assert-equal t (eq 0 (+ *big* (- *big*))))
(assert-equal *big* (+ (- *big*) (* 2 *big*)))
;; bignum から fixnum への降格
(assert-equal 1 (- (+ *big* 1) *big*))
(assert-equal t (fixnump (- (+ *big* 1) *big*)))

;;; --- 10. float が絡むケースは従来どおり ---
(assert-equal 0.5 (+ -0.5 1.0))
(assert-equal 0.5 (+ -1 1.5))
(assert-equal -0.5 (- 1 1.5))
(assert-equal t (floatp (+ -1 1.5)))

;;; --- 11. JIT 経由(primitive_add2 / primitive_subtract2)でも同じ結果 ---
;; JIT は「両方が非負 fixnum」のときだけインラインで計算し、それ以外は
;; primitive_add2 / primitive_subtract2 を呼ぶ。**呼ばれる側が今回の変更点**なので、
;; JIT コンパイルされた関数からも符号の組み合わせを確認する。
(defun fsa-add (a b) (+ a b))
(defun fsa-sub (a b) (- a b))
(assert-equal t (%%za-compiled-p (function fsa-add)))
(assert-equal t (%%za-compiled-p (function fsa-sub)))

(assert-equal 2   (fsa-add 1 1))
(assert-equal 0   (fsa-add 1 -1))
(assert-equal 0   (fsa-add -1 1))
(assert-equal -2  (fsa-add -1 -1))
(assert-equal t   (eq 0 (fsa-add -1 1)))

(assert-equal 0   (fsa-sub 1 1))
(assert-equal 2   (fsa-sub 1 -1))
(assert-equal -2  (fsa-sub -1 1))
(assert-equal 0   (fsa-sub -1 -1))
(assert-equal -1  (fsa-sub 1 2))          ; 結果が負(以前はここで bignum 機構へ落ちていた)
(assert-equal t   (fixnump (fsa-sub 1 2)))
(assert-equal t   (eq 0 (fsa-sub -1 -1)))

;; JIT 経由の 60bit 境界
(assert-equal t (bignump (fsa-add *max-fix* 1)))
(assert-equal t (bignump (fsa-sub (- *max-fix*) 1)))
(assert-equal t (fixnump (fsa-sub (+ *max-fix* 1) 1)))
(assert-equal *max-fix* (fsa-sub (+ *max-fix* 1) 1))

;;; --- 12. ループで繰り返しても壊れない(GC を挟んでも即値のまま) ---
;; 高速 path はヒープを確保しないので、以下のループは 1 度も cons を作らない。
(defun fsa-alternating (n)
  (let ((i 0) (s 0))
    (while (< i n)
      (setq s (+ s -1))
      (setq s (- s -1))
      (setq i (+ i 1)))
    s))
(assert-equal t (%%za-compiled-p (function fsa-alternating)))
(assert-equal 0 (fsa-alternating 1000))
(assert-equal t (eq 0 (fsa-alternating 1000)))

(defun fsa-down (n)
  (let ((i 0) (s 0))
    (while (< i n)
      (setq s (- s 1))
      (setq i (+ i 1)))
    s))
(assert-equal t (%%za-compiled-p (function fsa-down)))
(assert-equal -1000 (fsa-down 1000))
(assert-equal t (fixnump (fsa-down 1000)))
