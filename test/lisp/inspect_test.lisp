;; test/lisp/inspect_test.lisp
;;
;; [#214] **探索の道具の核(apps/inspect.lisp の inspect-run)を、キー入力なしで試験する。**
;;
;; 核は「値・合図の入力・表示先」を受け取る形にしてある。だから
;; **文字列入力ストリームを渡せば、ゲストへキーを送る仕組みが無くても試験できる。**
;; apps/pager.lisp / test/lisp/pager_test.lisp(#215)と同じ形である。
;;
;; 置いた対照(#214 の完了条件に 1 対 1 で対応する):
;;   §1 無検査の 3 窓口を守れている(陽性対照)
;;   §2 値の種類ごとに 1 行要約が出る(cons / string / vector / array / 実体 /
;;      分類 / 記号 / 関数 / 数 / 文字 / 流れ口)
;;   §3 1 段ぶん列挙できる
;;   §4 番号でたどって中に入れる。戻れる
;;   §5 循環した構造を渡しても止まらない(陽性対照: 自分を含むリスト)
;;   §6 任意の値を渡しても壊れない(陽性対照)
;;   §7 上限が効いている(巨大構造の歯止め)
;;
;; 入力の差し替え(create-string-input-stream)も、使っている内省の窓口も
;; **すべて既存のもので、新しい組み込みは 1 つも足していない。**
;;
;; /app から読み込んで動かす側は test/lisp/inspect_app_disk_test.lisp。
;;
;; **ここで assert-equal に渡す「実際の値」に合成値を置いていない。**
;; 落ちたとき assert-equal は実際の値を `~S` で印字する
;; (test/lisp/test_framework.lisp:222)ので、循環した構造を渡すと
;; **試験が落ちた瞬間に印字で止まる**。渡すのは文字列・記号・fixnum・
;; 短いリストだけである。

;;; ---------------------------------------------------------------------------
;;; 0. 道具立て
;;;
;;;    **ホスト共有から読む。** 先頭に区切りが無い名前は従来どおりホスト共有へ行く
;;;    (test/lisp/load_disk_test.lisp の契約)。段では /app から読む側が先に走るので、
;;;    ここは「同じ中身・別の経路」で読み直していることになる。
;;;    このファイルを単独で読んでも成立するよう、自分で load する。
;;; ---------------------------------------------------------------------------

(defglobal *ins-load-result* (load "apps/inspect.lisp"))
(assert-equal t *ins-load-result*)

;; 行送りを文字列リテラルに直に書く手段が無い(エスケープはバックスラッシュと
;; 二重引用符だけ)ので、1 文字の文字列を作って継ぎ合わせる
(defglobal *ins-nl* (create-string 1 #\newline))

;; 命令の並びを「1 行 1 命令」の入力に組み立てる。
;; **繰り返しは関数の中に入れる**(トップレベルに書いた繰り返しは 1 反復ごとに
;; 再帰的に評価されてスタックを壊す。#215 で踏んだ罠)
(defun ins-lines (cmds)
  (let ((s "") (cur cmds))
    (while (consp cur)
      (setq s (string-append s (car cur) *ins-nl*))
      (setq cur (cdr cur)))
    s))

;; 1 回ぶん動かして (結果 出力文字列) を返す。
;; **上限は引数で受けて dynamic-let で束縛する。** 動的変数に直に setq すると
;; 以後のすべての対照に漏れる(動的変数は作業単位をまたいで共有される)
(defun ins-run (limit v cmds)
  (dynamic-let ((*inspect-max-items* limit))
    (let ((out (create-string-output-stream)))
      (let ((r (inspect-run v (create-string-input-stream (ins-lines cmds)) out)))
        (list r (get-output-stream-string out))))))

;; 1 段ぶんの表示を文字列で取る(道筋は空)。
;; **文字列の器は 1024 バイトで黙ってあふれる**(stream.h:123)ので、
;; 上限を小さく束縛したうえで短い値にしか使わない
(defun ins-show (limit v)
  (dynamic-let ((*inspect-max-items* limit))
    (let ((out (create-string-output-stream)))
      (inspect-show v nil out)
      (get-output-stream-string out))))

;; 非負整数を 10 進文字列にする。期待値を組み立てるためだけの道具で、
;; 作り方は src/lisp/utility.lisp:20 の %room-digit-string と同じ
;; (**桁揃えの ~nA は無い**ので、文字列は STRING-APPEND で継ぐのがこの系の作法)
(defun ins-num (n)
  (let ((s (create-string-output-stream)))
    (format s "~D" n)
    (get-output-stream-string s)))

;; リストの長さを数える。**この試験の中だけの道具で、循環しない列にしか使わない**
;; (子の並びは apps/inspect.lisp 側で必ず上限で切れている)
(defun ins-count (lst)
  (let ((n 0) (cur lst))
    (while (consp cur)
      (setq n (+ n 1))
      (setq cur (cdr cur)))
    n))

;; 子の並びから名前で 1 つ引く
(defun ins-child-named (kids label)
  (let ((cur kids) (found nil))
    (while (and (consp cur) (null found))
      (if (eq (car (car cur)) label) (setq found (car cur)) nil)
      (setq cur (cdr cur)))
    found))

;; 資料。分類と実体の対照のために 1 つクラスを作る
(defclass ins-point () ((x :initarg :x :initform 0) (y :initarg :y :initform 0)))
(defglobal *ins-p* (make-instance 'ins-point ':x 7 ':y 8))

;; 解釈実行ではなく機械語化される関数(test/lisp/disassemble_test.lisp:14-15 と
;; 同じ前提で、実機の段では defun が JIT を通る)
(defun ins-fn (a b) (+ a b))

;;; ---------------------------------------------------------------------------
;;; 1. 【陽性対照】無検査の 3 窓口を守れている
;;;
;;;    `%%CLASS-NAME`(runtime.c:11570)/ `%%INSTANCE-CLASS`(:11694)/
;;;    `%%INSTANCE-SLOTS`(:11706)は**型検査が無く word1/word2 を生で読む。**
;;;    分類でも実体でもない値を渡すと**ヒープ外のワードがそのまま Lisp 値として
;;;    流れる**(過去に `(%%class-slots nil)` が 6931512 を返し、それが
;;;    `(make-array 6931512)` に使われてヒープを使い切った記録が
;;;    runtime.c:11610-11625 のコメントに残っている)。
;;;
;;;    **守れていることはこの節で見える。** 守りを外せば、下の nil のところに
;;;    生のワード(たいてい fixnum でも nil でもない何か)が出る。
;;;    最後の 4 件が陰性対照で、**本物を渡したときは本当に中が取れる**ことを
;;;    固定する(全部 nil を返す実装でも通ってしまうのを防ぐ)。
;;; ---------------------------------------------------------------------------

;; 分類ではない値 → nil(%%CLASS-NAME を呼んでいない)
(assert-equal nil (%inspect-class-name 42))
(assert-equal nil (%inspect-class-name nil))
(assert-equal nil (%inspect-class-name "s"))
(assert-equal nil (%inspect-class-name '(1 2)))
(assert-equal nil (%inspect-class-name (vector 1 2)))
(assert-equal nil (%inspect-class-name *ins-p*))

;; 実体ではない値 → nil(%%INSTANCE-CLASS / %%INSTANCE-SLOTS を呼んでいない)
(assert-equal nil (%inspect-instance-class 42))
(assert-equal nil (%inspect-instance-class nil))
(assert-equal nil (%inspect-instance-class (class-of 1)))
(assert-equal nil (%inspect-instance-slots 42))
(assert-equal nil (%inspect-instance-slots nil))
(assert-equal nil (%inspect-instance-slots (class-of 1)))
;; 欄の定義も同じ守りを通る(%%CLASS-SLOTS は検査を持つが、
;; 分類でない値を渡して domain-error を signal させない)
(assert-equal nil (%inspect-instance-slot-descriptors 42))
(assert-equal nil (%inspect-instance-slot-descriptors (class-of 1)))

;; 【陰性対照】本物を渡せば中が取れる
(assert-equal '<fixnum> (%inspect-class-name (class-of 1)))
(assert-equal t (%%classp (%inspect-instance-class *ins-p*)))
(assert-equal t (general-vector-p (%inspect-instance-slots *ins-p*)))
(assert-equal 2 (ins-count (%inspect-instance-slot-descriptors *ins-p*)))

;;; ---------------------------------------------------------------------------
;;; 2. 【完了条件】値の種類ごとに 1 行要約が出る
;;;
;;;    **要約は自前で組んでいる。** 合成した値を `~A`/`~S` に渡していないことが
;;;    ここで固定される: cons もベクタも配列も、中身ではなく**件数**が出る。
;;;    印字系に丸投げしていれば、ここは要素が並んだ文字列になる。
;;; ---------------------------------------------------------------------------

(assert-equal "<FIXNUM> 42" (inspect-brief 42))
(assert-equal "<FIXNUM> -7" (inspect-brief -7))
;; nil は cons でも記号でもある内部表現(runtime.c:3723-3736)。
;; **最初に除けているので <NULL> になる**
(assert-equal "<NULL>" (inspect-brief nil))
(assert-equal "<SYMBOL> T" (inspect-brief t))
(assert-equal "<SYMBOL> FOO" (inspect-brief 'foo))
(assert-equal "<CHARACTER> #\\a" (inspect-brief #\a))
(assert-equal "<STRING> \"ab\" len=2" (inspect-brief "ab"))
(assert-equal "<CONS> len=3" (inspect-brief '(1 2 3)))
;; ドット対は「尾がある」と言う(件数は car を数えた数)
(assert-equal "<CONS> len=1+tail" (inspect-brief '(1 . 2)))
(assert-equal "<GENERAL-VECTOR> len=2" (inspect-brief (vector 1 2)))
;; 配列は形も出す。`ARRAY-DIMENSIONS` は cons の列なので**要素を 1 つずつ出している**
(assert-equal "<GENERAL-ARRAY*> dims=2x3 len=6"
              (inspect-brief (create-array '(2 3) 0)))
;; 分類そのもの。種類は <BUILT-IN-CLASS> / <STANDARD-CLASS> で、自分の名前を足す
(assert-equal "<BUILT-IN-CLASS> <FIXNUM>" (inspect-brief (class-of 1)))
(assert-equal "<STANDARD-CLASS> INS-POINT" (inspect-brief (class-of *ins-p*)))
;; 実体。種類の名前がすでに所属分類の名前なので、欄の数だけ足す
(assert-equal "INS-POINT slots=2" (inspect-brief *ins-p*))
;; 流れ口は印字系が 1 行で返す種類なのでそのまま渡している
(assert-equal "<STREAM> #<STREAM>" (inspect-brief (create-string-input-stream "x")))

;; 文字列は 1 行に収まる形へ直す。**行送りと制御文字をそのまま流さない**
;; (流すと 1 行要約が複数行になる)
(assert-equal "<STRING> \".\" len=1" (inspect-brief *ins-nl*))
;; 桁数の上限で切り、切ったことを隠さない
(assert-equal "<STRING> \"abc...\" len=5"
              (dynamic-let ((*inspect-brief-chars* 3)) (inspect-brief "abcde")))

;; 関数。**中は見せない**(仮引数の並びと本体を読む窓口が無い)。
;; 組み込み primitive は機械語を持たないので長さが出ない
;; (test/lisp/disassemble_test.lisp:31 と同じ前提)
(assert-equal "<FUNCTION> #<FUNCTION-BUILTIN>" (inspect-brief (function car)))
;; 機械語化された関数は長さが出る。**前提は disassemble_test.lisp:14-15 と同じ**
(assert-equal t (%%za-compiled-p (function ins-fn)))
(assert-equal t (> (%%disasm-code-len (function ins-fn)) 0))
(assert-equal (string-append "<FUNCTION> #<FUNCTION-COMPILED> code="
                             (ins-num (%%disasm-code-len (function ins-fn))))
              (inspect-brief (function ins-fn)))

;;; ---------------------------------------------------------------------------
;;; 3. 【完了条件】1 段ぶん列挙できる
;;;
;;;    **1 段だけである。** 子の要約は出すが、子の子は出さない。
;;;    ここが循環・巨大構造に対する構造上の歯止めになっている。
;;; ---------------------------------------------------------------------------

;; cons: 要素が番号つきで並ぶ
(assert-equal (string-append
               "== <CONS> len=2" *ins-nl*
               " 0: <FIXNUM> 10" *ins-nl*
               " 1: <STRING> \"ab\" len=2" *ins-nl*)
              (ins-show 5 '(10 "ab")))

;; ドット対: 尾が tail という名前の子になる(そこから先へも入れる)
(assert-equal (string-append
               "== <CONS> len=1+tail" *ins-nl*
               " 0: <FIXNUM> 1" *ins-nl*
               " 1 TAIL: <FIXNUM> 2" *ins-nl*)
              (ins-show 5 '(1 . 2)))

;; 中身を持たない値: 「無い」と言う(黙って何も出さない形にしない)
(assert-equal (string-append "== <FIXNUM> 42" *ins-nl* " (no parts)" *ins-nl*)
              (ins-show 5 42))
(assert-equal (string-append "== <NULL>" *ins-nl* " (no parts)" *ins-nl*)
              (ins-show 5 nil))

;; 実体: 欄の名前つきで値が並ぶ。**番号で選ぶ**(同名の欄が 2 つ並びうるため。
;; 親から単純連結で上書きしない。init_aot.lisp:423-427)
(assert-equal (string-append
               "== INS-POINT slots=2" *ins-nl*
               " 0 X: <FIXNUM> 7" *ins-nl*
               " 1 Y: <FIXNUM> 8" *ins-nl*)
              (ins-show 5 *ins-p*))

;; 文字列: 1 文字ずつ
(assert-equal (string-append
               "== <STRING> \"ab\" len=2" *ins-nl*
               " 0: <CHARACTER> #\\a" *ins-nl*
               " 1: <CHARACTER> #\\b" *ins-nl*)
              (ins-show 5 "ab"))

;; ベクタ: `ELT` が cons / string / vector の 3 種に共通で効く(runtime.c:11281)
(assert-equal 3 (ins-count (%inspect-children (vector 1 2 3))))
;; 配列: 次元に関わらず平らに見える(ELT / LENGTH の既知の簡略化)
(assert-equal 6 (ins-count (%inspect-children (create-array '(2 3) 0))))

;; 分類: 親と欄の定義が子になる
(assert-equal t (if (ins-child-named (%inspect-children (class-of *ins-p*)) 'super) t nil))
(assert-equal t (if (ins-child-named (%inspect-children (class-of *ins-p*)) 'slot-def) t nil))

;; 記号: 名前・最上位環境の手続き束縛が子になる。
;; **`(function name)` を使っていない**(未定義なら signal するので任意の記号に
;; 使えない。eval.c:535-548)。環境を素の対として読んでいる
(assert-equal "CAR" (cdr (ins-child-named (%inspect-children 'car) 'name)))
(assert-equal t (if (ins-child-named (%inspect-children 'car) 'function) t nil))
;; 束縛を 1 つも持たない記号は名前だけ
(assert-equal 1 (ins-count (%inspect-children 'ins-no-such-name-at-all)))

;; 動的な名前は値を出す(名前が実行時に決まるので %%EVAL-IN-ENVIRONMENT を通す)
(assert-equal 12 (cdr (ins-child-named (%inspect-children '*inspect-max-items*) 'dynamic)))

;; 属性は属性名が子の名前になる(属性表 ((記号 . 属性名) . 値) を絞る)
(defglobal *ins-prop-set* (set-property 7 'ins-prop-sym 'ins-prop-name))
(assert-equal 7 (cdr (ins-child-named (%inspect-children 'ins-prop-sym) 'ins-prop-name)))

;; 関数は子を持たない。代わりに宣言が 3 行で出る(**第 1 版の割り切り**:
;; 解釈実行の関数の仮引数と本体を読む窓口が 1 つも無い)
(assert-equal 0 (ins-count (%inspect-children (function ins-fn))))
(assert-equal t (if (string-index "jit: YES" (ins-show 5 (function ins-fn))) t nil))
(assert-equal t (if (string-index "optimize:" (ins-show 5 (function ins-fn))) t nil))
(assert-equal t (if (string-index "declared-types: (none)"
                                  (ins-show 5 (function ins-fn))) t nil))
;; 組み込み primitive は機械語を持たないので jit は NO
(assert-equal t (if (string-index "jit: NO" (ins-show 5 (function car))) t nil))

;;; ---------------------------------------------------------------------------
;;; 4. 【完了条件】番号でたどって中に入れる。戻れる
;;;
;;;    返り値は **(理由 動いた回数 深さ)** の 3 つ組で、記号 1 つと fixnum 2 つ。
;;;    **見ていた値を返さない**(対話の輪は結果を必ず印字する。repl.c:67/:70)。
;;; ---------------------------------------------------------------------------

;; 入力が尽きれば待たずに終わる
(assert-equal '(inspect-eof 0 0) (car (ins-run 5 42 nil)))
;; q でやめる。大文字でも同じ
(assert-equal '(inspect-quit 0 0) (car (ins-run 5 42 '("q"))))
(assert-equal '(inspect-quit 0 0) (car (ins-run 5 42 '("Q"))))

;; 中に入って、戻る。深さが上がって下がる
(assert-equal '(inspect-quit 1 1) (car (ins-run 5 '(10 (20 30)) '("1" "q"))))
(assert-equal '(inspect-quit 2 2) (car (ins-run 5 '(10 (20 30)) '("1" "0" "q"))))
(assert-equal '(inspect-quit 3 1) (car (ins-run 5 '(10 (20 30)) '("1" "0" "u" "q"))))
(assert-equal '(inspect-quit 4 0) (car (ins-run 5 '(10 (20 30)) '("1" "0" "u" "u" "q"))))
;; 根でさらに戻ろうとしても壊れない(動いた回数も増えない)
(assert-equal '(inspect-quit 0 0) (car (ins-run 5 '(10 20) '("u" "q"))))
(assert-equal t (if (string-index "(at the top)" (car (cdr (ins-run 5 '(10 20) '("u" "q")))))
                    t nil))

;; 本当に中へ入っていること。**2 段目の見出しが子の要約になっている**
(assert-equal (string-append
               "== <CONS> len=2" *ins-nl*
               " 0: <FIXNUM> 10" *ins-nl*
               " 1: <CONS> len=2" *ins-nl*
               "inspect> "
               "== <CONS> len=2" *ins-nl*
               " 0: <FIXNUM> 20" *ins-nl*
               " 1: <FIXNUM> 30" *ins-nl*
               " (up available)" *ins-nl*
               "inspect> ")
              (car (cdr (ins-run 5 '(10 (20 30)) '("1" "q")))))

;; 無い番号・数でない行は**知らせて、その場に留まる**
(assert-equal '(inspect-quit 0 0) (car (ins-run 5 '(10 20) '("9" "q"))))
(assert-equal t (if (string-index "(no such part)"
                                  (car (cdr (ins-run 5 '(10 20) '("9" "q"))))) t nil))
(assert-equal '(inspect-quit 0 0) (car (ins-run 5 '(10 20) '("zz" "q"))))
(assert-equal t (if (string-index "(type a number, u, or q)"
                                  (car (cdr (ins-run 5 '(10 20) '("zz" "q"))))) t nil))
;; 空行はもう一度出すだけで、動いた回数に数えない
(assert-equal '(inspect-quit 0 0) (car (ins-run 5 42 '("" "q"))))

;; 値が nil の子にも入れる(「無い」と混ざらない)。
;; 子は (名前 . 値) の cons なので、無いときだけ nil になる形にしてある
(assert-equal '(inspect-quit 1 1) (car (ins-run 5 '(nil) '("0" "q"))))

;; 番号は複数桁も読む(自前で数字だけを見て組み立てている)
(assert-equal '(inspect-quit 1 1)
              (car (ins-run 20 '(0 1 2 3 4 5 6 7 8 9 10 11 12) '("12" "q"))))

;;; ---------------------------------------------------------------------------
;;; 5. 【陽性対照】循環した構造を渡しても止まらない
;;;
;;;    #214 の完了条件そのもの(「自分を含むリストを渡す」)。
;;;    印字系は循環も深さも長さも**一切見張っていない**(print.c:343-361)し、
;;;    `LENGTH` も循環した列では戻ってこない(runtime.c:11239-11245)。
;;;    止まらない理由は 3 つで、ここで全部見える:
;;;      (a) 1 行要約を自前で組んでいる(印字系へ合成値を渡さない)
;;;      (b) 長さを上限つきに数えている
;;;      (c) 1 段しか出さない
;;;
;;;    **この節が通ったということは止まらなかったということである**(止まれば
;;;    段がここで固まり、#count が出ないので検出器が「途中で止まった」と断定する。
;;;    test/lisp/test_framework.lisp の #at と #count の不変条件)。
;;; ---------------------------------------------------------------------------

;; 自分を含むリスト (1 <self>)
(defglobal *ins-self* (list 1 nil))
(defglobal *ins-self-wired* (set-car (cdr *ins-self*) *ins-self*))

;; 要約は返ってくる。長さも出る(2 要素で、2 つめが自分)
(assert-equal "<CONS> len=2" (inspect-brief *ins-self*))
;; 1 段ぶんの表示も返ってくる。**自分へ戻る枝に [seen] が付く**
;; (既訪判定は MEMBER の eq 比較。init_aot.lisp:25-30)
(assert-equal (string-append
               "== <CONS> len=2" *ins-nl*
               " 0: <FIXNUM> 1" *ins-nl*
               " 1: <CONS> len=2 [seen]" *ins-nl*)
              (ins-show 5 *ins-self*))

;; 自分の中へ 5 回入っても戻ってくる。**深さの上限を置いていないのに終わる**のは、
;; 1 命令で 1 段しか進まないからである(入力が尽きれば必ず終わる)
(assert-equal '(inspect-quit 5 5) (car (ins-run 2 *ins-self* '("1" "1" "1" "1" "1" "q"))))
;; 入力を尽かせても終わる
(assert-equal '(inspect-eof 3 3) (car (ins-run 2 *ins-self* '("1" "1" "1"))))

;; cdr の輪が閉じた列 (1 2 1 2 ...)。**件数は上限で打ち切られ、「以上」と出る**
(defglobal *ins-ring* (list 1 2))
(defglobal *ins-ring-wired* (set-cdr (cdr *ins-ring*) *ins-ring*))
(assert-equal "<CONS> len>=3" (dynamic-let ((*inspect-max-items* 3))
                                (inspect-brief *ins-ring*)))
(assert-equal "<CONS> len>=7" (dynamic-let ((*inspect-max-items* 7))
                                (inspect-brief *ins-ring*)))
;; 子の並びも上限で切れる(要素数は無限にあるが 3 件しか出ない)
(assert-equal 3 (dynamic-let ((*inspect-max-items* 3))
                  (ins-count (%inspect-children *ins-ring*))))
;; 1 段ぶんの表示も返ってくる
(assert-equal '(inspect-quit 0 0) (car (ins-run 3 *ins-ring* '("q"))))

;; ベクタの中に自分が入っている形でも同じ
(defglobal *ins-vself* (vector 1 nil))
(defglobal *ins-vself-wired* (set-aref *ins-vself* *ins-vself* 1))
(assert-equal "<GENERAL-VECTOR> len=2" (inspect-brief *ins-vself*))
(assert-equal t (if (string-index "[seen]" (ins-show 5 *ins-vself*)) t nil))
(assert-equal '(inspect-quit 2 2) (car (ins-run 3 *ins-vself* '("1" "1" "q"))))

;;; ---------------------------------------------------------------------------
;;; 6. 【陽性対照】任意の値を渡しても壊れない
;;;
;;;    #214 の完了条件「無検査の窓口を守れていることを示す」。§1 が窓口 1 つずつの
;;;    対照で、ここは**道具を丸ごと通した**対照である。
;;;    どの種類の値を渡しても 1 行要約が返り、子の並びが引ける。
;;; ---------------------------------------------------------------------------

;; 1 行要約が空でない文字列として返るか
(defun ins-brief-ok-p (v)
  (let ((s (inspect-brief v)))
    (if (stringp s) (> (length s) 0) nil)))

;; 子の並びが引けて、その全員の要約も返るか。**子の数は上限で切れている**ので
;; ここの繰り返しは必ず終わる
(defun ins-kids-ok-p (v)
  (let ((cur (%inspect-children v)) (ok t))
    (while (consp cur)
      (if (consp (car cur))
          (if (ins-brief-ok-p (cdr (car cur))) nil (setq ok nil))
        (setq ok nil))
      (setq cur (cdr cur)))
    ok))

(defun ins-value-ok-p (v)
  (if (ins-brief-ok-p v) (ins-kids-ok-p v) nil))

(defun ins-all-ok-p (lst)
  (let ((cur lst) (ok t))
    (while (consp cur)
      (if (ins-value-ok-p (car cur)) nil (setq ok nil))
      (setq cur (cdr cur)))
    ok))

;; **種類を取りこぼさないこと自体が対照である。** 下の並びには
;; 無検査の 3 窓口が word1/word2 を生で読む種類(分類・実体)と、
;; 読んだら困る種類(fixnum / nil / 文字 / 文字列 / 関数 / 流れ口)の両方が入っている
(defglobal *ins-values*
  (list 42 -1 0 nil t 'foo "" "abc" #\a #\newline
        '(1 2) '(1 . 2) (vector) (vector 1 2) (create-array '(2 2) 0)
        (class-of 1) (class-of *ins-p*) *ins-p*
        (function car) (function ins-fn)
        (create-string-input-stream "x") (create-string-output-stream)
        (* 1000000000 1000000000)))
(assert-equal 23 (ins-count *ins-values*))
(assert-equal t (ins-all-ok-p *ins-values*))

;; 対話の輪も同じ並びで回る(1 つずつ q で抜ける)
(defun ins-run-all-ok-p (lst)
  (let ((cur lst) (ok t))
    (while (consp cur)
      (if (equal '(inspect-quit 0 0) (car (ins-run 3 (car cur) '("q")))) nil (setq ok nil))
      (setq cur (cdr cur)))
    ok))
(assert-equal t (ins-run-all-ok-p *ins-values*))

;; 大きな整数は印字系が 1 行で返す種類なので、そのまま渡している。
;; **先に本当に bignum であることを押さえる**(fixnum のマグニチュードは
;; bit4-62 の 59 ビット = 最大 約 5.76e17 なので 1e18 は収まらない。runtime.h:127)
(assert-equal t (bignump (* 1000000000 1000000000)))
(assert-equal "<BIGNUM> 1000000000000000000" (inspect-brief (* 1000000000 1000000000)))

;;; ---------------------------------------------------------------------------
;;; 7. 【完了条件】上限が効いている(巨大構造の歯止め)
;;;
;;;    **出す量は *inspect-max-items* だけで決まる。**
;;;    効いていなければ 3 本とも同じ件数になる(規則 8 の対照)。
;;; ---------------------------------------------------------------------------

;; 既定値を値そのもので固定する(画面から読んでいない。読む口が無い)
(assert-equal 12 (dynamic *inspect-max-items*))
(assert-equal 32 (dynamic *inspect-brief-chars*))

(defglobal *ins-long* (create-array '(100) 0))
(assert-equal 100 (length *ins-long*))
(assert-equal 2 (dynamic-let ((*inspect-max-items* 2)) (ins-count (%inspect-children *ins-long*))))
(assert-equal 5 (dynamic-let ((*inspect-max-items* 5)) (ins-count (%inspect-children *ins-long*))))
(assert-equal 100 (dynamic-let ((*inspect-max-items* 500)) (ins-count (%inspect-children *ins-long*))))
;; 0 以下は 1 に丸める。丸めないと「1 件も出ないのに続きがある」表示になる
(assert-equal 1 (dynamic-let ((*inspect-max-items* 0)) (ins-count (%inspect-children *ins-long*))))

;; **束縛を抜けたら既定値へ戻っている**(dynamic-let なので漏れない)
(assert-equal 12 (dynamic *inspect-max-items*))
