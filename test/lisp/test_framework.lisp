;; test/lisp/test_framework.lisp
;;
;; QEMU実機上でLispテストを走らせるための共通フレームワーク(assert-equal等)。
;; isiki_test.lisp/za_test.lisp/za_test_ext5.lispはこのファイルが定義するマクロ・
;; 関数を前提とするが、自分自身ではこのファイルをloadしない。boot-entryスクリプト
;; (qemu_boot_test.lisp、qemu_boot_m*.lisp)が最初に1回だけloadする決まりにする
;; (内容ファイル側にloadを持たせると、1ブートで複数の内容ファイルを連続loadする
;; ケースで*isiki-test-stream*が再オープンされてtest-results.txtの上書き・
;; カウンタリセットが起きるため)。

(defglobal *isiki-test-stream* (open-output-file "/9p/test-results.txt"))
(defglobal *isiki-test-pass* 0)
(defglobal *isiki-test-fail* 0)

(defmacro assert-equal (expected form)
  `(let ((%isiki-expected ,expected) (%isiki-actual ,form))
     (if (equal %isiki-expected %isiki-actual)
         (setq *isiki-test-pass* (+ *isiki-test-pass* 1))
         (progn
           (setq *isiki-test-fail* (+ *isiki-test-fail* 1))
           (format *isiki-test-stream* "[NG] ~S => ~S (expected ~S)~%"
                   ',form %isiki-actual %isiki-expected)))))

(defmacro assert-float-close (expected form)
  `(let ((%isiki-expected ,expected) (%isiki-actual ,form))
     (if (< (abs (- %isiki-expected %isiki-actual)) 1.0e-6)
         (setq *isiki-test-pass* (+ *isiki-test-pass* 1))
         (progn
           (setq *isiki-test-fail* (+ *isiki-test-fail* 1))
           (format *isiki-test-stream* "[NG] ~S => ~S (expected ~~ ~S)~%"
                   ',form %isiki-actual %isiki-expected)))))

;; (assert-output (result-var output-var) form body...) : formを
;; *standard-output*が文字列出力ストリーム(create-string-output-stream)に
;; 束縛された状態で評価し、その戻り値をresult-var、出力された文字列を
;; output-varへ束縛してbodyを評価する。room_test.lisp等で使っていた
;; with-standard-output+create-string-output-stream+get-output-stream-stringの
;; 定型コードをまとめたヘルパー。bodyの中で通常のassert-equal等を呼んで
;; result-var/output-varを検証する(出力内容が完全一致でなくprefix/suffix等
;; での検証が必要な場合にも対応できるよう、単一のexpected値との比較には
;; 固定しない)。
(defmacro assert-output (vars form &rest body)
  (let ((result-var (car vars)) (output-var (car (cdr vars))))
    `(with-standard-output (create-string-output-stream)
       (let ((,result-var ,form))
         (let ((,output-var (get-output-stream-string (standard-output))))
           ,@body)))))

(defun isiki-test-report ()
  (format *isiki-test-stream* "~%==== isiki tests: ~D passed, ~D failed ====~%"
          *isiki-test-pass* *isiki-test-fail*))
