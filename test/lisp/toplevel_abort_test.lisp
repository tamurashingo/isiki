;; test/lisp/toplevel_abort_test.lisp
;;
;; [P2] トップレベルの整備。
;;
;;   1. %%eval-in-environment が内側に block %TOP-LEVEL を張らなくなったこと
;;      = with-environment の中のエラーが握り潰されなくなったこと
;;   2. エラーで打ち切られたフォームの中の switch-environment が巻き戻ること
;;   3. 正常終了したフォームの switch-environment は次のフォームに効くこと
;;   4. report-condition 経由の表示に使う %report-condition-string が
;;      期待どおりのメッセージを返すこと
;;
;; **このファイル自身が load 経由で評価される**ので、2 と 3 は load 側の
;; トップレベル(cc_load)の挙動をそのまま確かめていることになる。
;; REPL 側(os_repl_step)の同じ配線は test/c/repl_test.c で見る
;; (init.lisp をロードしないテストなので、そちらは %abort-top-level と同じ
;; 脱出をフォームに直接書いて C の配線だけを検証する)。

(defglobal *tl-trace* nil)
(defun tl-note (x) (setq *tl-trace* (cons x *tl-trace*)) x)

(defglobal *tl-env* (make-environment 'tl-env (%%global-environment)))
(defglobal *tl-env0* (%environment-name (%%current-environment)))

;;; ---------------------------------------------------------------------------
;;; 1. with-environment がエラーを握り潰さないこと
;;; ---------------------------------------------------------------------------
;;
;; P2 以前は %%eval-in-environment が os_eval_top_level を呼んでいたため、
;; 内側にもう 1 つ block %TOP-LEVEL が張られ、%abort-top-level がそこで止まっていた。
;; その結果 condition が with-environment 式の**値**になり、外側の progn が続いて
;; 'after まで評価されていた(documents/error-unwind-survey.md §3-3)。

(setq *tl-trace* nil)
;; **このトップレベルフォームは打ち切られる。** 値は残らないので、足跡で確かめる
(progn (tl-note 'before)
       (with-environment *tl-env* (tl-note 'in-body) (error "tl inside with-environment"))
       (tl-note 'after))
(assert-equal '(BEFORE IN-BODY) (reverse *tl-trace*))

;; 対照: with-environment を外した同じ形(P2 以前からこちらは正しく打ち切られていた)
(setq *tl-trace* nil)
(progn (tl-note 'before) (error "tl plain") (tl-note 'after))
(assert-equal '(BEFORE) (reverse *tl-trace*))

;; 対照: エラーが無ければ with-environment は最後まで走り、値も返る
(setq *tl-trace* nil)
(defglobal *tl-we-value* (progn (tl-note 'before)
                                 (with-environment *tl-env* (tl-note 'in-body) 'we-done)
                                 (tl-note 'after)))
(assert-equal '(BEFORE IN-BODY AFTER) (reverse *tl-trace*))
(assert-equal 'after *tl-we-value*)

;; with-environment の unwind-protect は脱出経路でも走る = 環境は戻っている
(assert-equal *tl-env0* (%environment-name (%%current-environment)))

;;; ---------------------------------------------------------------------------
;;; 2. 打ち切られたフォームの中の switch-environment は巻き戻る
;;; ---------------------------------------------------------------------------
;;
;; switch-environment は proc->env を**恒久的に**書き換えるプリミティブ
;; (with-environment と違って unwind-protect で戻さない)。やりかけのまま
;; 次のフォームへ持ち越さないよう、打ち切られたときだけ cc_load / os_repl_step が戻す。

(assert-equal *tl-env0* (%environment-name (%%current-environment)))
;; **このトップレベルフォームは打ち切られる**
(progn (switch-environment 'tl-env) (error "tl after switch"))
(assert-equal *tl-env0* (%environment-name (%%current-environment)))

;;; ---------------------------------------------------------------------------
;;; 3. 正常終了したフォームの switch-environment は次のフォームに効く
;;; ---------------------------------------------------------------------------

(switch-environment 'tl-env)
(assert-equal 'tl-env (%environment-name (%%current-environment)))
;; 後続のテストへ影響を残さないよう戻す
(switch-environment *tl-env0*)
(assert-equal *tl-env0* (%environment-name (%%current-environment)))

;;; ---------------------------------------------------------------------------
;;; 4. %report-condition-string(REPL の表示が使うメッセージ)
;;; ---------------------------------------------------------------------------

(assert-equal "boom 42"
              (%report-condition-string
                (make-instance '<simple-error> ':format-string "boom ~A" ':format-arguments '(42))))

;; C 側の signal_domain_error_for_class は expected-class に**クラスオブジェクト**を
;; 入れる(os_resolve_class の戻り値)。report-condition は ~A で出すので
;; #<CLASS <NUMBER>> という表記になる。**実機で実際に出る文字列**をそのまま固定する
;; (init_test.lisp:456 はクラス名シンボルを渡す版を見ているので、両方が固定される)
(assert-equal "5 is not of expected class #<CLASS <NUMBER>>"
              (%report-condition-string
                (make-instance '<domain-error> ':object 5 ':expected-class (%find-class '<number>))))

(assert-equal "5 is not of expected class <NUMBER>"
              (%report-condition-string
                (make-instance '<domain-error> ':object 5 ':expected-class '<number>)))

;; クラス名しか持たない condition は既定メソッドでクラス名だけを出す
(assert-equal "<CONTROL-ERROR>"
              (%report-condition-string (make-instance '<control-error>)))

;; **表示の途中でさらにエラーが起きたら nil を返す**(C 側はそこで
;; #<INSTANCE-OF ...> の表示へ落とす)。report-condition を壊して確かめる
(defglobal *tl-orig-report* (function report-condition))
(defun report-condition (condition stream) (error "tl report broke"))
(assert-equal nil (%report-condition-string (make-instance '<control-error>)))
(defun report-condition (condition stream) (funcall *tl-orig-report* condition stream))
;; 戻せていること
(assert-equal "<CONTROL-ERROR>" (%report-condition-string (make-instance '<control-error>)))

;;; ---------------------------------------------------------------------------
;;; 5. 打ち切りと「condition を値として返しただけ」は別物
;;; ---------------------------------------------------------------------------
;;
;; 戻り値だけでは区別が付かないので os_eval_top_level_ex が out_aborted で返す。
;; Lisp 側からは「後続が走るかどうか」で見える。

(setq *tl-trace* nil)
(defglobal *tl-value-not-abort*
  (progn (tl-note 'before)
         (make-instance '<simple-error> ':format-string "not signaled" ':format-arguments nil)
         (tl-note 'after)))
(assert-equal '(BEFORE AFTER) (reverse *tl-trace*))
(assert-equal 'after *tl-value-not-abort*)
