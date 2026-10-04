;; test/lisp/jit_limits_test.lisp
;;
;; JIT の「容量上限」ごとの境界テスト。za.c の各上限は超えると黙って
;; コンパイルを断念し、インタプリタへ落ちる(遅くなるだけで結果は正しい)。
;; 2026-10-04 に断念箇所 30 箇所すべてへ ZA_BAIL_LINE() を入れ、
;; %%DIAG-ZA-BAIL-AT で「どの上限で諦めたか」が外から読めるようにした。
;; 本ファイルはその計器の**陽性対照**を兼ねる(規則 8): 1 つ超えの式を
;; 食わせて、診断が確かにその上限の行を名指しすることを確かめる。
;;
;; ## 上限ちょうどは乗り、1 つ超えは乗らない
;;
;; 上限ちょうどの側を本ファイルに置くのは、既存テストが持っていない上限だけ。
;; 既に境界を持っている上限は、そのファイルを正とし、ここでは 1 つ超え側
;; (= 診断の陽性対照)だけを置く。重複して書かない。
;;
;;   上限                           値   上限ちょうどの正   本ファイル
;;   ZA_MAX_PARAMS                  16   (なし)             ちょうど+超え
;;   ZA_MAX_OPERANDS                16   (なし)             ちょうど+超え
;;   ZA_MAX_NLX_DEPTH                8   (なし)             ちょうど+超え
;;   ZA_MAX_FLET_BINDINGS            4   (なし)             ちょうど+超え
;;   ZA_MAX_TAGBODY_TAGS            16   (なし)             ちょうど+超え
;;   ZA_MAX_TAGBODY_FORMS           64   (なし)             ちょうど+超え
;;   ZA_MAX_TAGBODY_GOTOS_PER_TAG    8   (なし)             ちょうど+超え
;;   ZA_MAX_LET_DEPTH               16   za_test_ext8/12    超えのみ
;;   ZA_MAX_LOCALS_PER_LET           4   jit_frame_test     超えのみ
;;   ZA_MAX_CALL_DEPTH               4   za_test_ext15      超えのみ
;;   ZA_MAX_ARITH_DEPTH              4   za_test_ext16      超えのみ
;;   ZA_MAX_QQ_DEPTH                 6   za_test_ext18      超えのみ
;;   ZA_MAX_QQ_ELEMENTS             16   za_test_ext18      超えのみ
;;   ZA_MAX_COMPILE_NEST            60   jit_nest_depth     超えのみ
;;
;; ## ここに無い上限: ZA_MAX_LAMBDA_SLOTS(256)
;;
;; これだけは境界テストを**置かない**。他の 14 個は 1 つの defun の中で閉じた
;; 上限(コンパイルのたびに 0 から数え直す)だが、ZA_MAX_LAMBDA_SLOTS は
;; g_za_lambda_slots の大域プール(解放リスト付き)で、ブート中に積み上がる。
;; 境界を踏むテストは (1) 直前に何個 lambda を作ったかで結果が変わり、
;; (2) 踏んだあとプールを使い切って後続のテストを壊す。
;; 「境界を回帰テストにしてはいけない上限」として記録しておく
;; (同じ判断を documents/for-expansion.md の再帰深さ 33/34 でもしている)。
;;
;; ## 上限を上げたら落ちるのが正しい
;;
;; 定数を上げたら本ファイルは落ちる。それが正しい振る舞い。落ちたら
;; 「上限ちょうど/1 つ超え」の段数を新しい値に合わせて書き直すこと。黙って
;; 通ってしまうと、上限を上げたのに境界が動いていない(= 効いていない)ことに
;; 気付けない。
;;
;; ## BAIL-LINE: のコメント
;;
;; 行番号は za.c の該当行。tools/check_jit_bail_lines.sh が
;; 「その行に `[診断] 容量上限 <定数>` が実際に書かれているか」を za.c と
;; 照合する(make test に入っている)。za.c に行が入って番号がずれたら
;; QEMU を回す前にホスト側で落ちる。

(defun jlm-t () 1)
(defun jlm-inc (n) (+ n 1))

