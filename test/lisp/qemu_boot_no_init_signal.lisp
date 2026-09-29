;; test/lisp/qemu_boot_no_init_signal.lisp
;;
;; [P4] **このファイルは (load "src/lisp/init.lisp") を意図的に行わない。**
;; それがこの試験の要件である。他の boot-entry スクリプトを真似して
;; init.lisp を足さないこと。
;;
;; AOT フォーム(init_aot.lisp 由来)だけが走っている素のカーネル起動では、
;; make-instance / signal-condition は**ある**のに条件クラスは**1つも登録されて
;; いない**。os_signal_condition のフォールバック判定は前者しか見ていないため、
;; この状態で signal すると make-instance が (%find-class '<DOMAIN-ERROR>) に
;; nil を得て、(%%class-slots nil) が型検査の無いまま生の値を読み、その値を
;; length に渡す。P4 で length が非シーケンスを signal するようになったので、
;;   length -> signal -> make-instance -> %%class-slots -> length -> ...
;; の無限再帰になり、スタックガードページに当たって停止していた。
;;
;; os_signal_condition の深さ打ち切り(SIGNAL_MAX_DEPTH)がそれを止める。
;; **ここで固定するのは「戻ってくること」であって、戻り値そのものではない。**
;; 条件クラスが無い状態での戻り値は仕様の範囲外で、意味のある値にはならない。
;;
;; test_framework.lisp は使わない。あちらの assert-* は条件システム(init.lisp)を
;; 前提にしている箇所があり、この状態では黙って何も数えない。

(defglobal *s* (open-output-file "/9p/test-results.txt"))
(defglobal *pass* 0)
(defglobal *fail* 0)

(defun ck (label ok)
  (if ok
      (setq *pass* (+ *pass* 1))
    (progn
      (setq *fail* (+ *fail* 1))
      (format *s* "[NG] ~A~%" label)))
  ;; **1件ごとに書き出す。** 停止したときに「どこまで進んだか」が残らないと
  ;; 外からは何も分からない(-display none でコンソールは見えない)
  (format *s* "#ck ~A~%" label)
  (finish-output *s*))

;;; この試験の前提: 条件クラスが登録されていないこと
(ck "no-condition-class" (null (%find-class '<domain-error>)))
(ck "no-simple-error"    (null (%find-class '<simple-error>)))
;;; 対照: AOT が登録する組み込みクラスは引けること(前提が「全部壊れている」の
;;; ではなく「条件クラスだけ無い」ことを示す)
(ck "builtin-class-ok"   (not (null (%find-class '<number>))))

;;; ここから本体。**どれも「戻ってくる」ことだけを見る。**
;;; 無限再帰ならこの時点でスタックを食い潰して停止するので、
;;; 後続が1つでも走ればそれが合格の証拠になる。
(defglobal *r1* (length 5))              ; 非シーケンス -> domain-error を signal
(ck "length-returned" t)
(defglobal *r2* (elt '(1 2) 99))         ; 範囲外 -> program-error を signal
(ck "elt-returned" t)
(defglobal *r3* (subseq "abc" 1 99))     ; 範囲外 -> program-error を signal
(ck "subseq-returned" t)
(defglobal *r4* (div 1 0))               ; [P4-1] division-by-zero を signal
(ck "div-returned" t)

;;; **Lisp 側の (error ...) はここでは見ない。** あちらは init_aot.lisp の
;;; make-instance を Lisp から直接呼ぶ経路で、クラスが引けないまま
;;; (make-array <生のワード>) まで進んでしまう。これは P4 以前からそうで、
;;; 分岐元(feature/error-unwind @ 7453e77)に戻して同じ条件で走らせても
;;; ゲストが落ちることを確認済み。**P4 が持ち込んだものではない**ため、
;;; この PR では直さず、別途報告する。

;;; **深さ打ち切り(SIGNAL_MAX_DEPTH)は発動しないのが正しい。**
;;; os_signal_condition が make-instance を呼ぶ前にクラスの解決可否を見るので、
;;; 再帰はそもそも始まらない。打ち切りは最後の砦であって、常用の経路ではない。
;;; ここが 0 でなくなったら、クラス事前確認を素通りした経路が増えたということ
(ck "no-depth-overflow" (= (%%diag-signal-overflows) 0))

;;; 打ち切ったあとも処理系が動いていること
(ck "still-alive"   (= (+ 1 2) 3))
(ck "cons-alive"    (equal (list 1 2) '(1 2)))
(ck "gc-alive"      (= (length (subseq '(1 2 3 4 5) 1 4)) 3))

(format *s* "~%==== isiki tests: ~D passed, ~D failed ====~%" *pass* *fail*)
(close *s*)
