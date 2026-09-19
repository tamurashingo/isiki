;; test/lisp/float_contagion_bench.lisp
;;
;; c-1(float の型昇格)の効果を測る。documents/float-contagion.md 6章。
;;
;; **眼目は速度ではなくヒープ確保である。** single-float はタグ0x4の即値なので
;; 結果を包むヒープ確保が要らない。double は MAGIC_FLOAT の instance なので
;; 1演算ごとに32byte確保する。その差が single 即値化の効果そのものである。
;;
;; 計測の作法は既存ドキュメントの教訓に従う:
;;   - **同一ブート内で比較する。** 実行間に20%以上のドリフトがある
;;     (documents/bench-pinned-nil.md)
;;   - **被測定以外の条件を完全に揃える。** 改善Aでは初期値の不揃いが誤った
;;     結論を生んだ(documents/type-system-survey.md §7-4)。ここでは
;;     single/double/混在の3シナリオで**ループの形も回数も初期値の大きさも
;;     同じ**にし、リテラルの型だけを変える
;;   - **レンジを記録する。** 重なったら「差が出なかった」と報告する
;;   - **%%za-compiled-p で JIT に乗っていることを確認してから測る**
;;
;; abi_bench.lisp と同じく、heap-delta は計測窓中に GC が発火すると意味を失う
;; ので gc-delta=0 のときだけ bytes/call を有効な値として出す。

;; --- 計測窓 ---------------------------------------------------------------
;; loop-fn を1回だけ funcall する(funcall 自体のオーバーヘッドは n で割れば
;; 無視できる)。時刻・ヒープ・GC回数を窓の前後で取る。
(defun fcb-measure-once (loop-fn n)
  (let ((heap-before (%%heap-used-bytes))
        (gc-before (%%gc-collect-count))
        (t-before (get-internal-real-time)))
    (funcall loop-fn n)
    (list (- (get-internal-real-time) t-before)
          (- (%%heap-used-bytes) heap-before)
          (- (%%gc-collect-count) gc-before))))

(defun fcb-report (label result n)
  (let ((elapsed (car result))
        (heap-delta (car (cdr result)))
        (gc-delta (car (cdr (cdr result)))))
    (if (= gc-delta 0)
        (format *isiki-test-stream*
                "[FCBENCH] ~A: n=~D ticks=~D heap-delta=~D bytes/call=~D gc-delta=0~%"
                label n elapsed heap-delta (div heap-delta n))
        (format *isiki-test-stream*
                "[FCBENCH] ~A: n=~D ticks=~D heap-delta=~D gc-delta=~D (GC発火のためbytes/callは無効)~%"
                label n elapsed heap-delta gc-delta))
    (finish-output *isiki-test-stream*)
    elapsed))

;; --- シナリオ --------------------------------------------------------------
;; 3つとも**まったく同じ形**。変えるのは加算する定数の型だけ。
;; 初期値の大きさも揃える(1.0 は3形式すべてで正確に表せる)。

(defun fcb-loop-single (n)
  (let ((i 0) (s 1.0f0))
    (while (< i n)
      (setq s (+ s 1.0f0))
      (setq i (+ i 1)))
    s))

(defun fcb-loop-double (n)
  (let ((i 0) (s 1.0d0))
    (while (< i n)
      (setq s (+ s 1.0d0))
      (setq i (+ i 1)))
    s))

;; 混在: 累算器は single だが加える側が double なので、1回目以降 s は double に
;; なる。**double 相当の確保になるはず**(型昇格が効いていることの裏取り)
(defun fcb-loop-mixed (n)
  (let ((i 0) (s 1.0f0))
    (while (< i n)
      (setq s (+ s 1.0d0))
      (setq i (+ i 1)))
    s))

;; 対照: float を一切使わない fixnum 版。ループそのもの(while/setq/比較)の
;; コストを切り分けるための基準値
(defun fcb-loop-fixnum (n)
  (let ((i 0) (s 1))
    (while (< i n)
      (setq s (+ s 1))
      (setq i (+ i 1)))
    s))

;; --- JIT に乗っていることの確認 --------------------------------------------
;; 乗っていなければインタプリタ経路(必ず cons を作る)を測ることになり、
;; 「確保ゼロ」の主張が成り立たない
(assert-equal t (%%za-compiled-p (function fcb-loop-single)))
(assert-equal t (%%za-compiled-p (function fcb-loop-double)))
(assert-equal t (%%za-compiled-p (function fcb-loop-mixed)))
(assert-equal t (%%za-compiled-p (function fcb-loop-fixnum)))

