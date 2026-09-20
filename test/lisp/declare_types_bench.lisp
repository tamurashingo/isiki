;; test/lisp/declare_types_bench.lisp
;;
;; declare (Phase 4a-2) のコストを **コンパイル時と実行時に分けて** 計測する。
;;
;; 本 Phase では declare は記録されるだけで生成コードを変えないので、
;;   - コンパイル時間は **増える**(宣言の走査と符号化が入るため)
;;   - 実行時間は **変わらないはず**(生成コードが同一のため)
;; というのが期待される形である。後者が崩れていたら、それは
;; 「型情報を実行時の表現に持ち込んでしまった」ことを意味する(指示書 §1 違反)。
;;
;; 最適化を入れた後は実行時側に差が出るようになる。そのときの基準値でもある。
;;
;; 時間は tick(get-internal-real-time)。**100 tick = 1秒**
;; (src/c/clock.c の TICKS_PER_SECOND)。
;;
;; ベンチなので make test-qemu には載せない。
;;   make test-qemu-milestone MILESTONE=test/lisp/qemu_boot_declare_bench.lisp

(defglobal *dtb-env* (%%current-environment))

;;; ---------------------------------------------------------------------------
;;; コンパイル時間だけを測る
;;;
;;; 同じ defun フォームを n 回 eval する。**評価されるのは defun そのもの**
;;; なので、測っているのは eval_defun + za_try_compile_defun の時間だけで、
;;; 定義された関数は一度も呼ばれない。
;;; 名前も本体も固定し、declare の有無と型だけを変える。
;;; ---------------------------------------------------------------------------

(defun dtb-compile-ticks (form n)
  (let ((t0 (get-internal-real-time)))
    (while (> n 0)
      (%%eval-in-environment form *dtb-env*)
      (setq n (- n 1)))
    (- (get-internal-real-time) t0)))

(defun dtb-report-compile (label form n)
  (let ((imm0 (%%imm-space-used-bytes)))
    (let ((ticks (dtb-compile-ticks form n)))
      (format *isiki-test-stream*
              "[DTBENCH] compile ~A n=~D ticks=~D imm-delta=~D~%"
              label n ticks (- (%%imm-space-used-bytes) imm0))
      (finish-output *isiki-test-stream*)
      ticks)))

;; n はコンパイル1回が数ミリ秒であることを見込んだ値。Immobilized Space は
;; 同じ名前で再定義しても前のコードページを解放しないため、大きくしすぎると
;; 枯渇する(imm-delta を一緒に出しているのはそれを見張るため)。
;; imm-delta の実測は 822 byte/関数。Immobilized Space は 16MB で、同じ名前で
;; 再定義しても前のコードページは解放されないため、**5種 × n 個**が上限に効く。
;; n=1000 なら 5000 関数 ≒ 4.1MB で安全。n=200 では 40 tick しか出ず、
;; 1 tick = 10ms の分解能に対して差が埋もれた(実測で符号が負になった)。
(defglobal *dtb-compile-n* 1000)

