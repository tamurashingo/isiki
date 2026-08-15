;; test/lisp/za_test.lisp
;;
;; za.c(defunをx86-64機械語へJITコンパイルするコンパイラ)がどのdefunをコンパイルし、
;; どれをインタプリタへフォールバックするかを検証するテスト。
;;
;; za.cが出力する機械語は実機ビルド(mingw-gcc, MS x64呼び出し規約, 実行可能メモリ)を
;; 前提としており、ネイティブgccでビルドするユニットテスト(test/c/eval_test.c)では
;; ABIやメモリ実行属性が一致しないため、ユニットテスト側ではJIT自体を常に無効化して
;; いる(za.c参照)。そのため実際のJITコンパイル判定・生成された機械語の実行結果の検証は
;; このファイルをQEMU実機上でloadすることでのみ行う(make test-qemu)。
;;
;; %%za-compiled-p は第一引数の関数がzaで機械語へコンパイルされたかどうかを判定する
;; テスト用の内部組み込み関数(ISLisp仕様外なので%%を付ける、runtime.c参照)。
;; 本ファイルは test/lisp/test_framework.lisp が定義する assert-equal 等をそのまま
;; 使う(boot-entryスクリプトが本ファイルより先にそれをloadしている前提)。
;;
;; 拡張5(非局所脱出: block/return-from, catch/throw, tagbody/go, unwind-protect)は
;; test/lisp/za_test_ext5.lisp に分割してある(QEMUテストのmilestone分割用)。

;; (+ x y z) : オペランド3個の+はzaでコンパイルされる
(defun isiki-za-test-add3 (x y z) (+ x y z))
(assert-equal t (%%za-compiled-p (function isiki-za-test-add3)))
(assert-equal 6 (isiki-za-test-add3 1 2 3))

;; &restはあるがbodyから参照しないparamsはzaでコンパイルされる
(defun isiki-za-test-add-fixed (x &rest y) (+ x 1))
(assert-equal t (%%za-compiled-p (function isiki-za-test-add-fixed)))
(assert-equal 11 (isiki-za-test-add-fixed 10 20 30))

;; rest引数名を+のオペランドに使うとzaはコンパイルを諦めインタプリタにフォールバックする
(defun isiki-za-test-bad-rest (x &rest y) (+ x y))
(assert-equal nil (%%za-compiled-p (function isiki-za-test-bad-rest)))

;; xを17個並べてzaのオペランド上限(16個)を超えるとインタプリタにフォールバックする
(defun isiki-za-test-add17 (x)
  (+ x x x x x x x x x x x x x x x x x))
(assert-equal nil (%%za-compiled-p (function isiki-za-test-add17)))
(assert-equal 17 (isiki-za-test-add17 1))

;; (if x 1 2) : else付きifはzaでコンパイルされる
(defun isiki-za-test-pick (x) (if x 1 2))
(assert-equal t (%%za-compiled-p (function isiki-za-test-pick)))
(assert-equal 1 (isiki-za-test-pick 0))
(assert-equal 2 (isiki-za-test-pick nil))

;; (if x 1) : else無しifもzaでコンパイルされる
(defun isiki-za-test-pick-noelse (x) (if x 1))
(assert-equal t (%%za-compiled-p (function isiki-za-test-pick-noelse)))
(assert-equal 1 (isiki-za-test-pick-noelse 5))
(assert-equal nil (isiki-za-test-pick-noelse nil))

;; ifのネストもzaでコンパイルされる
(defun isiki-za-test-pick2 (x y) (if x (if y 1 2) 3))
(assert-equal t (%%za-compiled-p (function isiki-za-test-pick2)))
(assert-equal 1 (isiki-za-test-pick2 1 1))
(assert-equal 2 (isiki-za-test-pick2 1 nil))
(assert-equal 3 (isiki-za-test-pick2 nil 1))

;; ifのthen/elseの中に+を書いてもzaでコンパイルされる
(defun isiki-za-test-addif (x y) (if x (+ x y) 0))
(assert-equal t (%%za-compiled-p (function isiki-za-test-addif)))
(assert-equal 7 (isiki-za-test-addif 3 4))
(assert-equal 0 (isiki-za-test-addif nil 4))

