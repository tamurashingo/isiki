;; test/lisp/za_test_ext18.lisp
;;
;; za.c(defunをx86-64機械語へJITコンパイルするコンパイラ)の拡張18
;; (quasiquote/unquote/unquote-splicing)を検証するテスト。za_test_ext5.lisp〜
;; za_test_ext17.lispと同様に独立ファイルとして切り出す。za.cのグラマー全体・
;; za_test.lisp(拡張0〜4)については test/lisp/za_test.lisp のコメントを参照。
;;
;; 本ファイルは test/lisp/test_framework.lisp が定義する assert-equal 等をそのまま
;; 使う(boot-entryスクリプトが本ファイルより先にそれをloadしている前提)。他のext系
;; ファイルへの依存は無い(自己完結、iz18-プレフィクスで独自にヘルパーを定義する)。
;;
;; eval.cのqq_expand/qq_appendはネストしたbacktickを特別扱いしない(quote-levelを
;; 追跡しない素朴な実装)。za_compile_quasiquoteもこれと等価な結果を返す(正しい
;; 標準的ネスト対応を新たに追加しない、za_compile_quasiquote定義直前のコメント参照)。
;; test/lisp/isiki_test.lisp内のトップレベル(defunの外)で書かれたquasiquoteテストは
;; os_eval経由でインタプリタがそのまま評価するためJITとは無関係だが、意味論の正として
;; 参照する(p.62-63の例をdefun内へ移したものも含む)。

;;; --- 1. 完全に定数(unquote/unquote-splicingを一切含まない)ならJIT化され、
;;;        (quote ...)委譲で正しい結果を返す ---

(defun iz18-const (x)
  `(a b c))
(assert-equal t (%%za-compiled-p (function iz18-const)))
(assert-equal '(a b c) (iz18-const 1))

;;; --- 2. 単一unquote(リスト先頭・中間・末尾) ---

(defun iz18-unquote-head (x)
  `(,x b c))