(defglobal *dtb-c-none*
  (dtb-report-compile "宣言なし        "
    '(defun dtb-tmp (x) (+ (* x x) x)) *dtb-compile-n*))
(defglobal *dtb-c-fix*
  (dtb-report-compile "<fixnum>        "
    '(defun dtb-tmp (x) (declare (type <fixnum> x)) (+ (* x x) x)) *dtb-compile-n*))
(defglobal *dtb-c-sf*
  (dtb-report-compile "<single-float>  "
    '(defun dtb-tmp (x) (declare (type <single-float> x)) (+ (* x x) x)) *dtb-compile-n*))
(defglobal *dtb-c-df*
  (dtb-report-compile "<double-float>  "
    '(defun dtb-tmp (x) (declare (type <double-float> x)) (+ (* x x) x)) *dtb-compile-n*))
;; 未知の型名は *classes* を引いて外れる経路。解決できる場合より速いか遅いかを見る
(defglobal *dtb-c-unk*
  (dtb-report-compile "<unknown>       "
    '(defun dtb-tmp (x) (declare (type <no-such-class> x)) (+ (* x x) x)) *dtb-compile-n*))

(format *isiki-test-stream*
        "[DTBENCH] compile 差分: fixnum=~D single=~D double=~D unknown=~D (宣言なし比)~%"
        (- *dtb-c-fix* *dtb-c-none*) (- *dtb-c-sf* *dtb-c-none*)
        (- *dtb-c-df* *dtb-c-none*) (- *dtb-c-unk* *dtb-c-none*))
(finish-output *isiki-test-stream*)

;;; ---------------------------------------------------------------------------
;;; JIT コンパイル済み関数の実行時間だけを測る
;;;
;;; 型ごとに同じ形の数値計算ループ(自己再帰、末尾呼び出し)を回す。
;;; declare あり版となし版は **本体が完全に同一** で、宣言の有無だけが違う。
;;; ---------------------------------------------------------------------------

;; --- <fixnum> ---
(defun dtb-fix-d (n acc)
  (declare (type <fixnum> n) (type <fixnum> acc))
  (if (= n 0) acc (dtb-fix-d (- n 1) (+ acc (* n 3)))))
(defun dtb-fix-n (n acc)
  (if (= n 0) acc (dtb-fix-n (- n 1) (+ acc (* n 3)))))
;; **対照群。宣言なし同士で、本体も引数も完全に同じ。名前だけが違う。**
;; 生成コードが同じでも Immobilized Space 上の配置アドレスは関数ごとに違うため、
;; キャッシュライン境界やブランチ予測の当たり方が変わる。ここに出る差が
;; 「同一条件でも出てしまうノイズの大きさ」であり、宣言あり/なしの差は
;; **これと比べて初めて意味を持つ**。
(defun dtb-fix-c (n acc)
  (if (= n 0) acc (dtb-fix-c (- n 1) (+ acc (* n 3)))))

;; --- <single-float> ---
;; (* n 3.0f0) は fixnum × single → single(型昇格)。acc も single のまま回る
(defun dtb-sf-d (n acc)
  (declare (type <fixnum> n) (type <single-float> acc))
  (if (= n 0) acc (dtb-sf-d (- n 1) (+ acc (* n 3.0f0)))))
(defun dtb-sf-n (n acc)
  (if (= n 0) acc (dtb-sf-n (- n 1) (+ acc (* n 3.0f0)))))
(defun dtb-sf-c (n acc)
  (if (= n 0) acc (dtb-sf-c (- n 1) (+ acc (* n 3.0f0)))))

;; --- <double-float> ---
;; double は即値にできずヒープを確保するため、GC が回る。gc= の値も一緒に出す
(defun dtb-df-d (n acc)
  (declare (type <fixnum> n) (type <double-float> acc))
  (if (= n 0) acc (dtb-df-d (- n 1) (+ acc (* n 3.0d0)))))
(defun dtb-df-n (n acc)
  (if (= n 0) acc (dtb-df-n (- n 1) (+ acc (* n 3.0d0)))))
(defun dtb-df-c (n acc)
  (if (= n 0) acc (dtb-df-c (- n 1) (+ acc (* n 3.0d0)))))

;; **全部 JIT に乗っていること。** 乗っていない関数を測っても意味がない
(assert-equal t (%%za-compiled-p (function dtb-fix-d)))
(assert-equal t (%%za-compiled-p (function dtb-fix-n)))
(assert-equal t (%%za-compiled-p (function dtb-sf-d)))
(assert-equal t (%%za-compiled-p (function dtb-sf-n)))
(assert-equal t (%%za-compiled-p (function dtb-df-d)))
(assert-equal t (%%za-compiled-p (function dtb-df-n)))
(assert-equal t (%%za-compiled-p (function dtb-fix-c)))
(assert-equal t (%%za-compiled-p (function dtb-sf-c)))
(assert-equal t (%%za-compiled-p (function dtb-df-c)))

;; 宣言は記録されている(コンパイラまで届いている)こと
(assert-equal '(<FIXNUM> <FIXNUM>) (%%declared-types-of 'dtb-fix-d))
(assert-equal '(<FIXNUM> <SINGLE-FLOAT>) (%%declared-types-of 'dtb-sf-d))
(assert-equal '(<FIXNUM> <DOUBLE-FLOAT>) (%%declared-types-of 'dtb-df-d))
(assert-equal nil (%%declared-types-of 'dtb-fix-n))

;; **生成コードの長さが宣言の有無で一致すること。**
;; 本 Phase の主張「生成コードは変わらない」を実測で固定する。**これがベンチより
;; 確実な証拠である** — 長さが同じなら、実行時間の差は原理的に測定ノイズになる。
;; バイト列そのものは自己参照アドレス(関数ごとに配置が違う)を含むため一致しない。
(assert-equal (%%disasm-code-len (function dtb-fix-n)) (%%disasm-code-len (function dtb-fix-d)))
(assert-equal (%%disasm-code-len (function dtb-sf-n))  (%%disasm-code-len (function dtb-sf-d)))
(assert-equal (%%disasm-code-len (function dtb-df-n))  (%%disasm-code-len (function dtb-df-d)))
(format *isiki-test-stream* "[DTBENCH] code-len fixnum=~D single=~D double=~D (宣言あり/なしで一致)~%"
        (%%disasm-code-len (function dtb-fix-d))
        (%%disasm-code-len (function dtb-sf-d))
        (%%disasm-code-len (function dtb-df-d)))
(finish-output *isiki-test-stream*)

;; **宣言の有無で結果が変わらないこと。** 差が出たら計測以前の問題である
(assert-equal (dtb-fix-n 1000 0) (dtb-fix-d 1000 0))
(assert-equal (dtb-sf-n 1000 0.0f0) (dtb-sf-d 1000 0.0f0))
(assert-equal (dtb-df-n 1000 0.0d0) (dtb-df-d 1000 0.0d0))

;; 1回分の計測。tick と、その窓で走った GC 回数を返す
(defun dtb-run-once (f n init)
  (let ((t0 (get-internal-real-time)) (gc0 (%%gc-collect-count)))
    (funcall f n init)
    (list (- (get-internal-real-time) t0) (- (%%gc-collect-count) gc0))))

;; **3回走らせて最小値を採る。** 1 tick = 10ms と粗いうえ、ホスト側の負荷と
;; GC の発火位置で数 tick 揺れる(実測で「宣言ありのほうが速い」が出た)。
;; 最小値は「邪魔が入らなかった回」に相当するので、ノイズに強い。
(defun dtb-report-run (label f n init)
  (let ((a (dtb-run-once f n init))
        (b (dtb-run-once f n init))
        (c (dtb-run-once f n init)))
    (let ((best (min (car a) (min (car b) (car c)))))
      (format *isiki-test-stream* "[DTBENCH] run ~A n=~D ticks=~D (~D ~D ~D) gc=~D~%"
              label n best (car a) (car b) (car c)
              (+ (car (cdr a)) (+ (car (cdr b)) (car (cdr c)))))
      (finish-output *isiki-test-stream*)
      best)))

(defglobal *dtb-run-n* 1000000)
;; double は1演算ごとにヒープを確保するので桁を落とす(GC 込みの時間になる)
(defglobal *dtb-run-n-df* 200000)

(defglobal *dtb-r-fix-c* (dtb-report-run "<fixnum>       対照(なし)" (function dtb-fix-c) *dtb-run-n* 0))
(defglobal *dtb-r-fix-n* (dtb-report-run "<fixnum>       宣言なし" (function dtb-fix-n) *dtb-run-n* 0))
(defglobal *dtb-r-fix-d* (dtb-report-run "<fixnum>       宣言あり" (function dtb-fix-d) *dtb-run-n* 0))
(defglobal *dtb-r-sf-c*  (dtb-report-run "<single-float> 対照(なし)" (function dtb-sf-c) *dtb-run-n* 0.0f0))
(defglobal *dtb-r-sf-n*  (dtb-report-run "<single-float> 宣言なし" (function dtb-sf-n) *dtb-run-n* 0.0f0))
(defglobal *dtb-r-sf-d*  (dtb-report-run "<single-float> 宣言あり" (function dtb-sf-d) *dtb-run-n* 0.0f0))
(defglobal *dtb-r-df-c*  (dtb-report-run "<double-float> 対照(なし)" (function dtb-df-c) *dtb-run-n-df* 0.0d0))
(defglobal *dtb-r-df-n*  (dtb-report-run "<double-float> 宣言なし" (function dtb-df-n) *dtb-run-n-df* 0.0d0))
(defglobal *dtb-r-df-d*  (dtb-report-run "<double-float> 宣言あり" (function dtb-df-d) *dtb-run-n-df* 0.0d0))

(format *isiki-test-stream*
        "[DTBENCH] run 差分(宣言あり - 宣言なし): fixnum=~D single=~D double=~D~%"
        (- *dtb-r-fix-d* *dtb-r-fix-n*) (- *dtb-r-sf-d* *dtb-r-sf-n*)
        (- *dtb-r-df-d* *dtb-r-df-n*))
;; **これがノイズの基準。** 宣言の有無という違いが一切無い2関数の差。
;; 上の差分がこれと同じオーダーなら、宣言による実行時コストは検出できていない。
(format *isiki-test-stream*
        "[DTBENCH] run 対照(なし - なし、同一条件のノイズ): fixnum=~D single=~D double=~D~%"
        (- *dtb-r-fix-c* *dtb-r-fix-n*) (- *dtb-r-sf-c* *dtb-r-sf-n*)
        (- *dtb-r-df-c* *dtb-r-df-n*))
(finish-output *isiki-test-stream*)