;; +のオペランドの位置に直接ifを書くとzaはコンパイルを諦めインタプリタにフォールバックする
(defun isiki-za-test-bad (x y) (+ x (if y 1 2)))
(assert-equal nil (%%za-compiled-p (function isiki-za-test-bad)))
(assert-equal 4 (isiki-za-test-bad 3 5))
(assert-equal 5 (isiki-za-test-bad 3 nil))

;; (- x y) : 2引数の-はzaでコンパイルされる
(defun isiki-za-test-sub (x y) (- x y))
(assert-equal t (%%za-compiled-p (function isiki-za-test-sub)))
(assert-equal 7 (isiki-za-test-sub 10 3))

;; (- x) : 単項の-もzaでコンパイルされる
(defun isiki-za-test-neg (x) (- x))
(assert-equal t (%%za-compiled-p (function isiki-za-test-neg)))
(assert-equal (- 5) (isiki-za-test-neg 5))
(assert-equal 0 (isiki-za-test-neg 0))

;; (- x y z) : 3引数の-もzaでコンパイルされる
(defun isiki-za-test-sub3 (x y z) (- x y z))
(assert-equal t (%%za-compiled-p (function isiki-za-test-sub3)))
(assert-equal 5 (isiki-za-test-sub3 10 3 2))

;; (* x y z) : 3引数の*はzaでコンパイルされる
(defun isiki-za-test-mul3 (x y z) (* x y z))
(assert-equal t (%%za-compiled-p (function isiki-za-test-mul3)))
(assert-equal 24 (isiki-za-test-mul3 2 3 4))

;; (* x) : オペランド1個の*はzaのコンパイル対象外でインタプリタにフォールバックする
(defun isiki-za-test-ident (x) (* x))
(assert-equal nil (%%za-compiled-p (function isiki-za-test-ident)))
(assert-equal 9 (isiki-za-test-ident 9))

;; (< x y) : 2引数の<はzaでコンパイルされる
(defun isiki-za-test-lt (x y) (< x y))
(assert-equal t (%%za-compiled-p (function isiki-za-test-lt)))
(assert-equal t (isiki-za-test-lt 1 2))
(assert-equal nil (isiki-za-test-lt 2 1))

;; (= x y) : 2引数の=はzaでコンパイルされる
(defun isiki-za-test-numeq (x y) (= x y))
(assert-equal t (%%za-compiled-p (function isiki-za-test-numeq)))
(assert-equal t (isiki-za-test-numeq 3 3))
(assert-equal nil (isiki-za-test-numeq 3 4))

;; (< x y z) : 3引数の<はzaのコンパイル対象外でインタプリタにフォールバックする
(defun isiki-za-test-lt3 (x y z) (< x y z))
(assert-equal nil (%%za-compiled-p (function isiki-za-test-lt3)))
(assert-equal t (isiki-za-test-lt3 1 2 3))
(assert-equal nil (isiki-za-test-lt3 1 3 2))

;; ifのthen/elseの中に-や*を書いてもzaでコンパイルされる
(defun isiki-za-test-calc (x y) (if (< x y) (- y x) (* x y)))
(assert-equal t (%%za-compiled-p (function isiki-za-test-calc)))
(assert-equal 2 (isiki-za-test-calc 3 5))
(assert-equal 15 (isiki-za-test-calc 5 3))

;; -のオペランドに直接ifを書くとzaはコンパイルを諦めインタプリタにフォールバックする
(defun isiki-za-test-bad2 (x y) (- x (if y 1 2)))
(assert-equal nil (%%za-compiled-p (function isiki-za-test-bad2)))
(assert-equal 9 (isiki-za-test-bad2 10 5))
(assert-equal 8 (isiki-za-test-bad2 10 nil))

;;; --- 拡張2: 制御構文(マクロ展開を前提とする) ---
;;
;; cond/let/let*/while/for/and/or/caseはCの特殊形式ではなくすべてinit.lispのdefmacroで
;; 定義されたマクロで、展開結果はif/progn/setq/tagbody/go/lambdaの組み合わせになる。
;; zaはbodyそのもの、およびifのtest/then/elseの位置でmacroexpand-1相当をfixpointまで
;; 適用してから検証・コード生成する。andはif木へ完全展開されるためこれでコンパイル
;; できるようになるが、let/or/condはlambda即時呼び出しやprognへ展開されるため、
;; 拡張3(関数呼び出し)・拡張4(クロージャ)完了までは非対応としてインタプリタへ
;; フォールバックする(結果自体は正しい)。