(assert-equal t (%%za-compiled-p (function iz18-unquote-head)))
(assert-equal '(1 b c) (iz18-unquote-head 1))

(defun iz18-unquote-mid (x)
  `(a ,x c))
(assert-equal t (%%za-compiled-p (function iz18-unquote-mid)))
(assert-equal '(a 2 c) (iz18-unquote-mid 2))

(defun iz18-unquote-tail (x)
  `(a b ,x))
(assert-equal t (%%za-compiled-p (function iz18-unquote-tail)))
(assert-equal '(a b 3) (iz18-unquote-tail 3))

;;; --- 3. 複数unquote(演算式・入れ子呼び出しも含む) ---

(defun iz18-unquote-multi (x y)
  `(,x ,(+ x y) ,y))
(assert-equal t (%%za-compiled-p (function iz18-unquote-multi)))
(assert-equal '(1 3 2) (iz18-unquote-multi 1 2))

;;; --- 4. unquote-splicing(リスト中間・末尾・空リスト) ---

(defun iz18-splice-mid (lst)
  `(a ,@lst b))
(assert-equal t (%%za-compiled-p (function iz18-splice-mid)))
(assert-equal '(a x y b) (iz18-splice-mid '(x y)))
(assert-equal '(a b) (iz18-splice-mid nil))

(defun iz18-splice-tail (lst)
  `(a b ,@lst))
(assert-equal t (%%za-compiled-p (function iz18-splice-tail)))
(assert-equal '(a b x y z) (iz18-splice-tail '(x y z)))

(defun iz18-splice-multi (l1 l2)
  `(,@l1 mid ,@l2))
(assert-equal t (%%za-compiled-p (function iz18-splice-multi)))
(assert-equal '(1 2 mid 3 4) (iz18-splice-multi '(1 2) '(3 4)))

;;; --- 5. dotted tail unquote(`(a . ,b)`) ---

(defun iz18-dotted-unquote (x)
  `(a . ,x))
(assert-equal t (%%za-compiled-p (function iz18-dotted-unquote)))
(assert-equal '(a . 5) (iz18-dotted-unquote 5))
(assert-equal '(a b . cons) (iz18-dotted-unquote '(b . cons)))

;;; --- 6. dotted tail定数(unquoteを伴わないatom終端) ---

(defun iz18-dotted-const (x)
  `(a b . c))
(assert-equal t (%%za-compiled-p (function iz18-dotted-const)))
(assert-equal '(a b . c) (iz18-dotted-const 1))

;;; --- 7. 裸のunquote/unquote-splicing(quasiquoteの直接の内容が(unquote x)その
;;;        ものであるケース、`,x`単体) ---

(defun iz18-bare-unquote (x)
  `,(+ x 1))
(assert-equal t (%%za-compiled-p (function iz18-bare-unquote)))
(assert-equal 6 (iz18-bare-unquote 5))

;;; --- 8. isiki_test.lisp(ISLISP仕様p.62-63)の例をdefun内へ移したもの ---
;;
;; トップレベル版(インタプリタ経由)はisiki_test.lispで既に検証済み。同じ式を
;; defun本体に置き、JIT側でも同じ結果になることを確認する。

(defun iz18-spec-example-1 ()
  `(list ,(+ 1 2) 4))
(assert-equal t (%%za-compiled-p (function iz18-spec-example-1)))
(assert-equal '(list 3 4) (iz18-spec-example-1))

(defun iz18-spec-example-2 (name)
  `(list name ,name ',name))
(assert-equal t (%%za-compiled-p (function iz18-spec-example-2)))
(assert-equal '(list name a (quote a)) (iz18-spec-example-2 'a))

(defun iz18-spec-example-3 (x)
  `(a ,(+ 1 2) ,@(list x x x) b))
(assert-equal t (%%za-compiled-p (function iz18-spec-example-3)))
(assert-equal '(a 3 x x x b) (iz18-spec-example-3 'x))

(defun iz18-spec-example-4 ()
  `((foo ,(- 10 3)) ,@(cdr '(c)) . ,(car '(cons))))
(assert-equal t (%%za-compiled-p (function iz18-spec-example-4)))
(assert-equal '((foo 7) . cons) (iz18-spec-example-4))

;;; --- 9. ネストしたリスト・入れ子quasiquote(部分的に定数な入れ子) ---
;;
;; (b ,c)自体はunquoteを含むため実行時fold、aは定数のまま(quote a)委譲される
;; (要素ごとに個別判定する設計、za_compile_quasiquote定義直前のコメント参照)。

(defun iz18-nested-list (c)
  `(a (b ,c) d))
(assert-equal t (%%za-compiled-p (function iz18-nested-list)))
(assert-equal '(a (b 9) d) (iz18-nested-list 9))

;; ネストしたbacktickは特別扱いしない(qq_expandの素朴な意味論通り、内側の,cも
;; 同じ1回の走査で評価される)。
(defun iz18-nested-backtick (c)
  `(a `(b ,c)))
(assert-equal t (%%za-compiled-p (function iz18-nested-backtick)))
(assert-equal '(a (quasiquote (b 9))) (iz18-nested-backtick 9))

;;; --- 10. unquote内でreturn-from等の非局所脱出が起きた場合、quasiquote全体を
;;;         中断してその値がそのまま返る(NLX安全性、za_compile_callのabort_cleanup
;;;         と同型のcleanupが正しく動くことの確認) ---

(defun iz18-nlx-abort (x)
  (block done
    `(a ,(if (< x 0) (return-from done 'neg) x) b)))
(assert-equal t (%%za-compiled-p (function iz18-nlx-abort)))
(assert-equal 'neg (iz18-nlx-abort -1))
(assert-equal '(a 5 b) (iz18-nlx-abort 5))

;; unquote-splicing側・複数要素の後ろ側での中断も確認する(すでにlinkした前の
;; 要素スロットが正しくunlinkされること)。
(defun iz18-nlx-abort-splice (x)
  (block done
    `(a ,@(if (< x 0) (return-from done 'neg) (list x x)) b)))
(assert-equal t (%%za-compiled-p (function iz18-nlx-abort-splice)))
(assert-equal 'neg (iz18-nlx-abort-splice -1))
(assert-equal '(a 3 3 b) (iz18-nlx-abort-splice 3))

;;; --- 11. 要素数がZA_MAX_QQ_ELEMENTS(16)を超える場合はfallbackし、インタプリタ
;;;         経由の結果は仕様通り正しいままである ---

(defun iz18-too-many-elements (x)
  `(,x 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16))
(assert-equal nil (%%za-compiled-p (function iz18-too-many-elements)))
(assert-equal '(0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16) (iz18-too-many-elements 0))

;;; --- 12. ネスト深さがZA_MAX_QQ_DEPTH(4)を超える場合はfallbackし、インタプリタ
;;;         経由の結果は仕様通り正しいままである ---
;;
;; 各レベルにunquoteを1個ずつ持たせ、レベルごとに(qq_depth+1)を消費させる
;; (単に入れ子にするだけではfast-pathで畳み込まれてしまうため、各階層で必ず
;; 動的な内容を持たせる)。

(defun iz18-too-deep (a b c d e)
  `(l0 ,a (l1 ,b (l2 ,c (l3 ,d (l4 ,e))))))
(assert-equal nil (%%za-compiled-p (function iz18-too-deep)))
(assert-equal '(l0 1 (l1 2 (l2 3 (l3 4 (l4 5)))))
              (iz18-too-deep 1 2 3 4 5))

;;; --- 13. defunの外(トップレベル)のquasiquoteは元々インタプリタ経由であり、
;;;         今回の変更で壊れていないことの回帰確認 ---

(assert-equal '(list 3 4) `(list ,(+ 1 2) 4))
(assert-equal '(a 3 x x x b) `(a ,(+ 1 2) ,@(list 'x 'x 'x) b))