;; --- 結果の型が期待どおりであることの確認 ----------------------------------
(assert-equal '<single-float> (%%class-name (class-of (fcb-loop-single 10))))
(assert-equal '<double-float> (%%class-name (class-of (fcb-loop-double 10))))
(assert-equal '<double-float> (%%class-name (class-of (fcb-loop-mixed 10))))
(assert-equal '<fixnum>       (%%class-name (class-of (fcb-loop-fixnum 10))))

;; --- 1回あたりの確保量 ------------------------------------------------------
;; **固定オーバーヘッドを差し引く。** funcall の引数 cons や %%heap-used-bytes 自身が
;; 1回の計測につき数十byte使うので、`(assert-equal 0 ...)` は成り立たない
;; (実測で48byteあった)。n を変えて2回測り、**差分から1回あたりを出す**。
(defun fcb-heap-delta (loop-fn n)
  (let ((before (%%heap-used-bytes)))
    (funcall loop-fn n)
    (- (%%heap-used-bytes) before)))

(defun fcb-gc-delta (loop-fn n)
  (let ((before (%%gc-collect-count)))
    (funcall loop-fn n)
    (- (%%gc-collect-count) before)))

;; n を 500 → 1500 へ増やしたときの増分 ÷ 1000 = 1回あたりの確保バイト数
(defun fcb-bytes-per-call (loop-fn)
  (let ((d1 (fcb-heap-delta loop-fn 500))
        (d2 (fcb-heap-delta loop-fn 1500)))
    (div (- d2 d1) 1000)))

;; 計測窓中に GC が発火すると heap-delta が減少方向へ振れて意味を失う。
;; この規模では起きないことを前提条件として確かめる(abi_bench.lisp と同じ作法)
(assert-equal 0 (fcb-gc-delta (function fcb-loop-single) 1500))
(assert-equal 0 (fcb-gc-delta (function fcb-loop-double) 1500))
(assert-equal 0 (fcb-gc-delta (function fcb-loop-fixnum) 1500))

(defglobal *fcb-bpc-single* (fcb-bytes-per-call (function fcb-loop-single)))
(defglobal *fcb-bpc-double* (fcb-bytes-per-call (function fcb-loop-double)))
(defglobal *fcb-bpc-mixed*  (fcb-bytes-per-call (function fcb-loop-mixed)))
(defglobal *fcb-bpc-fixnum* (fcb-bytes-per-call (function fcb-loop-fixnum)))

(format *isiki-test-stream*
        "[FCBENCH] bytes/call: single=~D double=~D mixed=~D fixnum=~D~%"
        *fcb-bpc-single* *fcb-bpc-double* *fcb-bpc-mixed* *fcb-bpc-fixnum*)
(finish-output *isiki-test-stream*)

;; **これは観測ログではなく回帰テストである。**
;; single 経路に cons や instance の確保が戻ってきたら落ちる。
(assert-equal 0 *fcb-bpc-single*)
(assert-equal 0 *fcb-bpc-fixnum*)
;; double は MAGIC_FLOAT の instance(32byte)を毎回作る。この差が効果そのもの
(assert-equal 32 *fcb-bpc-double*)
(assert-equal 32 *fcb-bpc-mixed*)

;; --- 速度 --------------------------------------------------------------------
;; tick は 100Hz(1tick = 10ms)なので、n=2000 では JIT 経路が ticks=0 になり
;; 分解能が足りない。1秒前後になる n まで上げる。
;; **同一ブート内で2往復し、レンジを記録する。重なったら「差が出なかった」と報告する。**
;; double 側はこの n で GC が発火するが、**確保圧そのものがコストなので
;; それを含めた値が知りたい値である**(gc-delta も一緒に出す)。
(defglobal *fcb-n* 200000)

(defglobal *fcb-s1* (fcb-report "single (1)" (fcb-measure-once (function fcb-loop-single) *fcb-n*) *fcb-n*))
(defglobal *fcb-d1* (fcb-report "double (1)" (fcb-measure-once (function fcb-loop-double) *fcb-n*) *fcb-n*))
(defglobal *fcb-m1* (fcb-report "mixed  (1)" (fcb-measure-once (function fcb-loop-mixed) *fcb-n*) *fcb-n*))
(defglobal *fcb-f1* (fcb-report "fixnum (1)" (fcb-measure-once (function fcb-loop-fixnum) *fcb-n*) *fcb-n*))

(defglobal *fcb-s2* (fcb-report "single (2)" (fcb-measure-once (function fcb-loop-single) *fcb-n*) *fcb-n*))
(defglobal *fcb-d2* (fcb-report "double (2)" (fcb-measure-once (function fcb-loop-double) *fcb-n*) *fcb-n*))
(defglobal *fcb-m2* (fcb-report "mixed  (2)" (fcb-measure-once (function fcb-loop-mixed) *fcb-n*) *fcb-n*))
(defglobal *fcb-f2* (fcb-report "fixnum (2)" (fcb-measure-once (function fcb-loop-fixnum) *fcb-n*) *fcb-n*))

(format *isiki-test-stream*
        "[FCBENCH] ticks range: single=~D..~D double=~D..~D mixed=~D..~D fixnum=~D..~D~%"
        (min *fcb-s1* *fcb-s2*) (max *fcb-s1* *fcb-s2*)
        (min *fcb-d1* *fcb-d2*) (max *fcb-d1* *fcb-d2*)
        (min *fcb-m1* *fcb-m2*) (max *fcb-m1* *fcb-m2*)
        (min *fcb-f1* *fcb-f2*) (max *fcb-f1* *fcb-f2*))
(finish-output *isiki-test-stream*)

;; 速度そのものはアサーションにしない(実行間のドリフトが20%以上あるため。
;; documents/bench-pinned-nil.md)。レンジを出して人が読む。

(close (open-output-file "/9p/tmp/ckpt-1-float-contagion-bench.txt"))
