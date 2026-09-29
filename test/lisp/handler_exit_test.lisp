;; test/lisp/handler_exit_test.lisp
;;
;; [P3] with-handler の脱出先。
;;
;; **非継続コンディションでハンドラが正常 return したときの行き先を変えた。**
;;   変更前: %abort-top-level(外側の with-handler も block も飛び越えてトップレベルへ)
;;   変更後: そのハンドラを張った with-handler まで戻り、ハンドラの戻り値が
;;           with-handler 式の値になる
;;
;; spec:6929 は「The consequences are undefined if the handler returns normally」
;; なのでどちらも仕様違反ではない。外側へ譲りたいハンドラは spec:6932 のとおり
;; 自分で signal-condition を呼ぶ。
;;
;; *handlers* の要素は (handler-function . 脱出タグ) のペアになった
;; (init.lisp の with-handler / init_aot.lisp の signal-condition)。
;;
;; 各項目はインタプリタ版(トップレベルのフォーム)と JIT 版(defun に包んで
;; %%za-compiled-p で確認)の両方で見る。isiki_test.lisp / isiki_test_jit.lisp と
;; 同じ流儀だが、あちらは仕様の Example 専用(かつ生成物)なのでファイルを分ける。

(defglobal *he-trace* nil)
(defun he-note (x) (setq *he-trace* (cons x *he-trace*)) x)

;;; ---------------------------------------------------------------------------
;;; 1. ハンドラが正常 return すると、その with-handler の値になる
;;; ---------------------------------------------------------------------------