;;; --- ZA_MAX_PARAMS (16) ---
(defun jlm-params-ok (p1 p2 p3 p4 p5 p6 p7 p8 p9 p10 p11 p12 p13 p14 p15 p16) p1)
(assert-equal t (%%za-compiled-p (function jlm-params-ok)))
(assert-equal 1 (jlm-params-ok 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16))
(defun jlm-params-over (p1 p2 p3 p4 p5 p6 p7 p8 p9 p10 p11 p12 p13 p14 p15 p16 p17) p1)
(assert-equal nil (%%za-compiled-p (function jlm-params-over)))
(assert-equal 1144 (%%diag-za-bail-at 0))   ; BAIL-LINE: ZA_MAX_PARAMS
(assert-equal 1 (jlm-params-over 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17))

;;; --- ZA_MAX_OPERANDS ---
(defun jlm-operands-ok (x) (+ x 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15))
(assert-equal t (%%za-compiled-p (function jlm-operands-ok)))
(assert-equal 120 (jlm-operands-ok 0))
(defun jlm-operands-over (x) (+ x 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16))
(assert-equal nil (%%za-compiled-p (function jlm-operands-over)))
(assert-equal 2900 (%%diag-za-bail-at 0))   ; BAIL-LINE: ZA_MAX_OPERANDS
(assert-equal 136 (jlm-operands-over 0))

;;; --- ZA_MAX_NLX_DEPTH --- block は 1 段ずつ消費する。最内の catch が上限を見る
(defun jlm-nlx-ok (x) (block nil (block nil (block nil (block nil (block nil (block nil (block nil (catch (quote jc) x)))))))))
(assert-equal t (%%za-compiled-p (function jlm-nlx-ok)))
(assert-equal 5 (jlm-nlx-ok 5))
(defun jlm-nlx-over (x) (block nil (block nil (block nil (block nil (block nil (block nil (block nil (block nil (catch (quote jc) x))))))))))
(assert-equal nil (%%za-compiled-p (function jlm-nlx-over)))
(assert-equal 5408 (%%diag-za-bail-at 0))   ; BAIL-LINE: ZA_MAX_NLX_DEPTH
(assert-equal 5 (jlm-nlx-over 5))

;;; --- ZA_MAX_FLET_BINDINGS ---
(defun jlm-flet-ok (x) (flet ((f1 (y) y) (f2 (y) y) (f3 (y) y) (f4 (y) y)) (f1 x)))
(assert-equal t (%%za-compiled-p (function jlm-flet-ok)))
(assert-equal 7 (jlm-flet-ok 7))
(defun jlm-flet-over (x) (flet ((f1 (y) y) (f2 (y) y) (f3 (y) y) (f4 (y) y) (f5 (y) y)) (f1 x)))
(assert-equal nil (%%za-compiled-p (function jlm-flet-over)))
(assert-equal 6027 (%%diag-za-bail-at 0))   ; BAIL-LINE: ZA_MAX_FLET_BINDINGS
(assert-equal 7 (jlm-flet-over 7))

;;; --- ZA_MAX_TAGBODY_TAGS ---
(defun jlm-tbtags-ok (x) (progn (tagbody jt1 (car x) jt2 (car x) jt3 (car x) jt4 (car x) jt5 (car x) jt6 (car x) jt7 (car x) jt8 (car x) jt9 (car x) jt10 (car x) jt11 (car x) jt12 (car x) jt13 (car x) jt14 (car x) jt15 (car x) jt16 (car x)) x))
(assert-equal t (%%za-compiled-p (function jlm-tbtags-ok)))
(assert-equal (list 1 2) (jlm-tbtags-ok (list 1 2)))
(defun jlm-tbtags-over (x) (progn (tagbody jt1 (car x) jt2 (car x) jt3 (car x) jt4 (car x) jt5 (car x) jt6 (car x) jt7 (car x) jt8 (car x) jt9 (car x) jt10 (car x) jt11 (car x) jt12 (car x) jt13 (car x) jt14 (car x) jt15 (car x) jt16 (car x) jt17 (car x)) x))
(assert-equal nil (%%za-compiled-p (function jlm-tbtags-over)))
(assert-equal 6432 (%%diag-za-bail-at 0))   ; BAIL-LINE: ZA_MAX_TAGBODY_TAGS
(assert-equal (list 1 2) (jlm-tbtags-over (list 1 2)))