;; (and x y) : 2引数のandは(if x y nil)へ展開されるのでzaでコンパイルされる
(defun isiki-za-test-and2 (x y) (and x y))
(assert-equal t (%%za-compiled-p (function isiki-za-test-and2)))
(assert-equal 2 (isiki-za-test-and2 1 2))
(assert-equal nil (isiki-za-test-and2 nil 2))
(assert-equal nil (isiki-za-test-and2 1 nil))

;; (and x y z) : 3引数のandも入れ子のifへ完全展開されるのでzaでコンパイルされる
(defun isiki-za-test-and3 (x y z) (and x y z))
(assert-equal t (%%za-compiled-p (function isiki-za-test-and3)))
(assert-equal 3 (isiki-za-test-and3 1 2 3))
(assert-equal nil (isiki-za-test-and3 1 nil 3))

;; ifのtest位置にandを書いても展開されてzaでコンパイルされる
(defun isiki-za-test-if-and (x y) (if (and x y) 1 2))
(assert-equal t (%%za-compiled-p (function isiki-za-test-if-and)))
(assert-equal 1 (isiki-za-test-if-and 1 1))
(assert-equal 2 (isiki-za-test-if-and 1 nil))

;; letが展開する即時lambda呼び出しは、拡張3・拡張4が完了するまで非対応でフォールバックする
;; (結果自体はインタプリタ経由で正しい)
(defun isiki-za-test-let-fallback (x)
  (let ((y 1))
    (+ x y)))
(assert-equal nil (%%za-compiled-p (function isiki-za-test-let-fallback)))
(assert-equal 11 (isiki-za-test-let-fallback 10))

;; orはgensymの一時変数をletで束縛する形に展開されるため同様にフォールバックする
(defun isiki-za-test-or-fallback (x y) (or x y))
(assert-equal nil (%%za-compiled-p (function isiki-za-test-or-fallback)))
(assert-equal 1 (isiki-za-test-or-fallback 1 2))
(assert-equal 2 (isiki-za-test-or-fallback nil 2))

;; condはprognを含む形に展開されるため同様にフォールバックする
(defun isiki-za-test-cond-fallback (x)
  (cond (x 1)
        (t 2)))
(assert-equal nil (%%za-compiled-p (function isiki-za-test-cond-fallback)))
(assert-equal 1 (isiki-za-test-cond-fallback t))
(assert-equal 2 (isiki-za-test-cond-fallback nil))

;;; --- 拡張3: 関数呼び出し(自己再帰・相互再帰・他関数呼び出し) ---
;;
;; 呼び出しの引数式はleaf/算術(+ - *)/比較(< =)/ifに限定され、引数の位置に別の
;; 関数呼び出しを直接書くことはできない(その場合はzaはコンパイルを諦めインタプリタに
;; フォールバックする)。body直下、およびifのthen/else(ifが末尾位置にある場合)に
;; 書かれた呼び出しは末尾呼び出しとなり、呼び出し先がza/組み込みprimitiveでコンパイル
;; 済みの場合は共有トランポリンを介したTCOが働くため、深い自己再帰・相互再帰でも
;; Cスタックを消費せずクラッシュしない。

;; 自己再帰(アキュムレータ渡し末尾再帰スタイル)
(defun isiki-za-test-fact-iter (n acc)
  (if (= n 0)
      acc
      (isiki-za-test-fact-iter (- n 1) (* n acc))))
(assert-equal t (%%za-compiled-p (function isiki-za-test-fact-iter)))
(assert-equal 3628800 (isiki-za-test-fact-iter 10 1))

;; 深い自己再帰でもトランポリンのTCOによりCスタックを消費せずクラッシュしない
(defun isiki-za-test-sum-iter (n acc)
  (if (= n 0)
      acc
      (isiki-za-test-sum-iter (- n 1) (+ n acc))))
(assert-equal t (%%za-compiled-p (function isiki-za-test-sum-iter)))
(assert-equal 5050 (isiki-za-test-sum-iter 100 0))
(assert-equal 20000100000 (isiki-za-test-sum-iter 200000 0))

