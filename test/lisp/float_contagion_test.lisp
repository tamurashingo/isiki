;; test/lisp/float_contagion_test.lisp
;;
;; float の型昇格(float contagion)の回帰テスト。
;; documents/float-contagion.md
;;
;; 規則: **より広い形式へ寄せる。整数は float に従う。**
;;
;;   整数 × single → single      single × single → single
;;   整数 × double → double      single × double → double
;;
;; 本作業(c-1)の対象は算術(+ - * /)と比較(< > <= >= = /=)と選択(min max)。
;; **数学関数(sqrt/sin/log/expt 等)は c-2 で、まだ double を返す。**
;;
;; *read-default-float-format* の既定は <double-float> なので、
;; 接尾辞なしのリテラル(1.5)は double である。

;;; --- 1. 加算: 型の組み合わせ(左右両方向) ---
(assert-equal '<fixnum>       (%%class-name (class-of (+ 1 2))))
(assert-equal '<single-float> (%%class-name (class-of (+ 1 1.5f0))))
(assert-equal '<single-float> (%%class-name (class-of (+ 1.5f0 1))))
(assert-equal '<single-float> (%%class-name (class-of (+ 1.5f0 1.5f0))))
(assert-equal '<double-float> (%%class-name (class-of (+ 1.5f0 1.5d0))))
(assert-equal '<double-float> (%%class-name (class-of (+ 1.5d0 1.5f0))))
(assert-equal '<double-float> (%%class-name (class-of (+ 1.5d0 1.5d0))))
(assert-equal '<double-float> (%%class-name (class-of (+ 1 1.5d0))))
(assert-equal '<double-float> (%%class-name (class-of (+ 1.5d0 1))))
(assert-equal '<single-float> (%%class-name (class-of (+ (expt 2 100) 1.5f0))))
(assert-equal '<single-float> (%%class-name (class-of (+ 1.5f0 (expt 2 100)))))
(assert-equal '<double-float> (%%class-name (class-of (+ (expt 2 100) 1.5d0))))

;;; --- 2. 減算 ---
(assert-equal '<fixnum>       (%%class-name (class-of (- 3 2))))
(assert-equal '<single-float> (%%class-name (class-of (- 1 1.5f0))))
(assert-equal '<single-float> (%%class-name (class-of (- 1.5f0 1))))
(assert-equal '<single-float> (%%class-name (class-of (- 1.5f0 1.5f0))))
(assert-equal '<double-float> (%%class-name (class-of (- 1.5f0 1.5d0))))
(assert-equal '<double-float> (%%class-name (class-of (- 1.5d0 1.5f0))))
(assert-equal '<double-float> (%%class-name (class-of (- 1.5d0 1.5d0))))
(assert-equal '<single-float> (%%class-name (class-of (- (expt 2 100) 1.5f0))))
;;; 単項マイナスは引数の型をそのまま保つ
(assert-equal '<single-float> (%%class-name (class-of (- 1.5f0))))
(assert-equal '<double-float> (%%class-name (class-of (- 1.5d0))))
(assert-equal '<fixnum>       (%%class-name (class-of (- 1))))
(assert-equal t (= -1.5f0 (- 1.5f0)))
(assert-equal t (= -1.5d0 (- 1.5d0)))

;;; --- 3. 乗算 ---
(assert-equal '<fixnum>       (%%class-name (class-of (* 2 3))))
(assert-equal '<single-float> (%%class-name (class-of (* 2 1.5f0))))
(assert-equal '<single-float> (%%class-name (class-of (* 1.5f0 2))))
(assert-equal '<single-float> (%%class-name (class-of (* 1.5f0 1.5f0))))
(assert-equal '<double-float> (%%class-name (class-of (* 1.5f0 1.5d0))))
(assert-equal '<double-float> (%%class-name (class-of (* 1.5d0 1.5f0))))
(assert-equal '<double-float> (%%class-name (class-of (* 1.5d0 1.5d0))))
(assert-equal '<single-float> (%%class-name (class-of (* (expt 2 100) 1.5f0))))

;;; --- 4. 除算 ---
(assert-equal '<single-float> (%%class-name (class-of (/ 3 1.5f0))))
(assert-equal '<single-float> (%%class-name (class-of (/ 1.5f0 3))))
(assert-equal '<single-float> (%%class-name (class-of (/ 3.0f0 1.5f0))))
(assert-equal '<double-float> (%%class-name (class-of (/ 3.0f0 1.5d0))))
(assert-equal '<double-float> (%%class-name (class-of (/ 3.0d0 1.5f0))))
(assert-equal '<double-float> (%%class-name (class-of (/ 3.0d0 1.5d0))))
;;; 整数どうしの / は従来どおり(切り捨て整数除算。本作業で触らない)
(assert-equal '<fixnum> (%%class-name (class-of (/ 6 3))))
(assert-equal 2 (/ 6 3))

;;; --- 5. 値そのもの ---
(assert-equal t (= 3.0f0 (+ 1.5f0 1.5f0)))
(assert-equal t (= 3.0d0 (+ 1.5f0 1.5d0)))
(assert-equal t (= 2.5f0 (+ 1 1.5f0)))
(assert-equal t (= 0.0f0 (- 1.5f0 1.5f0)))
(assert-equal t (= 2.25f0 (* 1.5f0 1.5f0)))
(assert-equal t (= 2.0f0 (/ 3.0f0 1.5f0)))

;;; --- 6. n 引数版と 2 引数版で型が一致する ---
(assert-equal '<single-float> (%%class-name (class-of (+ 1.5f0 1.5f0 1.5f0))))
(assert-equal '<double-float> (%%class-name (class-of (+ 1.5f0 1.5d0 1.5f0))))
(assert-equal '<single-float> (%%class-name (class-of (+ 1 2 3 1.5f0))))
(assert-equal '<single-float> (%%class-name (class-of (- 10.0f0 1.5f0 1.5f0))))
(assert-equal '<double-float> (%%class-name (class-of (- 10.0f0 1.5d0 1.5f0))))
(assert-equal '<single-float> (%%class-name (class-of (* 1.5f0 1.5f0 1.5f0))))
(assert-equal '<double-float> (%%class-name (class-of (* 1.5f0 1.5d0 1.5f0))))
(assert-equal '<single-float> (%%class-name (class-of (/ 8.0f0 2.0f0 2.0f0))))
(assert-equal '<double-float> (%%class-name (class-of (/ 8.0f0 2.0d0 2.0f0))))

;;; **n 引数は左から畳み込み、そこまでに見た形式へ毎回丸める。**
;;; したがって (+ a b c) と (+ (+ a b) c) は型も値も一致する。
(assert-equal t (= (+ 1.5f0 1.5f0 1.5f0) (+ (+ 1.5f0 1.5f0) 1.5f0)))
(assert-equal t (= (+ 1.5f0 1.5d0 1.5f0) (+ (+ 1.5f0 1.5d0) 1.5f0)))
(assert-equal t (= (* 1.5f0 1.5f0 1.5f0) (* (* 1.5f0 1.5f0) 1.5f0)))
(assert-equal t (= (- 10.0f0 1.5f0 1.5f0) (- (- 10.0f0 1.5f0) 1.5f0)))
;;; 2進で割り切れない値でも合成が一致すること(丸めの回数が合っている確認)
(assert-equal t (= (+ 0.1f0 0.1f0 0.1f0) (+ (+ 0.1f0 0.1f0) 0.1f0)))
(assert-equal t (= (* 0.1f0 0.1f0 0.1f0) (* (* 0.1f0 0.1f0) 0.1f0)))
(assert-equal '<single-float> (%%class-name (class-of (+ 0.1f0 0.1f0 0.1f0))))

;;; --- 7. 比較は「広いほうへ揃えてから」 ---
;;; 1.5 は両形式で正確に表せるので、実装が誤っていても通る
(assert-equal t (= 1.5f0 1.5d0))
;;; ★これが t なら double を single へ落としている(精度を捨てている)
(assert-equal nil (= 0.1f0 0.1d0))
;;; single の 0.1 を double へ広げた値は double の 0.1 より**大きい**
(assert-equal nil (< 0.1f0 0.1d0))
(assert-equal t   (> 0.1f0 0.1d0))
(assert-equal t   (>= 0.1f0 0.1d0))
(assert-equal nil (<= 0.1f0 0.1d0))
(assert-equal t   (/= 0.1f0 0.1d0))
;;; 逆向きも同じ結論になること
(assert-equal t   (< 0.1d0 0.1f0))
(assert-equal nil (> 0.1d0 0.1f0))

;;; 整数と float の比較は整数側を float へ広げる
(assert-equal t   (= 1 1.0f0))
(assert-equal t   (= 1 1.0d0))
(assert-equal t   (= 1.0f0 1))
(assert-equal t   (< 1 1.5f0))
(assert-equal t   (> 1.5f0 1))
(assert-equal nil (= 1 1.5f0))
(assert-equal t   (<= 2 2.0f0))
(assert-equal t   (>= 2 2.0f0))

;;; bignum と float の比較(現状の挙動を固定する)
(assert-equal t   (> (expt 2 100) 1.5f0))
(assert-equal t   (< 1.5f0 (expt 2 100)))

;;; --- 8. min / max は引数をそのまま返す(型昇格しない) ---
;;; ISLisp §19 の max/min は「引数のうち最大/最小のもの」を返す規定なので、
;;; 返り値は元の引数そのままでよい。本作業で変更していない。
(assert-equal '<single-float> (%%class-name (class-of (max 1 2.0f0))))
(assert-equal '<single-float> (%%class-name (class-of (min 2.0f0 1.5f0))))
(assert-equal '<fixnum>       (%%class-name (class-of (max 2 1.0f0))))
(assert-equal '<fixnum>       (%%class-name (class-of (min 1 2.0f0))))
(assert-equal t (= 2.0f0 (max 1 2.0f0)))
(assert-equal t (= 1 (min 1 2.0f0)))

;;; --- 9. abs は型を保つ ---
(assert-equal '<single-float> (%%class-name (class-of (abs -1.5f0))))
(assert-equal '<double-float> (%%class-name (class-of (abs -1.5d0))))
(assert-equal '<fixnum>       (%%class-name (class-of (abs -1))))
(assert-equal t (= 1.5f0 (abs -1.5f0)))

;;; --- 10. equal は型昇格しない(タグ一致を要求する) ---
;;; = は数値として比較し、equal は型も見る。混同しないこと。
(assert-equal nil (equal 1.5f0 1.5d0))
(assert-equal t   (equal 1.5f0 1.5f0))
(assert-equal t   (equal 1.5d0 1.5d0))
(assert-equal t   (= 1.5f0 1.5d0))
;;; 演算結果でも同じ
(assert-equal t   (equal 3.0f0 (+ 1.5f0 1.5f0)))
(assert-equal nil (equal 3.0d0 (+ 1.5f0 1.5f0)))
(assert-equal t   (equal 3.0d0 (+ 1.5d0 1.5d0)))

;;; --- 11. 境界と特殊値(現状を固定する。挙動は変えていない) ---
;;; single の最大値どうしを足すと single の無限大になる
(defglobal *sf-inf* (+ *most-positive-single-float* *most-positive-single-float*))
(assert-equal '<single-float> (%%class-name (class-of *sf-inf*)))
(assert-equal t (> *sf-inf* *most-positive-single-float*))
;;; double へ広げても無限大のまま
(assert-equal t (> *sf-inf* *most-positive-double-float*))
;;; single の最大値 + double の最大値は double。double の範囲には収まらない
(assert-equal '<double-float>
              (%%class-name (class-of (+ *most-positive-single-float* *most-positive-double-float*))))
;;; single の最大値どうしの積も single の無限大
(assert-equal '<single-float>
              (%%class-name (class-of (* *most-positive-single-float* *most-positive-single-float*))))

;;; 0 と -0
(assert-equal t (= 1.5f0 (+ 1.5f0 0.0f0)))
(assert-equal t (= 1.5f0 (+ 1.5f0 -0.0f0)))
(assert-equal '<single-float> (%%class-name (class-of (* 0.0f0 -1.0f0))))
(assert-equal t (= 0.0f0 (* 0.0f0 -1.0f0)))

;;; fixnum の境界は従来どおり
(assert-equal '<bignum>       (%%class-name (class-of (+ *most-positive-fixnum* 1))))
(assert-equal '<single-float> (%%class-name (class-of (+ *most-positive-fixnum* 1.0f0))))
(assert-equal '<double-float> (%%class-name (class-of (+ *most-positive-fixnum* 1.0d0))))
(assert-equal '<fixnum>       (%%class-name (class-of (- *most-positive-fixnum* 1))))

;;; --- 12. 整数どうしの演算は1つも変わっていない ---
(assert-equal 3  (+ 1 2))
(assert-equal -1 (+ 1 -2))
(assert-equal -1 (- 1 2))
(assert-equal 6  (* 2 3))
(assert-equal 2  (/ 6 3))
(assert-equal 0  (+ 1 -1))
(assert-equal t  (bignump (* (expt 2 100) 2)))
(assert-equal t  (fixnump (+ 1 2)))

;;; --- 13. 数学関数 (c-2) ---
;;; c-1 の時点では数学関数はすべて double を返していた。
;;; c-2(documents/float-math-contagion.md)で入力の型を保つようになったので
;;; single を渡せば single が返る。詳しい表は
;;; test/lisp/float_math_contagion_test.lisp にある。
(assert-equal '<single-float> (%%class-name (class-of (sqrt 2.0f0))))
(assert-equal '<single-float> (%%class-name (class-of (exp 1.0f0))))
(assert-equal '<single-float> (%%class-name (class-of (log 2.0f0))))
(assert-equal '<single-float> (%%class-name (class-of (sin 1.0f0))))
;;; expt は %expt-integer の単位元が double リテラル(1.0)だったため
;;; 整数指数でも double になっていた。c-2 で単位元を base の型に合わせた
(assert-equal '<single-float> (%%class-name (class-of (expt 1.5f0 2))))
;;; float(FLOAT関数)は整数を *read-default-float-format* に従って変換する。
;;; 既定は <double-float> なので (float 2) は double
(assert-equal '<double-float> (%%class-name (class-of (float 2))))
(assert-equal '<single-float> (%%class-name (class-of (float 1.5f0))))