;;; --- ZA_MAX_TAGBODY_FORMS ---
(defun jlm-tbforms-ok (x) (progn (tagbody (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x)) x))
(assert-equal t (%%za-compiled-p (function jlm-tbforms-ok)))
(assert-equal (list 1 2) (jlm-tbforms-ok (list 1 2)))
(defun jlm-tbforms-over (x) (progn (tagbody (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x) (car x)) x))
(assert-equal nil (%%za-compiled-p (function jlm-tbforms-over)))
(assert-equal 6449 (%%diag-za-bail-at 0))   ; BAIL-LINE: ZA_MAX_TAGBODY_FORMS
(assert-equal (list 1 2) (jlm-tbforms-over (list 1 2)))

;;; --- ZA_MAX_TAGBODY_GOTOS_PER_TAG --- 同じタグへ飛ぶ go の本数
(defun jlm-tbgotos-ok (x) (progn (tagbody (if (> x 1000) (go jdone) nil) (if (> x 1000) (go jdone) nil) (if (> x 1000) (go jdone) nil) (if (> x 1000) (go jdone) nil) (if (> x 1000) (go jdone) nil) (if (> x 1000) (go jdone) nil) (if (> x 1000) (go jdone) nil) (if (> x 1000) (go jdone) nil) jdone) x))
(assert-equal t (%%za-compiled-p (function jlm-tbgotos-ok)))
(assert-equal 1 (jlm-tbgotos-ok 1))
(defun jlm-tbgotos-over (x) (progn (tagbody (if (> x 1000) (go jdone) nil) (if (> x 1000) (go jdone) nil) (if (> x 1000) (go jdone) nil) (if (> x 1000) (go jdone) nil) (if (> x 1000) (go jdone) nil) (if (> x 1000) (go jdone) nil) (if (> x 1000) (go jdone) nil) (if (> x 1000) (go jdone) nil) (if (> x 1000) (go jdone) nil) jdone) x))
(assert-equal nil (%%za-compiled-p (function jlm-tbgotos-over)))
(assert-equal 6568 (%%diag-za-bail-at 0))   ; BAIL-LINE: ZA_MAX_TAGBODY_GOTOS_PER_TAG
(assert-equal 1 (jlm-tbgotos-over 1))

;;; === ここから下は 1 つ超え側だけ(上限ちょうどは上の表のファイルが正) ===

;;; --- ZA_MAX_LET_DEPTH --- ちょうど(16)は za_test_ext8/za_test_ext12
(defun jlm-letdepth-over (x) (let ((v1 1)) (let ((v2 2)) (let ((v3 3)) (let ((v4 4)) (let ((v5 5)) (let ((v6 6)) (let ((v7 7)) (let ((v8 8)) (let ((v9 9)) (let ((v10 10)) (let ((v11 11)) (let ((v12 12)) (let ((v13 13)) (let ((v14 14)) (let ((v15 15)) (let ((v16 16)) (let ((v17 17)) x))))))))))))))))))
(assert-equal nil (%%za-compiled-p (function jlm-letdepth-over)))
(assert-equal 3662 (%%diag-za-bail-at 0))   ; BAIL-LINE: ZA_MAX_LET_DEPTH
(assert-equal 9 (jlm-letdepth-over 9))

;;; --- ZA_MAX_LOCALS_PER_LET --- ちょうど(4)は jit_frame_test
(defun jlm-letwidth-over (x) (let ((a x) (b 1) (c 2) (d 3) (e 4)) (+ a b c d e)))
(assert-equal nil (%%za-compiled-p (function jlm-letwidth-over)))
(assert-equal 3526 (%%diag-za-bail-at 0))   ; BAIL-LINE: ZA_MAX_LOCALS_PER_LET
(assert-equal 10 (jlm-letwidth-over 0))

;;; --- ZA_MAX_CALL_DEPTH --- ちょうど(4)は za_test_ext15
(defun jlm-calldepth-over (x) (jlm-inc (jlm-inc (jlm-inc (jlm-inc (jlm-inc x))))))
(assert-equal nil (%%za-compiled-p (function jlm-calldepth-over)))
(assert-equal 4413 (%%diag-za-bail-at 0))   ; BAIL-LINE: ZA_MAX_CALL_DEPTH
(assert-equal 15 (jlm-calldepth-over 10))

;;; --- ZA_MAX_ARITH_DEPTH --- ちょうど(4)は za_test_ext16
(defun jlm-arith-over (x) (+ 1 (+ 1 (+ 1 (+ 1 (+ 1 x))))))
(assert-equal nil (%%za-compiled-p (function jlm-arith-over)))
(assert-equal 2890 (%%diag-za-bail-at 0))   ; BAIL-LINE: ZA_MAX_ARITH_DEPTH
(assert-equal 15 (jlm-arith-over 10))

