;; M12(#27)のregressionテスト: call-next-methodで次のメソッドが無い場合の
;; エラー通知が正しくignore-errorsで捕捉できることを、host側(script_test.c、
;; global_environmentへ直接init.lispをloadする実プロダクション構成と同一の
;; トポロジー)で確認する。実機QEMU(x86_64/mingw)限定で再現したregressionだが、
;; この構成でも安価に確認できるようにしておく(詳細はtest/lisp/isiki_test.lisp
;; の同種のテストのコメント参照)
(defclass diag-point () ((x :initarg :x :initform 0) (y :initarg :y :initform 0)))
(defgeneric diag-no-next-method-test (obj))
(defmethod diag-no-next-method-test (obj) (call-next-method))
(assert-equal nil (ignore-errors (diag-no-next-method-test (make-instance 'diag-point))))
