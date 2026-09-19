;; test/lisp/single_float_test.lisp
;;
;; single-float(タグ0x4の即値)と型階層拡張の回帰テスト。
;; documents/single-float.md
;;
;; ここで確かめるのは4つ:
;;   (a) single-float が即値として作れ、GC を跨いでも壊れないこと
;;   (b) 指数マーカー e/E・f/F・d/D と *read-default-float-format* が効くこと
;;   (d) print が既定の型に応じた接尾辞を出し、read/print で型が保存されること
;;   (e) 型階層(<fixnum>/<bignum>/<single-float>/<double-float> と別名2つ)
;;
;; **既知の中間状態**: 演算の型昇格はまだ入っていないので、
;; single 同士の演算結果も double になる((c) は別 PR)。
;; このファイルはその前提のうえで書いてある。

;;; --- 1. リーダ: 指数マーカーで型が決まる ---
;;; *read-default-float-format* の既定値は <double-float>(§8 と documents/single-float.md)
(assert-equal '<single-float> (%%class-name (class-of 1.5f0)))
(assert-equal '<single-float> (%%class-name (class-of 1.5F0)))
(assert-equal '<double-float> (%%class-name (class-of 1.5d0)))
(assert-equal '<double-float> (%%class-name (class-of 1.5D0)))
;; e/E と「指数部が無い」は既定に従う(= いまは double)
(assert-equal '<double-float> (%%class-name (class-of 1.5e0)))
(assert-equal '<double-float> (%%class-name (class-of 1.5E0)))
(assert-equal '<double-float> (%%class-name (class-of 1.5)))

;;; 小数点が無くても指数部があれば float
(assert-equal '<single-float> (%%class-name (class-of 3f10)))
(assert-equal '<double-float> (%%class-name (class-of 3d10)))
;;; 3d10 は 30000000000 ちょうど。3f10 は binary32 の刻み(この大きさでは2048)に
;;; 丸められるので**ちょうどにはならない**。単精度の精度そのものの話であり、
;;; リーダの不具合ではない
(assert-equal t (= 3d10 30000000000))
(assert-equal t (< (abs (- 3f10 30000000000)) 4096))

;;; 値そのもの。**2進で表せる値どうしでしか = は成り立たない**
;;; (0.0125 のような値は single と double で丸め先が違う)
(assert-equal t (= 1.5f0 1.5d0))
(assert-equal t (= 1.5f0 1.5))
(assert-equal t (= -1.25f-1 -0.125d0))
(assert-equal t (= 0.5f0 0.5))
(assert-equal t (= -2.0f0 -2))
;;; 2進で表せない値は single と double でずれる(仕様どおりの挙動)
(assert-equal nil (= 0.1f0 0.1d0))
(assert-equal t (< (abs (- 0.1f0 0.1d0)) 0.000001))

;;; --- 2. リーダ: 接尾辞としては扱わない(指数部が無ければシンボル) ---
;;; 'f'/'d' は**指数マーカー**であって接尾辞ではない。桁が続かなければ float でない。
;;; read-error ではなくシンボルへ戻すので、"f1" のような既存のシンボル名も読める。
(assert-equal t (symbolp '3.14f))
(assert-equal t (symbolp '3f))
(assert-equal t (symbolp '3.14d))
(assert-equal t (symbolp 'f1))
(assert-equal t (symbolp 'd2))

;;; --- 3. 型述語 ---
(assert-equal t   (floatp 1.5f0))
(assert-equal t   (floatp 1.5d0))
(assert-equal t   (numberp 1.5f0))
(assert-equal nil (integerp 1.5f0))
(assert-equal nil (fixnump 1.5f0))
(assert-equal t   (%%single-float-p 1.5f0))
(assert-equal nil (%%single-float-p 1.5d0))
(assert-equal nil (%%single-float-p 1))

;;; --- 4. 型階層 ---
(assert-equal '<fixnum>       (%%class-name (class-of 1)))
(assert-equal '<fixnum>       (%%class-name (class-of -1)))
(assert-equal '<bignum>       (%%class-name (class-of (expt 2 100))))
(assert-equal '<single-float> (%%class-name (class-of 1.5f0)))
(assert-equal '<double-float> (%%class-name (class-of 1.5d0)))

(assert-equal t (subclassp (%find-class '<fixnum>)       (%find-class '<integer>)))
(assert-equal t (subclassp (%find-class '<bignum>)       (%find-class '<integer>)))
(assert-equal t (subclassp (%find-class '<integer>)      (%find-class '<number>)))
(assert-equal t (subclassp (%find-class '<single-float>) (%find-class '<float>)))
(assert-equal t (subclassp (%find-class '<double-float>) (%find-class '<float>)))
(assert-equal t (subclassp (%find-class '<float>)        (%find-class '<number>)))

;;; typep は <integer> / <float> でも従来どおり通る(階層で吸収される)
(assert-equal t   (typep 1 '<integer>))
(assert-equal t   (typep (expt 2 100) '<integer>))
(assert-equal t   (typep 1.5f0 '<float>))
(assert-equal t   (typep 1.5d0 '<float>))
(assert-equal t   (typep 1.5f0 '<number>))
(assert-equal nil (typep 1.5f0 '<integer>))
(assert-equal nil (typep 1 '<float>))
(assert-equal t   (typep 1.5f0 '<single-float>))
(assert-equal nil (typep 1.5f0 '<double-float>))

;;; --- 5. <short-float> / <long-float> は別名(サブクラスではない) ---
;;; 同一のクラスオブジェクトを2つ目の名前で登録している。
;;; サブクラスにすると class-of が決して返さない到達不能クラスができてしまう。
(assert-equal t (eq (%find-class '<short-float>) (%find-class '<single-float>)))
(assert-equal t (eq (%find-class '<long-float>)  (%find-class '<double-float>)))
;;; class-of は別名を**決して返さない**
(assert-equal nil (eq (%%class-name (class-of 1.5f0)) '<short-float>))
(assert-equal nil (eq (%%class-name (class-of 1.5d0)) '<long-float>))
;;; 別名経由の typep も同じクラスなので通る
(assert-equal t (typep 1.5f0 '<short-float>))
(assert-equal t (typep 1.5d0 '<long-float>))

;;; --- 6. 定数(すべてC側から導出。Lisp側に数値リテラルは無い) ---
(assert-equal 576460752303423487  *most-positive-fixnum*)
(assert-equal -576460752303423487 *most-negative-fixnum*)
(assert-equal t (fixnump *most-positive-fixnum*))
(assert-equal t (fixnump *most-negative-fixnum*))
(assert-equal t (bignump (+ *most-positive-fixnum* 1)))
(assert-equal t (bignump (- *most-negative-fixnum* 1)))
(assert-equal '<fixnum> (%%class-name (class-of *most-positive-fixnum*)))
(assert-equal '<bignum> (%%class-name (class-of (+ *most-positive-fixnum* 1))))
(assert-equal '<fixnum> (%%class-name (class-of *most-negative-fixnum*)))
(assert-equal '<bignum> (%%class-name (class-of (- *most-negative-fixnum* 1))))

(assert-equal '<single-float> (%%class-name (class-of *most-positive-single-float*)))
(assert-equal '<single-float> (%%class-name (class-of *most-negative-single-float*)))
(assert-equal '<double-float> (%%class-name (class-of *most-positive-double-float*)))
(assert-equal '<double-float> (%%class-name (class-of *most-negative-double-float*)))
;;; 負側は正側の符号反転ちょうど
(assert-equal t (= *most-negative-single-float* (- 0 *most-positive-single-float*)))
(assert-equal t (= *most-negative-double-float* (- 0 *most-positive-double-float*)))
;;; 単精度の最大値は倍精度の最大値より小さい
(assert-equal t (< *most-positive-single-float* *most-positive-double-float*))
;;; ISLisp の *most-positive-float* は double 側の端(従来どおり)
(assert-equal t (= *most-positive-float* *most-positive-double-float*))
(assert-equal t (= *most-negative-float* *most-negative-double-float*))

;;; --- 7. print と read/print の往復 ---
;;; 既定(<double-float>)では double は接尾辞なし、single は "f0" が付く
(defun sf-print-to-string (x)
  (let ((s (create-string-output-stream)))
    (format s "~S" x)
    (get-output-stream-string s)))

(assert-equal "1.5"    (sf-print-to-string 1.5d0))
(assert-equal "1.5f0"  (sf-print-to-string 1.5f0))
(assert-equal "-2.5"   (sf-print-to-string -2.5d0))
(assert-equal "-2.5f0" (sf-print-to-string -2.5f0))
;;; E表記になる値では、接尾辞ではなく指数マーカーそのものが型を表す
(assert-equal "1.5E20" (sf-print-to-string 1.5d20))
(assert-equal "1.5f20" (sf-print-to-string 1.5f20))
;;; single は「同じ値に読み戻せる最短の桁数」で出す。doubleと同じ17桁で出すと
;;; binary32 の丸め誤差が見えてしまう(documents/single-float.md 2-4)
(assert-equal "0.5f0"  (sf-print-to-string 0.5f0))
(assert-equal "4.0f0"  (sf-print-to-string 4.0f0))

;;; 出力した文字列を読み直すと型が保存される
(defun sf-read-back (x)
  (%%class-name (class-of (parse-number (sf-print-to-string x)))))
(assert-equal '<single-float> (sf-read-back 1.5f0))
(assert-equal '<double-float> (sf-read-back 1.5d0))
(assert-equal '<double-float> (sf-read-back 1.5d20))
(assert-equal '<single-float> (sf-read-back 1.5f20))
(assert-equal '<single-float> (sf-read-back 0.5f0))
(assert-equal t (= 1.5d0 (parse-number (sf-print-to-string 1.5d0))))
(assert-equal t (= 1.5f0 (parse-number (sf-print-to-string 1.5f0))))

;;; --- 8. *read-default-float-format* を切り替えて両方向 ---
;;; 動的変数は g_dynamic_bindings という単一のグローバルにあり、プロセス間で
;;; 共有される(documents/type-system-survey.md §4)。ここでは同一プロセス内で
;;; 切り替えて戻す。
(assert-equal '<double-float>
              (dynamic-let ((*read-default-float-format* '<double-float>))
                (%%class-name (class-of (parse-number "1.5")))))
(assert-equal '<single-float>
              (dynamic-let ((*read-default-float-format* '<single-float>))
                (%%class-name (class-of (parse-number "1.5")))))
;;; 明示マーカーは既定より強い
(assert-equal '<double-float>
              (dynamic-let ((*read-default-float-format* '<single-float>))
                (%%class-name (class-of (parse-number "1.5d0")))))
(assert-equal '<single-float>
              (dynamic-let ((*read-default-float-format* '<double-float>))
                (%%class-name (class-of (parse-number "1.5f0")))))
;;; print 側も同じ変数を見る(既定が <single-float> なら double に "d0" が付く)
(assert-equal "1.5d0"
              (dynamic-let ((*read-default-float-format* '<single-float>))
                (sf-print-to-string 1.5d0)))
(assert-equal "1.5"
              (dynamic-let ((*read-default-float-format* '<single-float>))
                (sf-print-to-string 1.5f0)))
;;; 抜けたら元に戻っている
(assert-equal "1.5"   (sf-print-to-string 1.5d0))
(assert-equal "1.5f0" (sf-print-to-string 1.5f0))

;;; --- 9. GC を跨いでも値が壊れない ---
;;; GC がタグ 0x4 をポインタとして追いかけていれば、生のビットパターンを
;;; ヒープオブジェクトとして複製して word0 に転送先を書き込む(静かなヒープ破壊)。
;;; ここで値が変わるか、落ちる。
;;;
;;; GC を直接呼ぶプリミティブは無いので、ゴミを大量に作って**実際に GC が起きた
;;; ことを %%gc-collect-count で確認**してから値を見る(発火していなければ
;;; このテストは何も検証していないことになるため、前提条件として確かめる)。
(defglobal *sf-kept* (list 0.5f0 -0.5f0 1.0f0 3.25f0 -7.75f0))
(defglobal *sf-gc-before* (%%gc-collect-count))

;; ゴミを捨てる量は**ヒープ総量から決める**。固定の回数にすると、
;; QEMU_MEM を増やしたときに一度もGCが起きず、このテストが何も見なくなる
;; (実際 256M で 100000回 では1回も発火しなかった)。
;; aot_leaf_gc_test と同じ考え方で、ヒープ総量の数倍を確保する。
;;
;; **再帰ではなくwhileで回す。** 末尾再帰の最適化に依存すると、
;; 効かない経路(インタプリタ/JIT)でスタックを食い潰してしまう。
;; 1回あたりの確保を大きく(1KB)して、回数そのものは現実的な範囲に収める。
(defglobal *sf-churn-chunk* 1024)
(defun sf-churn-bytes (total)
  (let ((made 0))
    (while (< made total)
      (create-string *sf-churn-chunk* #\a)
      (setq made (+ made *sf-churn-chunk*)))
    t))
(sf-churn-bytes (* 3 (%%heap-total-bytes)))

(defglobal *sf-gc-after* (%%gc-collect-count))
(assert-equal t (> *sf-gc-after* *sf-gc-before*))

(assert-equal t (= (elt *sf-kept* 0)  0.5f0))
(assert-equal t (= (elt *sf-kept* 1) -0.5f0))
(assert-equal t (= (elt *sf-kept* 2)  1.0f0))
(assert-equal t (= (elt *sf-kept* 3)  3.25f0))
(assert-equal t (= (elt *sf-kept* 4) -7.75f0))
(assert-equal '<single-float> (%%class-name (class-of (elt *sf-kept* 0))))
(assert-equal '<single-float> (%%class-name (class-of (elt *sf-kept* 4))))

;;; --- 9b. abs は float の型を保存する(decompose へ落とさない) ---
;;; single-float は即値なので、integer 用の decompose へ落とすと生のビットパターンを
;;; アドレスとして読んで落ちる。abs は is_float で分岐する
(assert-equal '<single-float> (%%class-name (class-of (abs -1.5f0))))
(assert-equal '<double-float> (%%class-name (class-of (abs -1.5d0))))
(assert-equal t (= 1.5f0 (abs -1.5f0)))
(assert-equal t (= 1.5f0 (abs 1.5f0)))
(assert-equal t (= 0.0f0 (abs -0.0f0)))

;;; --- 10. 既知の中間状態: 演算の型昇格はまだ入っていない ---
;;; single 同士の演算結果は **double** になる。(c) が入ったら single へ変わる。
;;; ここを直すのは次の作業であり、このアサーションはそのとき更新する。
(assert-equal '<double-float> (%%class-name (class-of (+ 1.5f0 1.5f0))))
(assert-equal '<double-float> (%%class-name (class-of (* 2.0f0 2.0f0))))
(assert-equal t (= 3.0d0 (+ 1.5f0 1.5f0)))
;;; 比較は型をまたいでも値で行われる
(assert-equal t (= 1.5f0 1.5d0))
(assert-equal t (< 1.0f0 1.5d0))
(assert-equal t (> 2.0d0 1.5f0))
