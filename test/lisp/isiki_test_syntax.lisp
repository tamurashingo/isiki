;; test/lisp/isiki_test_syntax.lisp
;;
;; isiki_test.lisp から分離した、**リーダー構文**に依存する ISLisp 仕様(islisp-v23.pdf)の
;; "Example:" の転記。#nA(...) 配列リテラルや #() のように、リーダーが構文を知らないと
;; (read が g_sym_read_error を返して)load 自体が中断し、同じファイルの後続テストが
;; 全部消えてしまうものだけをここに置く。テストの方針・記法は isiki_test.lisp と同じ。
;;
;; boot-entryスクリプト(qemu_boot_test.lisp)は isiki_test.lisp の後にこのファイルを
;; load し、load の戻り値(成功なら t)を isiki_test.lisp 側ではなく boot-entry 側で
;; assert-equal する(構文エラーで中断した場合に「0 failed」で通ってしまわないようにする)。

;; p.33 リテラル定数の例のうち #2A((a b c) (d e f)) => #2A((a b c) (d e f)) をそのまま転記。
;; 併せて、読めた配列が2次元・要素順が仕様通りであることを確認する(cf.)。
(assert-equal #2A((a b c) (d e f)) #2A((a b c) (d e f)))
(assert-equal '(2 3) (array-dimensions #2A((a b c) (d e f))))
(assert-equal 'f (aref #2A((a b c) (d e f)) 1 2))
(assert-equal 'a (aref #2A((a b c) (d e f)) 0 0))

;; p.93 basic-array-p/basic-array*-p/general-array*-p の例をそのまま転記
(assert-equal '((nil nil nil) (t nil nil) (t nil nil) (t nil nil) (t t t))
  (mapcar (lambda (x)
            (list (basic-array-p x)
                  (basic-array*-p x)
                  (general-array*-p x)))
          '((a b c)
            "abc"
            #(a b c)
            #1a(a b c)
            #2a((a) (b) (c)))))

;; p.93 create-array の例をそのまま転記
(assert-equal #2a((0.0 0.0 0.0) (0.0 0.0 0.0)) (create-array '(2 3) 0.0))
(assert-equal #(0.0 0.0) (create-array '(2) 0.0))

;; p.94 array1 => #3a(...) をそのまま転記
(assert-equal #3a(((0 0 0) (0 0 0) (0 0 0))
                  ((0 0 0) (0 0 0) (0 0 0))
                  ((0 0 0) (0 0 0) (0 0 0)))
  (create-array '(3 3 3) 0))

;; p.95 basic-vector-p/general-vector-p の例をそのまま転記
(assert-equal '((nil nil) (t nil) (t t) (t t) (nil nil))
  (mapcar (lambda (x)
            (list (basic-vector-p x)
                  (general-vector-p x)))
          '((a b c)
            "abc"
            #(a b c)
            #1a(a b c)
            #2a((a) (b) (c)))))

;; p.96 (vector) => #() をそのまま転記
(assert-equal #() (vector))

;; cf. #1a(a b c) は1次元配列なので general-vector と同じ外延(#(a b c) と equal)であること
(assert-equal #(a b c) #1a(a b c))
