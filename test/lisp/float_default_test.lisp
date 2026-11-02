;; test/lisp/float_default_test.lisp
;;
;; float の既定形式が**経路によってずれていないこと**を見張る。
;; documents/float-default.md
;;
;; *read-default-float-format* の既定は2箇所で決まる。
;;
;;   実行時    : runtime.h の DEFAULT_FLOAT_FORMAT_IS_SINGLE
;;               → %%default-float-format → init.lisp の defdynamic
;;   AOT変換時 : transpile.lisp の read-all-forms がホストCLの
;;               *read-default-float-format* を束縛する値
;;
;; **transpile.lisp はホストCL上で動くので runtime.h を読めず、手で合わせるしかない。**
;; ここが本ファイルの存在理由である。

;;; --- 1. 決定: 既定は <double-float> ---
;;; CommonLisp の既定(single-float)とは違う。理由は documents/float-default.md。
;;; **この判断を変えるときは、下の probe も含めて3箇所すべてを動かすこと。**
(assert-equal '<double-float> (%%class-name (class-of 1.5)))
(assert-equal '<double-float> (%%class-name (class-of 1.5e0)))
;;; 変数が保持しているのは**クラス名シンボル**なので、class-of ではなく値を直接見る
;;; (class-of すると <SYMBOL> になる)
(assert-equal '<double-float> (dynamic *read-default-float-format*))
;;; C側の定数から導いていること(Lisp側に直接書いていない)
(assert-equal '<double-float> (%%default-float-format))
(assert-equal (%%default-float-format) (dynamic *read-default-float-format*))

;;; --- 2. [検出器] AOT と実行時で既定が一致していること ---
;;;
;;; %aot-float-format-probe は init_aot.lisp の (defun %aot-float-format-probe () 1.5)。
;;; **AOTコンパイルされるので、返る値の型は transpile.lisp が何を既定だと
;;; 思っているかそのものである。** 実行時の既定と比べる。
;;;
;;; 片方だけ変えるとここが落ちる。**これがこのファイルの主目的。**
(assert-equal (%%class-name (class-of (parse-number "1.5")))
              (%%class-name (class-of (%aot-float-format-probe))))
;;; 上と同じことを、既定の名前と直接比べる形でも見る
;;; (parse-number 側が壊れていても検出できるように、独立した経路で)
(assert-equal (dynamic *read-default-float-format*)
              (%%class-name (class-of (%aot-float-format-probe))))
;;; 値そのものも確認しておく(型だけ合っていて値が化けている、を防ぐ)
(assert-equal t (= 1.5 (%aot-float-format-probe)))

;;; --- 3. 接尾辞付きは既定に左右されない ---
;;; 既定をどちらにしても、明示した型はそのままになる
(assert-equal '<single-float> (%%class-name (class-of 1.5f0)))
(assert-equal '<double-float> (%%class-name (class-of 1.5d0)))

;;; --- 4. 既定に従う経路が揃っていること ---
;;; リーダ・数学関数・float/convert がすべて同じ既定を見る
(assert-equal '<double-float> (%%class-name (class-of (sqrt 2))))
(assert-equal '<double-float> (%%class-name (class-of (exp 1))))
(assert-equal '<double-float> (%%class-name (class-of (float 2))))
(assert-equal '<double-float> (%%class-name (class-of (convert 3 <float>))))
(assert-equal '<double-float> (%%class-name (class-of (atan2 1 1))))
;;; ISLisp 仕様例 (convert 3 <float>) => 3.0 が何によって成立しているかの確認。
;;; **double 決め打ちではなく、*read-default-float-format* を見た結果である**
;;; (documents/float-default.md §3)
(assert-equal t (= 3.0 (convert 3 <float>)))
(assert-equal '<single-float>
              (dynamic-let ((*read-default-float-format* '<single-float>))
                (%%class-name (class-of (convert 3 <float>)))))
(assert-equal '<single-float>
              (dynamic-let ((*read-default-float-format* '<single-float>))
                (%%class-name (class-of (float 2)))))
;;; 抜けたら戻っている
(assert-equal '<double-float> (%%class-name (class-of (convert 3 <float>))))
