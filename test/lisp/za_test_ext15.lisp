;; test/lisp/za_test_ext15.lisp
;;
;; za.c(defunをx86-64機械語へJITコンパイルするコンパイラ)の拡張15
;; (引数位置・ifのtest位置での一般呼び出しのネスト対応)を検証するテスト。
;; za_test_ext5.lisp〜za_test_ext14.lispと同様に独立ファイルとして切り出す。
;; za.cのグラマー全体・za_test.lisp(拡張0〜4)については test/lisp/za_test.lisp
;; のコメントを参照。
;;
;; 本ファイルは test/lisp/test_framework.lisp が定義する assert-equal 等をそのまま
;; 使う(boot-entryスクリプトが本ファイルより先にそれをloadしている前提)。他のext系
;; ファイルへの依存は無い(自己完結、za_test_ext14.lispと同じ趣旨のヘルパーを
;; iz15-プレフィクスで独自に定義する)。
;;
;; 拡張15以前は`za_compile_expr`の`allow_call`ゲートにより、一般呼び出しの引数位置・
;; ifのtest位置に別の一般呼び出しを直接書くと(例: `(iz15-inc (iz15-inc x))`)
;; コンパイルを諦めインタプリタへfallbackしていた。CALL_SAVED_HEAD・引数スロットを
;; `call_depth`でインデックスする配列(ZA_OFF_CALL_BASE、ZA_MAX_CALL_DEPTH=4)にした
;; ことで、外側の呼び出しが使用中のスロットを内側の呼び出しが上書きしなくなり、
;; ゲート自体を撤去できた(za.c内のコメント参照)。この変更に伴い期待値が変わった
;; 既存の回帰テスト(test/lisp/za_test.lisp の isiki-za-test-call-nested-fallback、
;; fallback前提だったものをコンパイル成功前提に更新済み)・関連コメント
;; (za_test_ext5.lisp/za_test_ext10.lisp/za_test_ext13.lisp)も本対応と合わせて
;; 更新した。
;;
;; 注意: car/cdr/cons/atom/null/eqはza_compile_unary/za_compile_binaryを経由し、
;; za_compile_call(拡張15のcall_depth配列化対象)を一切通らないため、
;; `(car (cdr x))`のようなbuilt-in単項/2項同士のネストは拡張15とは無関係。
;; 拡張15執筆時点では+/-/*同様leaf限定で非対応だったが、拡張16
;; (za_compile_operand経由のオペランド再帰対応)で解消済み(詳細は
;; test/lisp/za_test_ext16.lisp参照)。本ファイルの以下のネスト検証は
;; ユーザー定義関数(iz15-inc等)の一般呼び出し同士に限定したまま残す
;; (拡張15固有の検証観点のため)。
;;
;; za.cの+/-/*のオペランドも拡張16以前はleaf(局所変数/固定引数/fixnumリテラル)
;; 限定でネストした演算(+ (+ ..) ..)は非対応だったが、拡張16で解消済み
;; (test/lisp/za_test_ext16.lisp参照)。本ファイルでは+/-の引数は常にleafに限定し、
;; ネストは一般呼び出し(関数呼び出し)側だけで検証する(拡張15固有の検証観点のため)。

(defun iz15-inc (x) (+ x 1))
(defun iz15-neg (x) (- x))
(defun iz15-add2 (a b) (+ a b))
(defun iz15-add3 (a b c) (+ a b c))
(defun iz15-pos-p (x) (> x 0))

;;; --- 1. 素朴な2重ネスト ---

;; 組み込み単項(car/cdr)同士: 前述の注意の通りza_compile_call自体を通らないため
;; 拡張15の対象外だが、拡張16でza_compile_operand経由でコンパイルされるようになった。
(defun iz15-second (lst) (car (cdr lst)))
(assert-equal t (%%za-compiled-p (function iz15-second)))
(assert-equal 2 (iz15-second (list 1 2 3)))

;; ユーザー定義関数同士
(defun iz15-double-inc (x) (iz15-inc (iz15-inc x)))
(assert-equal t (%%za-compiled-p (function iz15-double-inc)))
(assert-equal 7 (iz15-double-inc 5))

;;; --- 2. 3引数呼び出しの途中の引数だけが内側呼び出しになるケース
;;; (兄弟スロット(0番・2番の引数)が内側呼び出しの影響を受けず正しく残ることの
;;; 回帰確認) ---

(defun iz15-mid-nested (a x c) (iz15-add3 a (iz15-neg x) c))
(assert-equal t (%%za-compiled-p (function iz15-mid-nested)))
(assert-equal 2999 (iz15-mid-nested 1000 1 2000))
(assert-equal 15 (iz15-mid-nested 7 3 11))

;;; --- 3. 同じ引数リストの中に内側呼び出しが複数個並ぶケース(兄弟同士が
;;; depth+1を順番に再利用しても干渉しないことの確認) ---

(defun iz15-sibling-nested (x y) (iz15-add2 (iz15-inc x) (iz15-inc y)))
(assert-equal t (%%za-compiled-p (function iz15-sibling-nested)))
(assert-equal 17 (iz15-sibling-nested 5 10))
(assert-equal 103 (iz15-sibling-nested 100 1))

;;; --- 4. 3重以上のネスト(ZA_MAX_CALL_DEPTHの範囲内) ---

(defun iz15-triple-nested (x) (iz15-inc (iz15-inc (iz15-inc x))))
(assert-equal t (%%za-compiled-p (function iz15-triple-nested)))
(assert-equal 13 (iz15-triple-nested 10))

;; 4重ネスト: 最内側の呼び出しがcall_depth=3でコンパイルされる、ちょうど上限の
;; 1手前(ZA_MAX_CALL_DEPTH=4未満)の境界。
(defun iz15-quad-nested (x) (iz15-inc (iz15-inc (iz15-inc (iz15-inc x)))))
(assert-equal t (%%za-compiled-p (function iz15-quad-nested)))
(assert-equal 14 (iz15-quad-nested 10))

;;; --- 5. ZA_MAX_CALL_DEPTHを超えるネストは安全にfallback ---
;;
;; 5重ネスト: 最内側の呼び出しがcall_depth=4でコンパイルされ、
;; za_compile_call冒頭の`call_depth >= ZA_MAX_CALL_DEPTH`ガードに掛かって0を返す
;; ため、defun全体がインタプリタへfallbackする。結果自体はインタプリタ経由で
;; 正しいことを確認する(za_test_ext8.lispのlet深さ超えテストと同じ趣旨)。

(defun iz15-depth-over (x) (iz15-inc (iz15-inc (iz15-inc (iz15-inc (iz15-inc x))))))
(assert-equal nil (%%za-compiled-p (function iz15-depth-over)))
(assert-equal 15 (iz15-depth-over 10))

;;; --- 6. ifのtest位置での一般呼び出し(拡張15の副産物) ---

(defun iz15-if-test-call (x)
  (if (iz15-pos-p x) 'pos 'nonpos))
(assert-equal t (%%za-compiled-p (function iz15-if-test-call)))
(assert-equal 'pos (iz15-if-test-call 5))
(assert-equal 'nonpos (iz15-if-test-call 0))
(assert-equal 'nonpos (iz15-if-test-call (iz15-neg 3)))

;; ifのtest位置に来る呼び出し自身の引数もさらにネストしたケース(6と1の複合)
(defun iz15-if-test-call-nested-arg (x)
  (if (iz15-pos-p (iz15-neg x)) 'neg-was-pos 'neg-was-nonpos))
(assert-equal t (%%za-compiled-p (function iz15-if-test-call-nested-arg)))
(assert-equal 'neg-was-nonpos (iz15-if-test-call-nested-arg 5))
(assert-equal 'neg-was-pos (iz15-if-test-call-nested-arg (iz15-neg 5)))

;;; --- 7. 既存テストの回帰確認(自己完結版) ---
;;
;; test/lisp/za_test.lispのisiki-za-test-cons-nested-fallbackと同じ形(car/cdrを
;; cons引数へ直接ネスト)を本ファイル内でも独立に確認する。cons/car/cdrはいずれも
;; za_compile_binary/za_compile_unary経由のため拡張15の対象外だったが、拡張16で
;; コンパイルされるようになった。

(defun iz15-cons-nested (x) (cons (car x) (cdr x)))
(assert-equal t (%%za-compiled-p (function iz15-cons-nested)))
(assert-equal (cons 1 (list 2 3)) (iz15-cons-nested (list 1 2 3)))
