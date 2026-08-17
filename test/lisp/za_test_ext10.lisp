;; test/lisp/za_test_ext10.lisp
;;
;; za.c(defunをx86-64機械語へJITコンパイルするコンパイラ)の拡張10(quoteの対応範囲を
;; シンボルのみから、fixnum/char/nil(即値経由)およびcons/string等(ヒープ確保され
;; GCで移動しうる値、専用スロット+GCルート登録経由)まで広げる対応)を検証するテスト。
;; za_test_ext5.lisp/za_test_ext7.lisp/za_test_ext8.lisp/za_test_ext9.lispと同様に
;; 独立ファイルとして切り出す。za.cのグラマー全体・za_test.lisp(拡張0〜4)については
;; test/lisp/za_test.lisp のコメントを参照。
;;
;; 本ファイルは test/lisp/test_framework.lisp が定義する assert-equal 等をそのまま
;; 使う(boot-entryスクリプトが本ファイルより先にそれをloadしている前提)。他のext系
;; ファイルへの依存は無い(自己完結)。

;;; --- 1. (quote sym)の回帰確認(拡張7で対応済みの既存パス) ---

(defun isiki-za-test-quote-symbol (x)
  (if (eq x (quote foo)) 1 0))
(assert-equal t (%%za-compiled-p (function isiki-za-test-quote-symbol)))
(assert-equal 1 (isiki-za-test-quote-symbol 'foo))
(assert-equal 0 (isiki-za-test-quote-symbol 'bar))

;;; --- 2. リストのquote: if-test位置 ---

(defun isiki-za-test-quote-list-if-test ()
  (if (quote (1 2 3)) 'yes 'no))
(assert-equal t (%%za-compiled-p (function isiki-za-test-quote-list-if-test)))
(assert-equal 'yes (isiki-za-test-quote-list-if-test))

;;; --- 3. リストのquote: 関数呼び出しの引数位置 ---

(defun isiki-za-test-quote-list-call-arg ()
  (car (quote (1 2 3))))
(assert-equal t (%%za-compiled-p (function isiki-za-test-quote-list-call-arg)))
(assert-equal 1 (isiki-za-test-quote-list-call-arg))

;; (car (cdr (quote ...)))のようにquoteをネストした一般呼び出しの引数位置で使う
;; ケースは、quote自体は無関係で「引数位置の一般呼び出しのネスト」対応(拡張15、
;; za_test_ext15.lisp参照)が別途必要だったため、拡張10当時はここでは検証しなかった
;; (拡張10のスコープ外)。
(defun isiki-za-test-quote-list-call-arg-cdr ()
  (cdr (quote (1 2 3))))
(assert-equal t (%%za-compiled-p (function isiki-za-test-quote-list-call-arg-cdr)))
(assert-equal '(2 3) (isiki-za-test-quote-list-call-arg-cdr))

;;; --- 4. 文字列のquote ---

(defun isiki-za-test-quote-string ()
  (quote "hello"))
(assert-equal t (%%za-compiled-p (function isiki-za-test-quote-string)))
(assert-equal t (string= "hello" (isiki-za-test-quote-string)))

;;; --- 5. fixnum/nil/charのquote(即値経由、is_literal=1側) ---

(defun isiki-za-test-quote-fixnum ()
  (quote 42))
(assert-equal t (%%za-compiled-p (function isiki-za-test-quote-fixnum)))
(assert-equal 42 (isiki-za-test-quote-fixnum))

(defun isiki-za-test-quote-nil ()
  (quote nil))
(assert-equal t (%%za-compiled-p (function isiki-za-test-quote-nil)))
(assert-equal nil (isiki-za-test-quote-nil))

(defun isiki-za-test-quote-char ()
  (quote #\a))
(assert-equal t (%%za-compiled-p (function isiki-za-test-quote-char)))
(assert-equal #\a (isiki-za-test-quote-char))

;;; --- 6. 同じquote式の複数回評価でeq同一性が保たれること ---
;;
;; スロットに1回だけ格納した値をそのまま指し続けるため、同じ(quote X)式を複数回
;; (呼び出しごとに、またはif分岐ごとに)評価してもインタプリタと同様に同一のconsを
;; 指す(等価だが別オブジェクトの新規リテラルを都度生成するわけではない)。

(defun isiki-za-test-quote-eq-across-calls (x)
  (if x (quote (1 2 3)) (quote (9 9 9))))
(assert-equal t (%%za-compiled-p (function isiki-za-test-quote-eq-across-calls)))
(assert-equal t (eq (isiki-za-test-quote-eq-across-calls t) (isiki-za-test-quote-eq-across-calls t)))
(assert-equal t (eq (isiki-za-test-quote-eq-across-calls nil) (isiki-za-test-quote-eq-across-calls nil)))
(assert-equal nil (eq (isiki-za-test-quote-eq-across-calls t) (isiki-za-test-quote-eq-across-calls nil)))

;; 別々のテキスト上のquote式は(等価であっても)異なるconsであり、eqにはならない
;; (インタプリタと同じセマンティクス: quoteはAST上のリテラルをそのまま返すだけで
;; 内容による同一化は行わない)。
(defun isiki-za-test-quote-eq-distinct-forms ()
  (eq (quote (1 2 3)) (quote (1 2 3))))
(assert-equal t (%%za-compiled-p (function isiki-za-test-quote-eq-distinct-forms)))
(assert-equal nil (isiki-za-test-quote-eq-distinct-forms))