;; 相互再帰。isiki-za-test-even-pをコンパイルする時点ではisiki-za-test-odd-pはまだ
;; 定義されていないが、呼び出し先の解決は実行時にos_get_functionで行うため定義順序に
;; 依存せずコンパイルできる。ちょうど0の場合の戻り値はbare symbol(t/nil)リテラルには
;; 非対応(zaのグラマーはleaf/算術/比較/if/一般呼び出しのみ)なので、(= n n)/(< n 0)で
;; 代わりにt/nilを作る(paramへの参照はleaf扱いのままコンパイル対象になる)。
(defun isiki-za-test-even-p (n)
  (if (= n 0) (= n n) (isiki-za-test-odd-p (- n 1))))
(defun isiki-za-test-odd-p (n)
  (if (= n 0) (< n 0) (isiki-za-test-even-p (- n 1))))
(assert-equal t (%%za-compiled-p (function isiki-za-test-even-p)))
(assert-equal t (%%za-compiled-p (function isiki-za-test-odd-p)))
(assert-equal t (isiki-za-test-even-p 10))
(assert-equal nil (isiki-za-test-even-p 7))
(assert-equal t (isiki-za-test-odd-p 7))
(assert-equal nil (isiki-za-test-odd-p 10))
;; 深い相互再帰でもトランポリンのTCOによりCスタックを消費せずクラッシュしない
(assert-equal t (isiki-za-test-even-p 200000))

;; 他関数呼び出し: 既にコンパイル済みの別のza関数を(leaf引数で)呼ぶ
(defun isiki-za-test-call-other (x y)
  (isiki-za-test-add3 x y 1))
(assert-equal t (%%za-compiled-p (function isiki-za-test-call-other)))
(assert-equal 6 (isiki-za-test-call-other 2 3))

;; 引数の位置に別の呼び出しを直接書く(ネスト呼び出し)とzaはコンパイルを諦め
;; インタプリタにフォールバックする(結果自体はインタプリタ経由で正しい)
(defun isiki-za-test-call-nested-fallback (x)
  (isiki-za-test-add3 x (isiki-za-test-neg x) 1))
(assert-equal nil (%%za-compiled-p (function isiki-za-test-call-nested-fallback)))
(assert-equal 1 (isiki-za-test-call-nested-fallback 5))

;; ifのtest位置に呼び出しを直接書いた場合も同様にフォールバックする
(defun isiki-za-test-call-in-test-fallback (x)
  (if (isiki-za-test-numeq x 0) 1 2))
(assert-equal nil (%%za-compiled-p (function isiki-za-test-call-in-test-fallback)))
(assert-equal 1 (isiki-za-test-call-in-test-fallback 0))
(assert-equal 2 (isiki-za-test-call-in-test-fallback 5))

;; 除外リストの特殊形式(progn)がbody直下に来る場合も同様にフォールバックする
(defun isiki-za-test-progn-fallback (x)
  (progn (+ x 1)))
(assert-equal nil (%%za-compiled-p (function isiki-za-test-progn-fallback)))
(assert-equal 6 (isiki-za-test-progn-fallback 5))

