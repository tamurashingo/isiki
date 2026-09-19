;; test/lisp/float_math_contagion_test.lisp
;;
;; 数学関数の型昇格(c-2)の回帰テスト。documents/float-math-contagion.md
;;
;; 規則:
;;   single 入力 → single 出力
;;   double 入力 → double 出力
;;   整数 入力   → *read-default-float-format* に従う
;;
;; 内部計算は double(FPU)で行い、最後に結果の型へ丸める。
;; したがって single の結果は「double で計算してから単精度へ落とした値」になる。
;;
;; *read-default-float-format* の既定は <double-float> なので、
;; 接尾辞なしのリテラル(1.5)と整数入力の結果はどちらも double である。

;;; --- 1. 1引数の数学関数: 入力の型を保つ ---
(assert-equal '<single-float> (%%class-name (class-of (sqrt 2.0f0))))
(assert-equal '<double-float> (%%class-name (class-of (sqrt 2.0d0))))
(assert-equal '<double-float> (%%class-name (class-of (sqrt 2))))

(assert-equal '<single-float> (%%class-name (class-of (exp 1.0f0))))
(assert-equal '<double-float> (%%class-name (class-of (exp 1.0d0))))
(assert-equal '<double-float> (%%class-name (class-of (exp 1))))

(assert-equal '<single-float> (%%class-name (class-of (log 2.0f0))))
(assert-equal '<double-float> (%%class-name (class-of (log 2.0d0))))
(assert-equal '<double-float> (%%class-name (class-of (log 2))))

(assert-equal '<single-float> (%%class-name (class-of (sin 1.0f0))))
(assert-equal '<double-float> (%%class-name (class-of (sin 1.0d0))))
(assert-equal '<double-float> (%%class-name (class-of (sin 1))))

(assert-equal '<single-float> (%%class-name (class-of (cos 1.0f0))))
(assert-equal '<double-float> (%%class-name (class-of (cos 1.0d0))))
(assert-equal '<double-float> (%%class-name (class-of (cos 1))))

;;; Lisp 側の合成関数も、土台の関数が型を保てば自動的に保たれる
(assert-equal '<single-float> (%%class-name (class-of (tan 1.0f0))))
(assert-equal '<double-float> (%%class-name (class-of (tan 1.0d0))))
(assert-equal '<single-float> (%%class-name (class-of (atan 1.0f0))))
(assert-equal '<double-float> (%%class-name (class-of (atan 1.0d0))))
(assert-equal '<single-float> (%%class-name (class-of (asin 0.5f0))))
(assert-equal '<double-float> (%%class-name (class-of (asin 0.5d0))))
(assert-equal '<single-float> (%%class-name (class-of (acos 0.5f0))))
(assert-equal '<single-float> (%%class-name (class-of (sinh 1.0f0))))
(assert-equal '<double-float> (%%class-name (class-of (sinh 1.0d0))))
(assert-equal '<single-float> (%%class-name (class-of (cosh 1.0f0))))
(assert-equal '<single-float> (%%class-name (class-of (tanh 1.0f0))))
;;; atanh は係数が 0.5(double リテラル)だったため single が double へ落ちていた。
;;; 整数 2 での除算に直してある
(assert-equal '<single-float> (%%class-name (class-of (atanh 0.5f0))))
(assert-equal '<double-float> (%%class-name (class-of (atanh 0.5d0))))

;;; --- 2. 二項の数学関数は c-1 の型昇格規則そのまま ---
(assert-equal '<single-float> (%%class-name (class-of (atan2 1.0f0 1.0f0))))
(assert-equal '<double-float> (%%class-name (class-of (atan2 1.0f0 1.0d0))))
(assert-equal '<double-float> (%%class-name (class-of (atan2 1.0d0 1.0f0))))
(assert-equal '<double-float> (%%class-name (class-of (atan2 1.0d0 1.0d0))))
;;; 整数が混ざっても float 側に従う
(assert-equal '<single-float> (%%class-name (class-of (atan2 1.0f0 1))))
(assert-equal '<single-float> (%%class-name (class-of (atan2 1 1.0f0))))
;;; 両方整数なら既定に従う
(assert-equal '<double-float> (%%class-name (class-of (atan2 1 1))))

;;; --- 3. float 変換関数 ---
(assert-equal '<double-float> (%%class-name (class-of (float 2))))
(assert-equal '<single-float> (%%class-name (class-of (float 1.5f0))))
(assert-equal '<double-float> (%%class-name (class-of (float 1.5d0))))
(assert-equal t (= 2.0 (float 2)))

;;; --- 4. *read-default-float-format* の両方向 ---
;;; 整数入力のときだけ既定に従う。float 入力の型は既定に左右されない。
(assert-equal '<single-float>
              (dynamic-let ((*read-default-float-format* '<single-float>))
                (%%class-name (class-of (sqrt 2)))))
(assert-equal '<double-float>
              (dynamic-let ((*read-default-float-format* '<double-float>))
                (%%class-name (class-of (sqrt 2)))))
(assert-equal '<single-float>
              (dynamic-let ((*read-default-float-format* '<single-float>))
                (%%class-name (class-of (exp 1)))))
(assert-equal '<single-float>
              (dynamic-let ((*read-default-float-format* '<single-float>))
                (%%class-name (class-of (float 2)))))
(assert-equal '<single-float>
              (dynamic-let ((*read-default-float-format* '<single-float>))
                (%%class-name (class-of (atan2 1 1)))))
;;; float 入力は既定を変えても型が変わらない(入力の型が勝つ)
(assert-equal '<double-float>
              (dynamic-let ((*read-default-float-format* '<single-float>))
                (%%class-name (class-of (sqrt 2.0d0)))))
(assert-equal '<single-float>
              (dynamic-let ((*read-default-float-format* '<double-float>))
                (%%class-name (class-of (sqrt 2.0f0)))))
;;; 抜けたら元に戻っている
(assert-equal '<double-float> (%%class-name (class-of (sqrt 2))))

;;; --- 5. expt ---
;;; 整数のべき乗は整数のまま(従来どおり)
(assert-equal 1024 (expt 2 10))
(assert-equal '<fixnum> (%%class-name (class-of (expt 2 10))))
(assert-equal '<bignum> (%%class-name (class-of (expt 2 100))))
(assert-equal 10000 (expt -100 2))
;;; float の底は型が保たれる
(assert-equal '<single-float> (%%class-name (class-of (expt 2.0f0 10))))
(assert-equal '<double-float> (%%class-name (class-of (expt 2.0d0 10))))
(assert-equal t (= 1024.0f0 (expt 2.0f0 10)))
;;; **単位元が base の型に合っていること。** ここが c-1 からの持ち越しの要点。
;;; 以前は %expt-integer の単位元が 1.0(double)だったので double になっていた
(assert-equal '<single-float> (%%class-name (class-of (expt 2.0f0 0))))
(assert-equal '<double-float> (%%class-name (class-of (expt 2.0d0 0))))
(assert-equal t (= 1.0f0 (expt 2.0f0 0)))
(assert-equal '<fixnum> (%%class-name (class-of (expt 2 0))))
(assert-equal 1 (expt 2 0))
;;; 負指数は reciprocal 経由。型は保たれる
(assert-equal '<single-float> (%%class-name (class-of (expt 2.0f0 -1))))
(assert-equal t (= 0.5f0 (expt 2.0f0 -1)))
(assert-equal t (= -4.0 (expt -0.25 -1)))
;;; 指数が非整数 float のときは exp/log の合成。底が整数でも指数の型に従う
(assert-equal '<single-float> (%%class-name (class-of (expt 2 0.5f0))))
(assert-equal '<double-float> (%%class-name (class-of (expt 2 0.5d0))))
(assert-equal '<single-float> (%%class-name (class-of (expt 2.0f0 0.5f0))))
(assert-equal '<double-float> (%%class-name (class-of (expt 2.0f0 0.5d0))))
;;; 0 の float 乗は 0。型は指数に従う
(assert-equal '<single-float> (%%class-name (class-of (expt 0 1.5f0))))
(assert-equal '<double-float> (%%class-name (class-of (expt 0 1.5d0))))
(assert-equal t (= 0 (expt 0 1.5f0)))
(assert-equal 0 (expt 0 2))

;;; --- 6. 精度: 内部計算は double、最後に丸める ---
;;;
;;; **「double の結果を single へ落とした値」と直接比べる手段は無い。**
;;; double を single へ**狭める**演算子が存在しないためである
;;; (型昇格は広いほうへ寄せるので、(* double 1.0f0) は double のまま。
;;;  (float x) は float をそのまま返す。第2引数で形式を指定する拡張は
;;;  本作業では扱っていない。documents/float-math-contagion.md §3-6)。
;;;
;;; 代わりに、**single 経路と整数経路が同じ計算を通っていること**を見る。
;;; どちらも「double で計算してから結果の型へ丸める」実装なので一致するはず。
;;; もし single 入力のときだけ sqrtf/expf のような単精度版へ切り替えていたら、
;;; 最終ビットが食い違ってここが落ちる。
(assert-equal t (dynamic-let ((*read-default-float-format* '<single-float>))
                  (= (sqrt 2.0f0) (sqrt 2))))
(assert-equal t (dynamic-let ((*read-default-float-format* '<single-float>))
                  (= (exp 1.0f0) (exp 1))))
(assert-equal t (dynamic-let ((*read-default-float-format* '<single-float>))
                  (= (sin 1.0f0) (sin 1))))
(assert-equal t (dynamic-let ((*read-default-float-format* '<single-float>))
                  (= (cos 1.0f0) (cos 1))))
(assert-equal t (dynamic-let ((*read-default-float-format* '<single-float>))
                  (= (log 2.0f0) (log 2))))
;;; 同じことを double 側でも(整数経路と double 経路が一致すること)
(assert-equal t (= (sqrt 2.0d0) (sqrt 2)))
(assert-equal t (= (exp 1.0d0) (exp 1)))

;;; single の結果は double の結果と「値としては」違う(単精度へ丸めたぶん)
(assert-equal nil (= (sqrt 2.0f0) (sqrt 2.0d0)))
;;; ただし単精度の刻みのぶんしか違わない
(assert-equal t (< (abs (- (sqrt 2.0f0) (sqrt 2.0d0))) 0.000001))

;;; 正確に表せる入力では single でも double でも答えが一致する
(assert-equal t (= (sqrt 4.0f0) 2.0f0))
(assert-equal t (= (sqrt 4.0f0) (sqrt 4.0d0)))
(assert-equal t (= (exp 0.0f0) 1.0f0))
(assert-equal t (= (log 1.0f0) 0.0f0))
(assert-equal t (= (sin 0.0f0) 0.0f0))
(assert-equal t (= (cos 0.0f0) 1.0f0))

;;; 値そのものを記録に残す(直接比較できない以上、目視できる形で出しておく)
(format *isiki-test-stream*
        "[FMATH] sqrt2 single=~S double=~S / exp1 single=~S double=~S~%"
        (sqrt 2.0f0) (sqrt 2.0d0) (exp 1.0f0) (exp 1.0d0))
(finish-output *isiki-test-stream*)

;;; --- 7. 整数を返す関数は従来どおり ---
(assert-equal '<fixnum> (%%class-name (class-of (floor 1.5f0))))
(assert-equal '<fixnum> (%%class-name (class-of (floor 1.5d0))))
(assert-equal '<fixnum> (%%class-name (class-of (ceiling 1.5f0))))
(assert-equal '<fixnum> (%%class-name (class-of (truncate 1.5f0))))
(assert-equal '<fixnum> (%%class-name (class-of (round 1.5f0))))
(assert-equal '<fixnum> (%%class-name (class-of (isqrt 10))))
(assert-equal 1 (floor 1.5f0))
(assert-equal 2 (ceiling 1.5f0))
(assert-equal 1 (truncate 1.5f0))
(assert-equal 3 (isqrt 10))
;;; sqrt は完全平方なら整数を返す(従来どおり)
(assert-equal 2 (sqrt 4))
(assert-equal '<fixnum> (%%class-name (class-of (sqrt 4))))
;;; div/mod は ISLisp 上は整数演算。従来どおり整数を返す
(assert-equal '<fixnum> (%%class-name (class-of (div 7 2))))
(assert-equal '<fixnum> (%%class-name (class-of (mod 7 2))))

;;; --- 8. 値の正しさ(型が変わっても計算結果がずれていないこと) ---
(assert-equal t (< (abs (- (sqrt 2.0f0) 1.4142135)) 0.0001))
(assert-equal t (< (abs (- (exp 1.0f0) 2.7182817)) 0.0001))
(assert-equal t (< (abs (- (log 2.7182817f0) 1.0)) 0.0001))
(assert-equal t (< (abs (- (sin 0.0f0) 0.0)) 0.0001))
(assert-equal t (< (abs (- (cos 0.0f0) 1.0)) 0.0001))
(assert-equal t (< (abs (- (atan2 1.0f0 1.0f0) 0.7853981)) 0.0001))
(assert-equal t (< (abs (- (expt 2.0f0 10) 1024.0)) 0.0001))
(assert-equal t (< (abs (- (expt 2 0.5f0) 1.4142135)) 0.0001))

;;; --- 9. quotient / reciprocal は / の型昇格を継承する ---
(assert-equal '<single-float> (%%class-name (class-of (quotient 1.0f0 2.0f0))))
(assert-equal '<double-float> (%%class-name (class-of (quotient 1.0f0 2.0d0))))
(assert-equal '<fixnum>       (%%class-name (class-of (quotient 4 2))))
;;; 割り切れない整数どうしは float。型は既定に従う(float 経由のため)
(assert-equal '<double-float> (%%class-name (class-of (quotient 1 2))))
(assert-equal '<single-float> (%%class-name (class-of (reciprocal 2.0f0))))