(defglobal *he-r1* (with-handler (lambda (c) 'handled) (error "he1")))
(assert-equal 'handled *he-r1*)

(defun he-basic () (with-handler (lambda (c) 'handled) (error "he1-jit")))
(assert-equal t (%%za-compiled-p (function he-basic)))
(assert-equal 'handled (he-basic))

;;; ---------------------------------------------------------------------------
;;; 2. with-handler の後ろのフォームが続けて評価される
;;; ---------------------------------------------------------------------------
;;
;; **ここが今回の変更の要。** 変更前はこのトップレベルフォーム全体が
;; %abort-top-level で打ち切られ、'after まで到達しなかった。

(setq *he-trace* nil)
(defglobal *he-r2*
  (progn (he-note 'before)
         (with-handler (lambda (c) (he-note 'handler) 'handled)
           (error "he2")
           (he-note 'not-reached))
         (he-note 'after)))
(assert-equal '(BEFORE HANDLER AFTER) (reverse *he-trace*))
(assert-equal 'after *he-r2*)

(defun he-continues ()
  (progn (he-note 'before)
         (with-handler (lambda (c) (he-note 'handler) 'handled)
           (error "he2-jit")
           (he-note 'not-reached))
         (he-note 'after)))
(assert-equal t (%%za-compiled-p (function he-continues)))
(setq *he-trace* nil)
(assert-equal 'after (he-continues))
(assert-equal '(BEFORE HANDLER AFTER) (reverse *he-trace*))

;;; ---------------------------------------------------------------------------
;;; 3. 入れ子: 内側ハンドラが正常 return すると内側の with-handler で止まる
;;; ---------------------------------------------------------------------------
;;
;; 外側ハンドラは呼ばれず、外側の body は内側 with-handler の**後ろから続く**。

(setq *he-trace* nil)
(defglobal *he-r3*
  (with-handler (lambda (c) (he-note 'outer-handler) 'outer)
    (with-handler (lambda (c) (he-note 'inner-handler) 'inner)
      (error "he3"))
    (he-note 'after-inner-wh)
    'outer-body-done))
(assert-equal '(INNER-HANDLER AFTER-INNER-WH) (reverse *he-trace*))
(assert-equal 'outer-body-done *he-r3*)

(defun he-nested-inner ()
  (with-handler (lambda (c) (he-note 'outer-handler) 'outer)
    (with-handler (lambda (c) (he-note 'inner-handler) 'inner)
      (error "he3-jit"))
    (he-note 'after-inner-wh)
    'outer-body-done))
(assert-equal t (%%za-compiled-p (function he-nested-inner)))
(setq *he-trace* nil)
(assert-equal 'outer-body-done (he-nested-inner))
(assert-equal '(INNER-HANDLER AFTER-INNER-WH) (reverse *he-trace*))

;;; ---------------------------------------------------------------------------
;;; 4. 内側ハンドラが signal-condition を呼べば外側ハンドラへ譲れる(spec:6932)
;;; ---------------------------------------------------------------------------
;;
;; 外側ハンドラが正常 return したら、止まるのは**外側の** with-handler。
;; 内側 with-handler の後ろ('after-inner-wh)は走らない。

(setq *he-trace* nil)
(defglobal *he-r4*
  (with-handler (lambda (c) (he-note 'outer-handler) 'outer)
    (with-handler (lambda (c) (he-note 'inner-handler) (signal-condition c nil))
      (error "he4"))
    (he-note 'after-inner-wh)
    'outer-body-done))
(assert-equal '(INNER-HANDLER OUTER-HANDLER) (reverse *he-trace*))
(assert-equal 'outer *he-r4*)

(defun he-defer ()
  (with-handler (lambda (c) (he-note 'outer-handler) 'outer)
    (with-handler (lambda (c) (he-note 'inner-handler) (signal-condition c nil))
      (error "he4-jit"))
    (he-note 'after-inner-wh)
    'outer-body-done))
(assert-equal t (%%za-compiled-p (function he-defer)))
(setq *he-trace* nil)
(assert-equal 'outer (he-defer))
(assert-equal '(INNER-HANDLER OUTER-HANDLER) (reverse *he-trace*))

;;; ---------------------------------------------------------------------------
;;; 5. 脱出タグは**動的な出現ごと**に別でなければならない
;;; ---------------------------------------------------------------------------
;;
;; 同じ with-handler フォームへ再帰的に入る。内側ハンドラが signal-condition で
;; 外側へ譲り、外側ハンドラが正常 return する。
;;
;; **タグをマクロ展開時に1つだけ作って焼き込むと、この throw が内側の catch に
;; 捕まる**(catch は最も内側の一致を拾う)。そうなると内側の with-handler が
;; 値を返し、外側の body が 'after-inner から続いてしまう。
;; 実行時 gensym なら外側の catch まで素通しになり、'after-inner は走らない。

(defun he-rec (n)
  (with-handler (lambda (c) (if (> n 0) (list 'handled-at n) (signal-condition c nil)))
    (if (> n 0)
        (progn (he-rec (- n 1)) (he-note 'after-inner) 'outer-body-done)
        (error "he-rec"))))
(assert-equal t (%%za-compiled-p (function he-rec)))

(setq *he-trace* nil)
(assert-equal '(HANDLED-AT 1) (he-rec 1))
(assert-equal nil (reverse *he-trace*))

;;; ---------------------------------------------------------------------------
;;; 6. unwind-protect の cleanup は走り、*handlers* は元に戻る
;;; ---------------------------------------------------------------------------

(assert-equal t (null (dynamic *handlers*)))

(defglobal *he-cleanup* nil)
(defglobal *he-r6*
  (with-handler (lambda (c) 'handled)
    (unwind-protect (error "he6") (setq *he-cleanup* 'ran))))
(assert-equal 'handled *he-r6*)
(assert-equal 'ran *he-cleanup*)
;; **脱出しても *handlers* は積みっぱなしにならない**(with-handler の unwind-protect)
(assert-equal t (null (dynamic *handlers*)))

(defun he-unwind ()
  (setq *he-cleanup* nil)
  (with-handler (lambda (c) 'handled)
    (unwind-protect (error "he6-jit") (setq *he-cleanup* 'ran-jit))))
(assert-equal t (%%za-compiled-p (function he-unwind)))
(assert-equal 'handled (he-unwind))
(assert-equal 'ran-jit *he-cleanup*)
(assert-equal t (null (dynamic *handlers*)))

;; 入れ子でも積みっぱなしにならない
(assert-equal 'outer
  (with-handler (lambda (c) 'outer)
    (with-handler (lambda (c) (signal-condition c nil))
      (error "he6b"))))
(assert-equal t (null (dynamic *handlers*)))

;;; ---------------------------------------------------------------------------
;;; 7. 変えていない経路
;;; ---------------------------------------------------------------------------

;; ハンドラが return-from で脱出する従来の使い方
(assert-equal 'caught
  (block he-b (with-handler (lambda (c) (return-from he-b 'caught)) (error "he7"))))

;; continuable + ハンドラが正常 return → signal-condition の戻り値になる(従来どおり)
(assert-equal 101
  (+ 1 (with-handler (lambda (c) 100) (signal-condition (make-instance '<condition>) t))))

;; continue-condition(従来どおり)
(assert-equal 'resumed
  (with-handler (lambda (c) (continue-condition c 'resumed)) (cerror "cont" "he7c")))

;; ignore-errors(ハンドラが return-from で脱出するので従来どおり)
(assert-equal nil (ignore-errors (error "he7d")))
(assert-equal 3 (ignore-errors 1 2 3))

;; ハンドラ無しは従来どおりトップレベルへ abort する。
;; **このトップレベルフォームは打ち切られる**ので、足跡で確かめる
(setq *he-trace* nil)
(progn (he-note 'before) (error "he7e") (he-note 'after))
(assert-equal '(BEFORE) (reverse *he-trace*))
(assert-equal t (null (dynamic *handlers*)))

;;; ---------------------------------------------------------------------------
;;; 8. ハンドラ自身が throw/return-from 以外で脱出しても *handlers* は戻る
;;; ---------------------------------------------------------------------------
;;
;; ハンドラの中でさらにエラーが起きた場合。内側の signal-condition は
;; *handlers* を (cdr handlers) にしてからハンドラを呼ぶので、
;; ハンドラ内のエラーは**自分自身では捕まらず**外側へ行く(spec:6923 の注記)。

(setq *he-trace* nil)
(defglobal *he-r8*
  (with-handler (lambda (c) (he-note 'outer-handler) 'outer-handled)
    (with-handler (lambda (c) (he-note 'inner-handler) (error "from inner handler"))
      (error "he8"))))
(assert-equal '(INNER-HANDLER OUTER-HANDLER) (reverse *he-trace*))
(assert-equal 'outer-handled *he-r8*)
(assert-equal t (null (dynamic *handlers*)))