;;; --- 拡張1: 基本データ操作とヒープ確保・シャドウスタック連携 ---
;;
;; car/cdr/atom/null/eqは非allocating(cc_car/cc_cdrを直接呼ぶ、または引数リストconsを
;; 経由しない直接比較)、consのみヒープ確保を伴う(os_make_cons自身がGC_PROTECTで
;; 自分の2引数を保護するため、za側で追加のshadow stack link/unlinkは不要)。
;; いずれもleaf限定のexprとして+/-/*/</=/ifと同じ位置制約でコンパイルされる。
(close (open-output-file "tmp/ckpt-0-start.txt"))

;; (car x) / (cdr x) : leafのcar/cdrはzaでコンパイルされる
(defun isiki-za-test-car (x) (car x))
(assert-equal t (%%za-compiled-p (function isiki-za-test-car)))
(assert-equal 1 (isiki-za-test-car (cons 1 2)))
(defun isiki-za-test-cdr (x) (cdr x))
(assert-equal t (%%za-compiled-p (function isiki-za-test-cdr)))
(assert-equal 2 (isiki-za-test-cdr (cons 1 2)))
(close (open-output-file "tmp/ckpt-1-carcdr.txt"))

;; (atom x) : fixnum/symbol/nilはatom、consはatomでない
(defun isiki-za-test-atom (x) (atom x))
(assert-equal t (%%za-compiled-p (function isiki-za-test-atom)))
(assert-equal t (isiki-za-test-atom 1))
(assert-equal t (isiki-za-test-atom nil))
(assert-equal t (isiki-za-test-atom 'foo))
(assert-equal nil (isiki-za-test-atom (cons 1 2)))
;; ATOMはインタプリタ側でも(za未対応のフォールバック経路でも)使える正式な組み込み関数
(assert-equal t (atom 1))
(assert-equal nil (atom (cons 1 2)))
(close (open-output-file "tmp/ckpt-2-atom.txt"))

;; (null x) : nilならt、それ以外はnil
(defun isiki-za-test-null (x) (null x))
(assert-equal t (%%za-compiled-p (function isiki-za-test-null)))
(assert-equal t (isiki-za-test-null nil))
(assert-equal nil (isiki-za-test-null 1))
(close (open-output-file "tmp/ckpt-3-null.txt"))

;; (eq x y) : ポインタ同一性比較
(defun isiki-za-test-eq (x y) (eq x y))
(assert-equal t (%%za-compiled-p (function isiki-za-test-eq)))
(assert-equal t (isiki-za-test-eq 3 3))
(assert-equal nil (isiki-za-test-eq 3 4))
(assert-equal t (isiki-za-test-eq nil nil))
(close (open-output-file "tmp/ckpt-4-eq.txt"))

;; (cons x y) : ヒープ確保を伴うがleafオペランドのみでzaでコンパイルされる
(defun isiki-za-test-cons (x y) (cons x y))
(assert-equal t (%%za-compiled-p (function isiki-za-test-cons)))
(assert-equal 3 (car (isiki-za-test-cons 3 4)))
(assert-equal 4 (cdr (isiki-za-test-cons 3 4)))
(close (open-output-file "tmp/ckpt-5-cons.txt"))

;; ifのthen/elseの中にcar/cons等を書いてもzaでコンパイルされる
(defun isiki-za-test-car-if (x y) (if x (cons x y) (car y)))
(assert-equal t (%%za-compiled-p (function isiki-za-test-car-if)))
(assert-equal 5 (car (isiki-za-test-car-if 5 6)))
(assert-equal 9 (isiki-za-test-car-if nil (cons 9 10)))
(close (open-output-file "tmp/ckpt-6-carif.txt"))

;; car/cdr/cons/atom/null/eqの引数位置に直接別の呼び出しを書く(ネスト)とzaはコンパイル
;; を諦めインタプリタにフォールバックする(結果自体は正しい)
(defun isiki-za-test-cons-nested-fallback (x)
  (cons (car x) (cdr x)))
(assert-equal nil (%%za-compiled-p (function isiki-za-test-cons-nested-fallback)))
(assert-equal 1 (car (isiki-za-test-cons-nested-fallback (cons 1 2))))
(assert-equal 2 (cdr (isiki-za-test-cons-nested-fallback (cons 1 2))))
(close (open-output-file "tmp/ckpt-7-nested.txt"))

;; 大量にconsを呼び続けてGC(os_gc_collect)を誘発しても、za生成コードが結果を破壊
;; しないことを確認する(os_make_cons自身のGC_PROTECTだけで安全という設計の裏付け)。
;; isiki-za-test-cons-chain自身はlet/for/setqを含むためza非対応(フォールバック)だが、
;; ループ本体で呼ぶisiki-za-test-consはza機械語として繰り返し実行される。
(defun isiki-za-test-cons-chain (n)
  (let ((acc nil))
    (for ((i 0 (+ i 1))) ((= i n) acc)
      (setq acc (isiki-za-test-cons i acc)))))
(close (open-output-file "tmp/ckpt-8-before-chain.txt"))

(assert-equal 50000 (length (isiki-za-test-cons-chain 50000)))
(close (open-output-file "tmp/ckpt-9-after-chain.txt"))
(assert-equal 49999 (car (isiki-za-test-cons-chain 50000)))
(close (open-output-file "tmp/ckpt-10-final.txt"))

;;; --- 拡張4: クロージャ・lambda ---
;;
;; (lambda (params...) . body)は、外側defunの固定引数(&restを除く)を呼ばれるたびに
;; 新規environmentへos_set_variableでコピーし、それを閉じ込めたMAGIC_FUNCTION_INTERPRETED
;; インスタンス(インタプリタの通常のクロージャ表現そのもの)を組み立てるだけで、単一
;; レベルのネストのみ対応する。lambda本体自体はzaがコンパイルせず、呼び出しは常に
;; インタプリタ(apply_function)経由で実行される。

;; 定数を返すだけのクロージャ
(defun isiki-za-test-make-const (x) (lambda () x))
(assert-equal t (%%za-compiled-p (function isiki-za-test-make-const)))
(assert-equal 42 (funcall (isiki-za-test-make-const 42)))
(close (open-output-file "tmp/ckpt-11-const.txt"))

;; 1引数を捕捉するアダー・クロージャ
(defun isiki-za-test-make-adder (x) (lambda (y) (+ x y)))
(assert-equal t (%%za-compiled-p (function isiki-za-test-make-adder)))
(assert-equal 7 (funcall (isiki-za-test-make-adder 3) 4))
(close (open-output-file "tmp/ckpt-12-adder.txt"))

;; 複数の固定引数を捕捉するクロージャ
(defun isiki-za-test-make-combo (x y) (lambda (z) (+ x (+ y z))))
(assert-equal t (%%za-compiled-p (function isiki-za-test-make-combo)))
(assert-equal 6 (funcall (isiki-za-test-make-combo 1 2) 3))
(close (open-output-file "tmp/ckpt-13-combo.txt"))

;; 独立性: isiki-za-test-make-adder(za機械語として実行される)を異なる引数で2回呼び、
;; 両方のクロージャを同時に保持しても互いに影響されず独立した値を保持すること
(let ((c1 (isiki-za-test-make-adder 10))
      (c2 (isiki-za-test-make-adder 20)))
  (assert-equal 15 (funcall c1 5))
  (assert-equal 25 (funcall c2 5))
  (assert-equal 15 (funcall c1 5)))
(close (open-output-file "tmp/ckpt-14-independence.txt"))

;; if分岐内のlambda: どちらの分岐でクロージャが作られても正しく捕捉される
(defun isiki-za-test-maybe (x) (if (< x 0) (lambda () -1) (lambda () 1)))
(assert-equal t (%%za-compiled-p (function isiki-za-test-maybe)))
(assert-equal (- 1) (funcall (isiki-za-test-maybe (- 5))))
(assert-equal 1 (funcall (isiki-za-test-maybe 5)))
(close (open-output-file "tmp/ckpt-15-maybe.txt"))

;; lambdaを引数位置に置くが、呼び出し先はfuncall(組み込みnative)ではなく
;; JIT/インタプリタのユーザ定義関数(引数をそのまま返すだけ)にするケース
(defun isiki-za-test-arg-echo (f) f)

;; 外側関数の固定引数が0個(コピーループが0回)の場合でも正しくクロージャが
;; 構築・伝播されること
(defun isiki-za-test-lambda-as-arg-zero-diag () (isiki-za-test-arg-echo (lambda () 42)))
(assert-equal t (%%za-compiled-p (function isiki-za-test-lambda-as-arg-zero-diag)))
(assert-equal t (functionp (isiki-za-test-lambda-as-arg-zero-diag)))
(assert-equal 42 (funcall (isiki-za-test-lambda-as-arg-zero-diag)))

(defun isiki-za-test-lambda-as-arg-diag (x) (isiki-za-test-arg-echo (lambda (y) (+ x y))))
(assert-equal t (%%za-compiled-p (function isiki-za-test-lambda-as-arg-diag)))
(assert-equal t (functionp (isiki-za-test-lambda-as-arg-diag 5)))
(assert-equal nil (consp (isiki-za-test-lambda-as-arg-diag 5)))
(assert-equal 105 (funcall (isiki-za-test-lambda-as-arg-diag 5) 100))
(close (open-output-file "tmp/ckpt-15c-lambda-as-arg-diag.txt"))

;; 呼び出しの引数位置に直接lambdaが現れるケース(ZA_OFF_LAMBDA_SAVED_HEADが
;; ZA_OFF_CALL_SAVED_HEADと衝突しないことの確認): funcallは既存の組み込み関数
(defun isiki-za-test-apply-it (x) (funcall (lambda (y) (+ x y)) 100))
(assert-equal t (%%za-compiled-p (function isiki-za-test-apply-it)))
(assert-equal 105 (isiki-za-test-apply-it 5))
(close (open-output-file "tmp/ckpt-16-apply-it.txt"))

;; 大量にクロージャを生成し続けてGC(os_gc_collect)を誘発しても、古いクロージャ・
;; 新しいクロージャどちらも正しい値を返すことを確認する
(defun isiki-za-test-adder-chain (n)
  (let ((acc nil))
    (for ((i 0 (+ i 1))) ((= i n) acc)
      (setq acc (cons (isiki-za-test-make-adder i) acc)))))
(close (open-output-file "tmp/ckpt-17-before-closure-chain.txt"))

(defun isiki-za-test-sum-closure-chain (lst)
  (let ((acc 0))
    (for ((rest lst (cdr rest))) ((null rest) acc)
      (setq acc (+ acc (funcall (car rest) 1))))))

(assert-equal 50000 (length (isiki-za-test-adder-chain 50000)))
(close (open-output-file "tmp/ckpt-18-after-closure-chain.txt"))

;; sum_{i=0}^{49999} (i+1) = sum_{i=0}^{49999} i + 50000 = 49999*50000/2 + 50000 = 1250025000
(assert-equal 1250025000 (isiki-za-test-sum-closure-chain (isiki-za-test-adder-chain 50000)))
(close (open-output-file "tmp/ckpt-19-final.txt"))

;;; --- 拡張6: 動的変数(defdynamic/dynamic) ---

(defdynamic *iza-test-dyn1* 111)

(defun isiki-za-test-read-dyn1 () (dynamic *iza-test-dyn1*))
(assert-equal t (%%za-compiled-p (function isiki-za-test-read-dyn1)))
(assert-equal 111 (isiki-za-test-read-dyn1))

(defun isiki-za-test-write-dyn1 (v) (defdynamic *iza-test-dyn1* v))
(assert-equal t (%%za-compiled-p (function isiki-za-test-write-dyn1)))
;; defdynamicはeval_defdynamicと同じくvalではなくname自身を返す
(assert-equal '*iza-test-dyn1* (isiki-za-test-write-dyn1 222))
(assert-equal 222 (isiki-za-test-read-dyn1))

;; defdynamicのvalue-formが制御転送(return-from)を返すなら、os_set_dynamicを呼ばず
;; そのまま制御転送を伝播すること(動的値は更新されないままであること)
(defun isiki-za-test-defdynamic-nlx (n)
  (block done
    (defdynamic *iza-test-dyn1* (if (< n 0) (return-from done 999) 333))))
(assert-equal t (%%za-compiled-p (function isiki-za-test-defdynamic-nlx)))
(assert-equal 999 (isiki-za-test-defdynamic-nlx -1))
(assert-equal 222 (isiki-za-test-read-dyn1))
(assert-equal '*iza-test-dyn1* (isiki-za-test-defdynamic-nlx 1))
(assert-equal 333 (isiki-za-test-read-dyn1))

;; 大量にdefdynamicを呼び続けてGC(os_gc_collect)を誘発しても、戻り値(name)が
;; コンパイル時に埋め込んだシンボルとeqであり続けること(os_set_dynamic呼び出しを
;; 挟んだ後の再解決が正しいことの確認)、および動的値自体も最終的に正しいことを確認する
(defdynamic *iza-test-dyn2* 0)

(defun isiki-za-test-write-dyn2 (v) (defdynamic *iza-test-dyn2* v))
(assert-equal t (%%za-compiled-p (function isiki-za-test-write-dyn2)))

(defun isiki-za-test-dyn-stress (n)
  (let ((acc nil))
    (for ((i 0 (+ i 1))) ((= i n) acc)
      (setq acc (cons (isiki-za-test-write-dyn2 i) acc)))))

(defun isiki-za-test-all-eq-sym (lst sym)
  (for ((rest lst (cdr rest)) (ok t ok)) ((null rest) ok)
    (if (not (eq (car rest) sym)) (setq ok nil))))

(assert-equal 50000 (length (isiki-za-test-dyn-stress 50000)))
(assert-equal t (isiki-za-test-all-eq-sym (isiki-za-test-dyn-stress 50000) '*iza-test-dyn2*))
(assert-equal 49999 (dynamic *iza-test-dyn2*))
(close (open-output-file "tmp/ckpt-20-defdynamic.txt"))