;;; --- ZA_MAX_QQ_DEPTH (6) --- ちょうど(6)は za_test_ext18
(defun jlm-qqdepth-over (a0 a1 a2 a3 a4 a5 a6) `(l0 ,a0 (l1 ,a1 (l2 ,a2 (l3 ,a3 (l4 ,a4 (l5 ,a5 (l6 ,a6))))))))
(assert-equal nil (%%za-compiled-p (function jlm-qqdepth-over)))
(assert-equal 4822 (%%diag-za-bail-at 0))   ; BAIL-LINE: ZA_MAX_QQ_DEPTH
(assert-equal '(l0 1 (l1 2 (l2 3 (l3 4 (l4 5 (l5 6 (l6 7))))))) (jlm-qqdepth-over 1 2 3 4 5 6 7))

;;; --- ZA_MAX_QQ_ELEMENTS --- ちょうど(16)は za_test_ext18
(defun jlm-qqelem-over (x) `(,x 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16))
(assert-equal nil (%%za-compiled-p (function jlm-qqelem-over)))
(assert-equal 4892 (%%diag-za-bail-at 0))   ; BAIL-LINE: ZA_MAX_QQ_ELEMENTS
(assert-equal '(0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16) (jlm-qqelem-over 0))

;;; --- ZA_MAX_COMPILE_NEST (60) --- ちょうど(59)は jit_nest_depth_test
;; 2026-10-04 までこの断念だけは行番号を記録していなかった(「ZA_BAIL_LINE は
;; この位置ではまだ未定義なので使えない」とコメントに書かれていた)。マクロの
;; 定義をファイル先頭へ移したので、他と同じく名指しできるようになった。
(defun jlm-nest-over () (if (jlm-t) 1 (if (jlm-t) 1 (if (jlm-t) 1 (if (jlm-t) 1 (if (jlm-t) 1 (if (jlm-t) 1 (if (jlm-t) 1 (if (jlm-t) 1 (if (jlm-t) 1 (if (jlm-t) 1 (if (jlm-t) 1 (if (jlm-t) 1 (if (jlm-t) 1 (if (jlm-t) 1 (if (jlm-t) 1 (if (jlm-t) 1 (if (jlm-t) 1 (if (jlm-t) 1 (if (jlm-t) 1 (if (jlm-t) 1 (if (jlm-t) 1 (if (jlm-t) 1 (if (jlm-t) 1 (if (jlm-t) 1 (if (jlm-t) 1 (if (jlm-t) 1 (if (jlm-t) 1 (if (jlm-t) 1 (if (jlm-t) 1 (if (jlm-t) 1 (if (jlm-t) 1 (if (jlm-t) 1 (if (jlm-t) 1 (if (jlm-t) 1 (if (jlm-t) 1 (if (jlm-t) 1 (if (jlm-t) 1 (if (jlm-t) 1 (if (jlm-t) 1 (if (jlm-t) 1 (if (jlm-t) 1 (if (jlm-t) 1 (if (jlm-t) 1 (if (jlm-t) 1 (if (jlm-t) 1 (if (jlm-t) 1 (if (jlm-t) 1 (if (jlm-t) 1 (if (jlm-t) 1 (if (jlm-t) 1 (if (jlm-t) 1 (if (jlm-t) 1 (if (jlm-t) 1 (if (jlm-t) 1 (if (jlm-t) 1 (if (jlm-t) 1 (if (jlm-t) 1 (if (jlm-t) 1 (if (jlm-t) 1 (if (jlm-t) 1 0)))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))
(assert-equal nil (%%za-compiled-p (function jlm-nest-over)))
(assert-equal 3907 (%%diag-za-bail-at 0))   ; BAIL-LINE: ZA_MAX_COMPILE_NEST
(assert-equal 1 (jlm-nest-over))

;;; --- 陰性対照: 上限に触らない defun は乗り、診断は何も記録しない ---
(defun jlm-ok-fn (x) (+ x 1))
(assert-equal t (%%za-compiled-p (function jlm-ok-fn)))
(assert-equal 0 (%%diag-za-bail-count))
(assert-equal 4 (jlm-ok-fn 3))
