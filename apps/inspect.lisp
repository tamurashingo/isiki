;;;; apps/inspect.lisp
;;;;
;;;; **値の構造を 1 段ぶん列挙し、番号でたどれるようにする道具**(issue #214)。
;;;; 調査は docs/investigation/app-inspect.{logic,refs}.md。
;;;;
;;;; ## 組み込みは 0 個
;;;;
;;;; 分類・実体・欄・関数・名前の内省は**すでに全部診断用の窓口として外に出ている**
;;;; (調査 logic §3-1)。利用者向けの道具が `%%` を直接呼ぶ前例もある
;;;; (src/lisp/disassemble.lisp:44/:52/:65-66)。だからこのファイルは
;;;; **C を 1 行も触らず、新しい窓口を 1 つも足していない。**
;;;;
;;;; ## 置き場所
;;;;
;;;; **アプリ専用の FAT32 ディスク(Makefile の APP_DISK_IMG)の根。**
;;;; apps/pager.lisp / apps/hello.lisp と同じ約束で、ホスト側の原本がこの apps/ に
;;;; あり、ビルドがイメージへ焼き込む。ゲストからは
;;;;   (mount "/app" (%device-named "ide-primary-slave") ':fat32)
;;;;   (load "/app/inspect.lisp")
;;;;
;;;; **常駐費用はゼロである。** apps/*.lisp は
;;;; `python3 tools/gen_embedded_lisp.py --print-inputs`(同梱の一覧)にも
;;;; Makefile:63 の `TRANSPILE_LISP_SRC`(AOT の一覧)にも**入っていない**。
;;;; したがって起動イメージも起動時間も不動領域も 1 バイトも増えない(規則 13: この
;;;; 2 つのコマンドで導出できる)。
;;;;
;;;; **9P の無い起動でも読まれうる**ので、ここで使えるのはカーネルへ同梱した
;;;; 基盤ライブラリ(src/lisp/init.lisp)と AOT 済みの層(init_aot.lisp など)、
;;;; それに C の窓口だけである(ホスト共有を load してはならない)。
;;;;
;;;; ## 守っていること(issue #214 / 調査が見つけた落とし穴)
;;;;
;;;; 1. **無検査の窓口を素で呼ばない。** `%%CLASS-NAME`(runtime.c:11570)/
;;;;    `%%INSTANCE-CLASS`(:11694)/ `%%INSTANCE-SLOTS`(:11706)は
;;;;    **型検査が無く word1/word2 を生で読む**。任意の値を渡すとヒープ外を読む。
;;;;    下の `%inspect-class-name` / `%inspect-instance-class` /
;;;;    `%inspect-instance-slots` の 3 つが**唯一の呼び出し口**で、
;;;;    どれも `%%CLASSP` / `%%CLASS-INSTANCE-P` で先に判定する
;;;; 2. **合成した値を `~A`/`~S` に渡さない。** 印字系は循環も深さも長さも
;;;;    一切見張っていない(print.c:343-361 / :426)。再帰するのは
;;;;    **cons と ベクタ/配列の 2 系統だけ**(print.c:452-502 を読むと、
;;;;    それ以外の種類はどれも長さの決まった 1 行を書くだけである)。だから
;;;;    `%inspect-safe-to-print-p` が真の値しか印字系へ渡さない
;;;; 3. **cons に `LENGTH` を使わない。** 既存の長さは終端に出会うまで止まらない
;;;;    (runtime.c:11239-11245)。`%inspect-cons-count` が上限つきに数える
;;;; 4. **戻り値を小さい値に限る。** 対話の輪は結果を**必ず**印字する
;;;;    (repl.c:67/:70)ので、見ていた値を返したら循環した構造でそこで止まる。
;;;;    `inspect-run` が返すのは記号 1 つと fixnum 2 つだけである
;;;; 5. **`(format t ...)` を使っていない。** 第 1 引数は MAGIC_STREAM を要求する
;;;;    (format.c:164-168 → stream_lisp.c:139)。出力先は常に引数で受け取った
;;;;    ストリームで、薄い皮だけが `(standard-output)` を呼ぶ
;;;; 6. **桁揃えに `~nA` を使っていない。** num は A/S では無視される
;;;;    (format.c:204-215)。番号は `~D` で直に出し、幅は揃えない
;;;; 7. **読み取り口は 1 つ持ち回る。** `(standard-input)` は皮の中で 1 回だけ呼ぶ。
;;;;    呼び直すと**先読みの 1 文字が毎回捨てられる**(stream_lisp.c:60-68 /
;;;;    :264-295)うえ、1 回あたり約 1.6KB 確保する
;;;; 8. **名前は `inspect`。** 一般名の `describe` は**すでに装置専用**で埋まっている
;;;;    (src/lisp/device.lisp:221-231)
;;;; 9. **`'eval-error` を値で返していない**(残数に上限がある。
;;;;    tools/check_eval_error_budget.py)
;;;;
;;;; ## 核と皮を分けた理由(これが設計の要)
;;;;
;;;; 核 `inspect-run` は**値・合図の入力・表示先の 3 つを受け取る。**
;;;; 文字列入力ストリームを渡せば**キー入力なしで試験できる**(apps/pager.lisp と
;;;; 同じ形)。試験は test/lisp/inspect_test.lisp。
;;;;
;;;; ## 第 1 版でやらないこと(調査 §11「足りないもの」)
;;;;
;;;; - **解釈実行の関数の中は見せない。** 仮引数の並び(word1)と本体(word2)を
;;;;   読む窓口が 1 つも無く、値から番地を得る手段も無い
;;;;   (refs §9-10)。種類・機械語化されているか・機械語の長さ・
;;;;   optimize 指定・宣言された仮引数の型は取れるので、それを出す
;;;; - **動的な名前の一覧と、使われている名前の総覧は出さない。**
;;;;   これは探索ではなく名前検索の仕事なので別件
;;;;
;;;; ## 出力は ASCII だけ
;;;;
;;;; 画面(framebuffer)は 1 バイト 1 文字の格子なので、**出力に多バイト文字を
;;;; 置かない。** 既存の src/lisp/disassemble.lisp / utility.lisp の出力も同じで、
;;;; 日本語はコメントにだけ書く。

;;; ---------------------------------------------------------------------------
;;; つまみ 2 つ
;;;
;;; **初期宣言は安全な形で書く**(src/lisp/init.lisp:898-901 の作法。#187)。
;;; defdynamic は value-form を評価して**無条件に上書きする**ので、素の初期値を
;;; 書くと「2 回目の load」が利用者の設定を無言で消す。この `or` の形は
;;; **「この変数は nil を意味のある値として持たない」**ことに依存している
;;; (件数と桁数に nil は無い)。
;;;
;;; *inspect-max-items* … 1 段で列挙する要素の上限。**これが巨大構造の歯止め**で、
;;;   100 万要素のベクタを渡しても出るのはこの件数ぶんだけである。
;;;   既定を 12 にしたのは、見出し 1 行 + 12 行が実画面に収まるからである。
;;;   **[#216] ここは ★ ではなくなった。** 行数を読む口(`term-rows`)が入ったので、
;;;   「既定の 12 が実画面に収まる」ことを機械で確かめられる
;;;   (test/lisp/term_test.lisp)。**既定値そのものは変えていない**:
;;;   1 段ぶんの見出しの行数は値の種類で変わるので、画面行数から逆算した値を
;;;   入れても「必ず収まる」とは言えない。収まることを**試験で見る**ほうが確かである
;;; *inspect-brief-chars* … 1 行要約に出す文字列の最大文字数。
;;;   **文字列入力ストリームへ出すときは器が 1024 バイトで黙ってあふれる**
;;;   (stream.h:123)ので、1 行を短く保つのは試験の都合でもある
(defdynamic *inspect-max-items* (or (dynamic *inspect-max-items*) 12))
(defdynamic *inspect-brief-chars* (or (dynamic *inspect-brief-chars*) 32))

;;; ---------------------------------------------------------------------------
;;; 無検査の 3 窓口への**唯一の呼び出し口**
;;;
;;; ここを通さずに `%%CLASS-NAME` / `%%INSTANCE-CLASS` / `%%INSTANCE-SLOTS` を
;;; 書いてはならない。3 つとも型検査が無く、クラスでも実体でもない値を渡すと
;;; **ヒープ外のワードがそのまま Lisp 値として流れる**(runtime.c:11570/:11694/
;;; :11706)。過去に `(%%class-slots nil)` が 6931512 を返し、それが
;;; `(make-array 6931512)` に使われてヒープを使い切った記録が
;;; runtime.c:11610-11625 のコメントに残っている。
;;;
;;; **守れていることは試験で見える**: 分類でも実体でもない値を渡すと nil が返る
;;; (test/lisp/inspect_test.lisp §1)。守りを外せば、返るのは生のワードである。
(defun %inspect-class-name (c)
  (if (%%classp c) (%%class-name c) nil))

(defun %inspect-instance-class (x)
  (if (%%class-instance-p x) (%%instance-class x) nil))

(defun %inspect-instance-slots (x)
  (if (%%class-instance-p x) (%%instance-slots x) nil))

;;; 実体の欄の定義の並び。`%%CLASS-SLOTS` には検査があるが(runtime.c:11610)、
;;; **それでも分類かどうかを先に見る。** `%%INSTANCE-CLASS` が分類でない値を
;;; 返す形の実体を渡されたときに、domain-error を signal させないためである
;;; (探索の道具が壊れた値を見せられて落ちるのは筋が悪い)。
(defun %inspect-instance-slot-descriptors (x)
  (let ((cls (%inspect-instance-class x)))
    (if (%%classp cls) (%%class-slots cls) nil)))

;;; ---------------------------------------------------------------------------
;;; 値の種類の名前
;;;
;;; `CLASS-OF`(init_aot.lisp:495)は組み込みの種類も含めて必ず何かを返すが、
;;; **その名前が *classes* に未登録なら nil を返しうる**(%find-class が nil)。
;;; だから `%inspect-class-name` の守りを通し、取れなければ `<UNKNOWN>` と出す。
(defun inspect-kind (v)
  (let ((n (%inspect-class-name (class-of v))))
    (if (null n) '<unknown> n)))

;;; ---------------------------------------------------------------------------
;;; 印字系へ渡してよい値か
;;;
;;; print.c:426-503 を読むと、`print_value` が**再帰するのは 2 か所だけ**である:
;;;   - TAG_CONS → print_list(print.c:343-361。循環も長さも見張らない)
;;;   - MAGIC_VECTOR → print_vector(print.c:388-418。同じ)
;;; それ以外(fixnum / bignum / 浮動小数 / 文字 / 文字列 / 記号 / 流れ口 /
;;; 作業単位 / 分類 / 分類の実体 / 関数)は**どれも長さの決まった 1 行**を書く。
;;; 分類と実体は中の名前を 1 つだけ印字するが、それは記号なのでそこで止まる。
;;;
;;; nil は TAG_CONS を持つ(runtime.c:3723-3736)が print.c:427 で先に除かれるので
;;; 安全である。**先に null を見る順序を崩さないこと。**
(defun %inspect-safe-to-print-p (v)
  (cond ((null v) t)
        ((consp v) nil)
        ((general-vector-p v) nil)
        ((general-array*-p v) nil)
        (t t)))

;;; ---------------------------------------------------------------------------
;;; cons の長さを上限つきに数える
;;;
;;; **既存の `LENGTH` を cons に使ってはならない。** 終端に出会うまで止まらないので
;;; 循環した列を渡すと戻ってこない(runtime.c:11239-11245)。
;;;
;;; 返り値は (件数 終わり方) の 2 つ組。終わり方は 3 通り:
;;;   nil-end … 素直に nil で終わった(= 件数が本当の長さ)
;;;   dotted  … 尾が cons でない(ドット対)。件数は car を数えた数
;;;   limit   … 上限に達した。**循環した列はここに入る**(それ以上は見ない)
(defun %inspect-cons-count (v limit)
  (let ((n 0) (cur v) (how 'limit) (going t))
    (while going
      (cond ((null cur) (setq how 'nil-end) (setq going nil))
            ((not (consp cur)) (setq how 'dotted) (setq going nil))
            ((>= n limit) (setq how 'limit) (setq going nil))
            (t (setq n (+ n 1)) (setq cur (cdr cur)))))
    (list n how)))

;;; いま有効な上限(1 未満は 1 に丸める)。
;;; 丸めないと「1 件も出ないのに続きがある」表示になって何も分からない。
(defun %inspect-limit ()
  (let ((n (dynamic *inspect-max-items*)))
    (if (< n 1) 1 n)))

;;; ---------------------------------------------------------------------------
;;; 1 行要約を out へ書く(改行は出さない)
;;;
;;; **ここが「自前の 1 行要約」である**(issue #214 の避け方その 1)。
;;; 合成した値は一度も `~A`/`~S` へ渡さない。渡すのは
;;;   - 記号(種類の名前・欄の名前)
;;;   - `%inspect-safe-to-print-p` が真の原子的な値
;;;   - 自分が作った ASCII の文字列
;;;   - fixnum(`~D`。**`~D` は TAG_INSTANCE を渡すと印字系へ丸投げする**ので
;;;     format.c:168-180 の format_integer_write より、fixnum しか渡さない)
;;; の 4 つだけである。

;;; 文字列の中身を 1 行に収まる形で出す。
;;; **行送りや制御文字をそのまま流してはならない**(1 行要約が複数行になる)。
;;; 画面に出せる範囲(0x20〜0x7E)の外は '.' に替える。
;;; 文字の番号を取る窓口が無いので、`CHAR<` / `CHAR>` の 2 回の比較で判定する。
(defun %inspect-write-string-body (out s limit)
  (let ((len (length s)) (i 0))
    (while (and (< i len) (< i limit))
      (let ((c (elt s i)))
        (format out "~C" (if (or (char< c #\space) (char> c #\~)) #\. c)))
      (setq i (+ i 1)))
    ;; 切ったことを隠さない。**隠すと「短い文字列」と見分けがつかない**
    (if (> len limit) (format out "...") nil)
    len))

;;; 関数の 1 行要約。
;;; 種別は印字系に任せる(`~S` が `#<FUNCTION-BUILTIN>` /
;;; `#<FUNCTION-COMPILED>` / `#<FUNCTION-INTERPRETED>` を出す。print.c:495-500)。
;;; **これは合成値ではないので渡してよい。**
;;; 機械語の長さは `%%DISASM-CODE-LEN`(disasm_lisp.c:227)。機械語を持たない関数
;;; (組み込み primitive・AOT・解釈実行)では nil が返るので、そのときは出さない。
(defun %inspect-write-function (out v)
  (format out "~S" v)
  (let ((len (%%disasm-code-len v)))
    (if (null len) nil (format out " code=~D" len))))

;;; 1 行要約の本体。**改行を出さない。**
(defun %inspect-brief-to (out v)
  (format out "~A" (inspect-kind v))
  (let ((lim (%inspect-limit)))
    (cond
      ;; nil は cons でも記号でもあるので**必ず最初に**除ける(§印字の順序と同じ)
      ((null v) nil)
      ((consp v)
       (let ((c (%inspect-cons-count v lim)))
         (cond ((eq (car (cdr c)) 'nil-end) (format out " len=~D" (car c)))
               ((eq (car (cdr c)) 'dotted) (format out " len=~D+tail" (car c)))
               ;; **循環した列はここに出る。** 「~D 件までは見た」としか言わない
               (t (format out " len>=~D" (car c))))))
      ((stringp v)
       (format out " \"")
       (%inspect-write-string-body out v (dynamic *inspect-brief-chars*))
       (format out "\" len=~D" (length v)))
      ;; ベクタと配列の `LENGTH` は要素数を数え上げるだけで**循環しない**
      ;; (runtime.c:11247-11259。次元の積を返す)
      ((general-vector-p v) (format out " len=~D" (length v)))
      ((general-array*-p v) (%inspect-write-dimensions out v))
      ;; 分類そのもの。種類の名前は `<STANDARD-CLASS>` 等なので、**自分の名前**を足す
      ((%%classp v) (format out " ~A" (%inspect-class-name v)))
      ;; 実体。種類の名前がすでに所属分類の名前なので、欄の数だけ足す
      ((%%class-instance-p v)
       (format out " slots=~D"
               (car (%inspect-cons-count (%inspect-instance-slot-descriptors v) lim))))
      ((symbolp v) (format out " ~A" (symbol-name v)))
      ((functionp v) (format out " ") (%inspect-write-function out v))
      ;; 残り(fixnum / bignum / 浮動小数 / 文字 / 流れ口 / 作業単位)は
      ;; **印字系が 1 行で返すことを確かめた種類**だけなので、そのまま渡す
      ((%inspect-safe-to-print-p v) (format out " ") (format out "~S" v))
      ;; ここへは来ない(上の cond が合成値を全部拾っている)が、
      ;; **来たときに合成値を印字系へ渡さない**ための最後の砦として置く
      (t (format out " ?")))))

;;; 配列の形。`ARRAY-DIMENSIONS`(runtime.c:3975)は次元の並び(cons の列)を返すので、
;;; **そのまま `~A` へ渡してはならない。** 要素を 1 つずつ `~D` で出す。
(defun %inspect-write-dimensions (out v)
  (format out " dims=")
  (let ((d (array-dimensions v)) (first t))
    (while (consp d)
      (if first (setq first nil) (format out "x"))
      ;; 次元は fixnum である(primitive_array_dimensions)。
      ;; 念のため fixnum でなければ番号は出さず印だけ出す
      (if (fixnump (car d)) (format out "~D" (car d)) (format out "?"))
      (setq d (cdr d))))
  (format out " len=~D" (length v)))

;;; 1 行要約を**文字列として**返す。試験と、短い値の使い回しのための便宜。
;;; `%room-digit-string`(src/lisp/utility.lisp:20)と同じ手で、
;;; 文字列の器へ書いて取り出す。器は 1024 バイト(stream.h:123)あり、
;;; 1 行要約は *inspect-brief-chars* で押さえているので**あふれない**。
(defun inspect-brief (v)
  (let ((s (create-string-output-stream)))
    (%inspect-brief-to s v)
    (get-output-stream-string s)))

;;; ---------------------------------------------------------------------------
;;; 1 段ぶんの子の並びを作る
;;;
;;; 返り値は `(名前 . 値)` の列。名前が nil なら「番号だけ」の意味である。
;;; **列は必ず上限で切れる。** これが巨大構造・循環した構造の歯止めで、
;;; 「中に入る」ときに番号で選ぶ対象でもある。
;;;
;;; cond の順序は `%inspect-safe-to-print-p` と同じ理由で**null が先**。

;;; cons の列。**上限まで car を拾う。** 尾が cons でなければ(ドット対)
;;; その尾を `tail` という名前の子にする(そこから先へも入れるように)。
(defun %inspect-children-cons (v limit)
  (let ((acc nil) (cur v) (n 0) (going t))
    (while going
      (cond ((null cur) (setq going nil))
            ((not (consp cur)) (setq acc (cons (cons 'tail cur) acc)) (setq going nil))
            ((>= n limit) (setq going nil))
            (t (setq acc (cons (cons nil (car cur)) acc))
               (setq n (+ n 1))
               (setq cur (cdr cur)))))
    (reverse acc)))

;;; 文字列・ベクタ・配列。**`ELT` が 3 種に共通で効く**(runtime.c:11281-11323)。
;;; `LENGTH` はこの 3 種では数え上げるだけなので使ってよい(cons ではない)。
;;; 配列は次元に関わらず 1 次元に平らに見える(ELT / LENGTH の既知の簡略化)。
(defun %inspect-children-seq (v limit)
  (let ((len (length v)) (acc nil) (i 0))
    (while (and (< i len) (< i limit))
      (setq acc (cons (cons nil (elt v i)) acc))
      (setq i (+ i 1)))
    (reverse acc)))

;;; 分類。親の並びと欄の定義の並びを、合わせて上限まで出す。
;;; 欄の定義は `(欄の名前 initarg 既定値を作る手続き)` で、名前は `(car d)`
;;; (src/lisp/init.lisp:401-404)。**同名の欄が 2 つ並びうる**
;;; (親から単純連結で、上書きしない。init_aot.lisp:423-427)ので、
;;; 選ぶのは名前ではなく**番号**にしてある。
(defun %inspect-children-class (v limit)
  (let ((acc nil) (n 0) (sup (%%class-supers v)) (slots (%%class-slots v)))
    (while (and (consp sup) (< n limit))
      (setq acc (cons (cons 'super (car sup)) acc))
      (setq n (+ n 1))
      (setq sup (cdr sup)))
    (while (and (consp slots) (< n limit))
      (setq acc (cons (cons 'slot-def (car slots)) acc))
      (setq n (+ n 1))
      (setq slots (cdr slots)))
    (reverse acc)))

;;; 実体。欄の定義の並びから名前を取り、欄の値の並び(ベクタ)を `ELT` で引く。
;;; **位置は 1 対 1 である**(`%slot-index` が定義の並びの位置をそのまま
;;; ベクタの添字に使う。init_aot.lisp:160-170)。
;;; 定義の並びとベクタの長さが食い違っても、**短い方で止める**。
(defun %inspect-children-instance (v limit)
  (let ((descs (%inspect-instance-slot-descriptors v))
        (vec (%inspect-instance-slots v))
        (acc nil) (i 0))
    (let ((len (if (general-vector-p vec) (length vec) 0)))
      (while (and (consp descs) (< i len) (< i limit))
        (setq acc (cons (cons (car (car descs)) (elt vec i)) acc))
        (setq i (+ i 1))
        (setq descs (cdr descs))))
    (reverse acc)))

;;; 記号。名前・最上位環境での変数束縛と手続き束縛・動的な値・属性を出す。
;;;
;;; **`(function name)` を使わない。** 未定義なら `<undefined-function>` を
;;; signal する(eval.c:535-548)ので、任意の記号を受け取る道具では使えない。
;;; 代わりに**環境をただの対の列として読む**(runtime.c:4643-4668 のレイアウト。
;;; 先頭 4 つの位置は後方互換のため固定されている):
;;;   1 番目 (name . 環境名) / 2 番目 (variables . alist) /
;;;   3 番目 (functions . alist) / 4 番目 (parent . 親環境)
;;; **既存の `ASSOC`(init_aot.lisp:34-39。eq 比較)を使わず、繰り返しで歩く。**
;;; あの `assoc` は**再帰**で書かれていて、最上位環境の手続きの対応表は
;;; それが安心して通る長さではない: C から登録される名前だけで
;;;   grep -rho 'os_set_function(os_make_symbol("[^"]*")' src/c/*.c \
;;;     | sed 's/.*os_make_symbol("//;s/")//' | sort -u | wc -l
;;; が 647 件あり、基盤ライブラリの defun がその上に積まれる(規則 13: この行で
;;; 導出できる)。1000 段を超える再帰を探索の道具のために積む理由は無い。
;;;
;;; 動的な値は `(dynamic sym)` を最上位環境で評価して取る。**名前が実行時に
;;; 決まるので特別形式を直に書けず**、`%%EVAL-IN-ENVIRONMENT` を通す
;;; (refs §10)。未束縛なら nil が返るだけである(eval.c:410-413)。
(defun %inspect-alist-find (key alist)
  (let ((cur alist) (found nil))
    (while (and (consp cur) (null found))
      (let ((e (car cur)))
        (if (and (consp e) (eq (car e) key)) (setq found e) nil))
      (setq cur (cdr cur)))
    found))

(defun %inspect-symbol-binding (sym slot-index)
  (let ((env (%%global-environment)))
    ;; 環境は cons の列。添字は 1(variables)か 2(functions)しか渡さない
    (let ((pair (%inspect-alist-find sym (cdr (elt env slot-index)))))
      (if (null pair) nil (cons t (cdr pair))))))

(defun %inspect-symbol-dynamic (sym)
  (%%eval-in-environment (list 'dynamic sym) (%%global-environment)))

;;; 属性表は `((記号 . 属性名) . 値)` の alist(src/lisp/init.lisp:901/:929)。
;;; sym のぶんだけを上限つきに拾う。**表全体は循環しないが長いので上限は要る。**
(defun %inspect-children-properties (sym limit)
  (let ((entries (dynamic *symbol-properties*)) (acc nil) (n 0))
    (while (and (consp entries) (< n limit))
      (let ((e (car entries)))
        (if (and (consp e) (consp (car e)) (eq (car (car e)) sym))
            (progn (setq acc (cons (cons (cdr (car e)) (cdr e)) acc))
                   (setq n (+ n 1)))
          nil))
      (setq entries (cdr entries)))
    (reverse acc)))

(defun %inspect-children-symbol (v limit)
  (let ((acc (list (cons 'name (symbol-name v))))
        (var (%inspect-symbol-binding v 1))
        (fn (%inspect-symbol-binding v 2))
        (dyn (%inspect-symbol-dynamic v)))
    ;; 束縛が無い場合と「nil に束縛されている」場合を混ぜないため、
    ;; %inspect-symbol-binding は (t . 値) か nil を返す形にしてある
    (if (null var) nil (setq acc (cons (cons 'value (cdr var)) acc)))
    (if (null fn) nil (setq acc (cons (cons 'function (cdr fn)) acc)))
    (if (null dyn) nil (setq acc (cons (cons 'dynamic dyn) acc)))
    (append (reverse acc) (%inspect-children-properties v limit))))

(defun %inspect-children (v)
  (let ((limit (%inspect-limit)))
    (cond ((null v) nil)
          ((consp v) (%inspect-children-cons v limit))
          ((stringp v) (%inspect-children-seq v limit))
          ((general-vector-p v) (%inspect-children-seq v limit))
          ((general-array*-p v) (%inspect-children-seq v limit))
          ((%%classp v) (%inspect-children-class v limit))
          ((%%class-instance-p v) (%inspect-children-instance v limit))
          ((symbolp v) (%inspect-children-symbol v limit))
          ;; **関数は中を見せない**(第 1 版の割り切り。仮引数と本体を読む窓口が無い)。
          ;; 流れ口・数・文字も 1 段ぶんの中身を持たない
          (t nil))))

;;; 番号で 1 つ選ぶ。範囲外なら nil。
;;; **`ELT` を cons に使わない**のは、範囲外で index-out-of-range を signal する
;;; (runtime.c:11287-11291)ためである。自分で歩けば「無い」を値で返せる。
(defun %inspect-nth-child (kids idx)
  (let ((cur kids) (i 0) (found nil))
    (while (and (consp cur) (null found))
      (if (= i idx) (setq found (car cur)) nil)
      (setq i (+ i 1))
      (setq cur (cdr cur)))
    found))

;;; ---------------------------------------------------------------------------
;;; 1 段ぶんを出す
;;;
;;; `path` は**いま見ている値から根へ向かう cons の列**(先頭が親)。
;;; 子が `path` に eq で居れば `[seen]` を付ける。既訪判定は `MEMBER`
;;; (eq 比較。init_aot.lisp:25-30)で、**循環した構造を渡したときに
;;; 「ここは戻ってきている」と分かる唯一の印**である。
;;;
;;; 出す量は `*inspect-max-items*` で決まる。**ここが巨大構造の歯止めで、
;;; 表示が止まらなくなる道は残っていない。**
(defun inspect-show (v path out)
  (format out "== ")
  (%inspect-brief-to out v)
  (format out "~%")
  ;; 関数だけは 1 段ぶんの子を持たないので、代わりに宣言を 2 行で出す
  (if (functionp v) (%inspect-show-function out v) nil)
  (let ((kids (%inspect-children v)) (i 0))
    (while (consp kids)
      (let ((kid (car kids)))
        (format out " ~D" i)
        (if (null (car kid)) nil (format out " ~A" (car kid)))
        (format out ": ")
        (%inspect-brief-to out (cdr kid))
        ;; 自分自身を含むリストを渡すと、ここに [seen] が出る
        (if (or (eq (cdr kid) v) (member (cdr kid) path)) (format out " [seen]") nil)
        (format out "~%"))
      (setq i (+ i 1))
      (setq kids (cdr kids)))
    (if (= i 0) (format out " (no parts)~%") nil))
  (if (null path) nil (format out " (up available)~%"))
  nil)

;;; 関数について外から取れるもの。**中(仮引数と本体)は取れない**(§第 1 版)。
;;; `%%OPTIMIZE-OF`(runtime.c:3951)は (speed safety space) の 3 つ組、
;;; `%%DECLARED-TYPES-OF`(:3956)は仮引数の型の記号の列。**どちらも cons の列
;;; なので `~A` に渡さず、要素を 1 つずつ出す。** 機械語化されていない関数では
;;; どちらも nil(前の内容を読まないことは runtime.c:2876 のコメントにある)。
(defun %inspect-show-function (out v)
  (format out " jit: ~A~%" (if (%%za-compiled-p v) 'yes 'no))
  (format out " optimize:")
  (%inspect-write-atom-list out (%%optimize-of v))
  (format out "~%")
  (format out " declared-types:")
  (%inspect-write-atom-list out (%%declared-types-of v))
  (format out "~%"))

;;; 記号と fixnum だけが並ぶ短い列を 1 行に出す。
;;; **合成値が混じっていたら印字系へ渡さず `?` にする**(ここは守りである)。
(defun %inspect-write-atom-list (out lst)
  (let ((cur lst) (n 0) (limit (%inspect-limit)))
    (if (null cur) (format out " (none)") nil)
    (while (and (consp cur) (< n limit))
      (if (%inspect-safe-to-print-p (car cur))
          (format out " ~A" (car cur))
        (format out " ?"))
      (setq n (+ n 1))
      (setq cur (cdr cur)))
    (if (consp cur) (format out " ...") nil)))

;;; ---------------------------------------------------------------------------
;;; 入力の 1 行を命令に読み替える
;;;
;;; **`PARSE-NUMBER` を使わない。** 数でない文字列を渡したときの出方に依らせたく
;;; ないので、数字だけかを自分で見て自分で組み立てる。`CHAR-INDEX`
;;; (runtime.c:3983 登録 / :10xxx 実体)は見つからなければ nil を返すので、
;;; **これだけで「数字か」の判定になる。**
;;;
;;; 返すのは非負の fixnum か nil(数ではない)。
(defun %inspect-parse-index (s)
  (let ((len (length s)) (i 0) (acc 0) (ok t))
    (if (= len 0) (setq ok nil) nil)
    (while (and ok (< i len))
      (let ((d (char-index (elt s i) "0123456789")))
        (if (null d)
            (setq ok nil)
          (progn (setq acc (+ (* acc 10) d))
                 (setq i (+ i 1))))))
    (if ok acc nil)))

;;; ---------------------------------------------------------------------------
;;; 核。**ここが道具の全部で、値とストリームしか触らない**
;;;
;;;   v   … 探索を始める値(**どんな値でもよい**)
;;;   in  … 命令を読む入力ストリーム
;;;   out … 表示先の出力ストリーム
;;;
;;; 命令は 4 つ:
;;;   数字  … その番号の子に入る
;;;   u / U … 1 つ戻る(根なら何もしない)
;;;   q / Q … やめる
;;;   空行  … いまの段をもう一度出す
;;;
;;; 返り値は **(理由 動いた回数 深さ) の 3 つ組**で、記号 1 つと fixnum 2 つだけ。
;;; **見ていた値を返さない。** 対話の輪は結果を必ず印字する(repl.c:67/:70)ので、
;;; 循環した構造を返したらそこで系が止まる(issue #214 の避け方その 3)。
;;;   inspect-quit … q でやめた
;;;   inspect-eof  … 入力が尽きた(文字列入力ストリームでの打ち切りがこれ)
;;;
;;; 道筋(path)は**根へ向かう cons の列**で、戻るのは 1 つ外すだけである。
;;; **循環した構造でも止まらない**: 1 段ずつしか見ず、`%inspect-children` が
;;; 上限で切れ、要約は自前で、長さは上限つきに数えている。
;;; 入れる段の深さに上限は置いていない —— 置かなくても、1 命令で 1 段しか
;;; 進まないので**入力が尽きれば必ず終わる**。
(defun inspect-run (v in out)
  (let ((cur v) (path nil) (moves 0) (depth 0)
        (reason 'inspect-eof) (running t))
    (while running
      (inspect-show cur path out)
      (format out "inspect> ")
      (let ((line (read-line in nil nil)))
        (cond
          ;; 入力が尽きた。**合図を待ち続けない**
          ((null line) (setq reason 'inspect-eof) (setq running nil))
          ((or (string= line "q") (string= line "Q"))
           (setq reason 'inspect-quit) (setq running nil))
          ((or (string= line "u") (string= line "U"))
           (if (null path)
               (format out "(at the top)~%")
             (progn (setq cur (car path))
                    (setq path (cdr path))
                    (setq depth (- depth 1))
                    (setq moves (+ moves 1)))))
          ;; 空行はもう一度出すだけ。**動いた回数には数えない**
          ((string= line "") nil)
          (t (let ((idx (%inspect-parse-index line)))
               (if (null idx)
                   (format out "(type a number, u, or q)~%")
                 (let ((kid (%inspect-nth-child (%inspect-children cur) idx)))
                   ;; 子は (名前 . 値) の cons なので、無いときだけ nil になる
                   ;; (値が nil の子と混ざらない)
                   (if (null kid)
                       (format out "(no such part)~%")
                     (progn (setq path (cons cur path))
                            (setq cur (cdr kid))
                            (setq depth (+ depth 1))
                            (setq moves (+ moves 1)))))))))))
    (list reason moves depth)))

;;; ---------------------------------------------------------------------------
;;; 皮。**これだけが画面とキーボードを知っている**
;;;
;;; `(standard-input)` / `(standard-output)` を**それぞれ 1 回だけ**呼んで核へ渡す。
;;; どちらも未束縛なら新しい流れを作る(= 1 回あたり約 1.6KB の確保)ので、
;;; ループの中で呼んではならない。先読みの 1 文字を落とさないためでもある
;;; (stream_lisp.c:60-68 / :264-295)。
(defun inspect (v)
  (inspect-run v (standard-input) (standard-output)))
