;;;; apps/editor.lisp
;;;;
;;;; **テキストを編集して保存する道具**(issue #218)。
;;;; 設計は issue #218 の「設計(確定版)」のコメント、
;;;; 調査は docs/investigation/app-editor.{logic,refs,predictions}.md。
;;;;
;;;; ## 組み込みは 0 個。**C を 1 行も触っていない**
;;;;
;;;; 調査が「C 側に 4 種の不足」と書いていたのは #216 / #199 より前の話で、
;;;; **4 種とも埋まっている。** 残るのは色・属性(格子が 1 マス 1 バイトなので
;;;; 構造的に無い)と 1 行消去の専用口(空白で塗れば同じ)だけで、どちらも要らない。
;;;;
;;;;   - 画面 … `term-*`(src/lisp/init.lisp。#216)
;;;;   - 打鍵 … `term-read-key`(= `read-char`。素の入力は皮が入切する)
;;;;   - 保存 … `with-open-output-file` + `write-char`(#199 で上書きが直った)
;;;;
;;;; ## 置き場所
;;;;
;;;; **アプリ専用の FAT32 ディスク(Makefile の APP_DISK_IMG)の根 = `/app`。**
;;;; `APP_LISP_SRC := $(wildcard apps/*.lisp)` なので、ここに置くだけで焼かれる。
;;;; **同梱にも AOT にも入っていない**ので、起動イメージ・起動時間・不動領域は
;;;; 1 バイトも増えない(`python3 tools/gen_embedded_lisp.py --print-inputs` と
;;;; Makefile の `TRANSPILE_LISP_SRC` で導出できる。規則 13)。
;;;;
;;;; 使ってよいのはカーネル同梱(`src/lisp/init.lisp`)と AOT 済み
;;;; (`file-cmd.lisp` など)だけである。**ホスト共有を `load` してはならない**
;;;; (9P の無い起動でも読まれうる)。
;;;;
;;;; ## 守っていること(apps/filer.lisp と同じ)
;;;;
;;;; 1. **キーワードは必ずクォートする**(`':foo`)。キーワード型が無い
;;;; 2. **`(format t ...)` を使わない。** 第 1 引数は MAGIC_STREAM を要求する
;;;;    (format.c:164-168)。出力先は常に引数で受け取ったストリーム
;;;; 3. **繰り返しは `while`。** トップレベルの再帰はスタックを壊す
;;;; 4. **`(standard-input)` / `(standard-output)` は皮の中で 1 回だけ**呼ぶ
;;;; 5. **素の入力は `unwind-protect` で戻す**(#215 で実際に踏んだバグ)
;;;;
;;;; ---------------------------------------------------------------------------
;;;; ## 設計の芯 1: **費用は「触った文字数」ではなく「Lisp から呼んだ回数」**
;;;;
;;;; 調査が実機で測った(logic §8-1):
;;;;
;;;;   | 書き方                       | 全画面   | 1 行     | 1 文字 |
;;;;   |------------------------------|---------:|---------:|-------:|
;;;;   | 1 文字ずつ `write-char`      | 505,344  | 10,720   | 608    |
;;;;   | 文字列を `~A` で 1 回で流す  |   7,504  |    560   | 576    |
;;;;
;;;; **1 文字と 1 行がほぼ同じ**(576 対 560)なのが要点である。長さではなく
;;;; **回数**が効いている。だから #218 ではまず `term-write-at`(init.lisp)を
;;;; `(format out "~A" str)` に直した —— **このアプリも apps/filer.lisp も
;;;; そのまま速くなる。** 前後の確保量は test/lisp/editor_app_disk_test.lisp §4 が
;;;; **毎回計り直して記録に残す**(規則 9: 時間ではなく確保量)。
;;;;
;;;; ## 設計の芯 2: **行 = その都度作り直す不変の文字列。文書 = 行をつないだ鎖**
;;;;
;;;; 入れ物を使い回して 1 文字ずつずらすのは **6.8 倍高い**(実測。1,024 対
;;;; 6,944 byte。logic §11-1)。ずらしは 1 文字ごとに呼び出しが 2 回起きるが、
;;;; 作り直し(切り出し 2 回 + 連結 1 回)は**長さによらず 3 回**で済むからである。
;;;;
;;;; ### 鎖の持ち方: **上下に分けて持つ**(zipper)
;;;;
;;;; logic §11-2 は「カーソルの行を指す鎖の位置と、その 1 つ前を覚えておくこと」と
;;;; 書いている。**それをそのまま形にしたのがこの持ち方である:**
;;;;
;;;;   above … カーソルより上の行。**逆順**(直前の行が car)
;;;;   line  … カーソルの行(文字列)
;;;;   below … カーソルより下の行(順どおり)
;;;;
;;;; こうすると上下の移動・行の挿入・削除・分割・結合が**すべて繋ぎ替え 1 回**に
;;;; なる。素直な 1 本の鎖 + 添字だと、**カーソルを 1 行上げるだけで先頭から
;;;; 数え直す**ことになり、1,000 行の文書では 1 打鍵あたり 1,000 回の `cdr` ——
;;;; つまり上の表の「全画面を描き直す」より高くつく。それでは §8-1 を直した
;;;; 意味が消える。
;;;;
;;;; 行数(nlines)と行番号(crow)は**数えずに持ち回る。** 数えると O(n) に戻る。
;;;;
;;;; ## 設計の芯 3: **描き直しは「1 行 1 回」。変わった行だけ**
;;;;
;;;;   | やること                     | 描き直す範囲       | 確保量(実測の見込み) |
;;;;   |------------------------------|--------------------|----------------------|
;;;;   | 文字の挿入・削除             | **その行 + 合図**  | 約 1KB               |
;;;;   | カーソルの左右上下           | **合図だけ**       | 約 0.6KB             |
;;;;   | 行の分割・結合               | 全画面(下が全部ずれる) | 約 7.5KB        |
;;;;   | 窓が動いたとき               | 全画面             | 約 7.5KB             |
;;;;   | 知らないキー                 | **何もしない**     | 0                    |
;;;;
;;;; 効きは test/lisp/editor_app_disk_test.lisp §5 が実機で数字にする。
;;;;
;;;; **行を必ず桁いっぱいまで空白で埋めて書く**(`%ed-fit`)。こうすると
;;;; 1 回の書き込みが**消去も兼ねる** —— 前の行の残りを消すために
;;;; 「空白で塗ってから書く」と呼び出しが 2 回になり、芯 1 に反する。
;;;;
;;;; ## 画面の割り付け —— **最下行は空けておく**
;;;;
;;;;   行 0 .. h-1 … 本文(窓)
;;;;   行 h        … 合図(行番号・桁・行数・変更の印・打てるキー)
;;;;   行 h+1 以降 … 使わない。**皮が h <= rows-2 を保証する**
;;;;
;;;; ### 起動時に **1 回だけ**消す(#230)
;;;;
;;;; 差分再描画は意図して塗り直さないので、**起動時に消さないと前の表示が
;;;; 残って本文が重なる**(利用者が実機で踏んだ)。消す場所は核の中の
;;;; **「断る検査を通ったあと・最初の全画面描画の直前」**であって、核の先頭では
;;;; ない —— 先頭で消すと**開かなかっただけでコンソールが消える。**
;;;; 毎回消してはならない(それは全画面の塗り直しと同じで、芯 3 が消える)。
;;;;
;;;; 調査(§8-4)は「最下行を合図に使い、行送りを出さないほうで回避する」案を
;;;; 挙げているが、**採らなかった。** 理由: 行送りを出さないと、文字列出力
;;;; ストリームへ流したときに合図の行と次の描き直しの 1 行目が繋がってしまい、
;;;; **CI 側の試験(位置指定の効かない流し込み)が行を数えられなくなる。**
;;;; apps/filer.lisp も同じ理由で最下行を空けている。**本文が 1 行減るだけ**の
;;;; 代償で、核が `term-rows` を見ずに済む(核は画面の寸法を知らない)。
;;;;
;;;; ## カーソルの位置は**合図の数字が主**
;;;;
;;;; カーソルの位置を読む口が無い(#216 の 5 つに無い)ので、**カーソルだけでは
;;;; 「正しい場所にいる」ことを機械で言えない。** エディタの主役はカーソルなので
;;;; apps/filer.lisp の「記号を置く」逃げ方は使えない —— 代わりに
;;;; **合図の行へ行番号と桁を数字で出す。** 数字なら `term-read-cell` で
;;;; 格子から読み返せる(logic §9-3 の案をそのまま採った)。
;;;;
;;;; ## 保存 —— **安全な差し替えは組めない。隠さない**
;;;;
;;;; 詳細と残る危険は **documents/editor-save-risk.md** に書いた。要点だけ:
;;;;
;;;;   - **書き込み用に開いた瞬間に中身が空になる**(mount.c:309-340)
;;;;   - **名前を変える手段も消す手段も無い**(fat16/fat32 にも 9P にも無い)
;;;;   - したがって「別名で書いてから差し替える」は**作れない**
;;;;   - → **控え(別名)を先に書く。** 差し替えではないので完全ではない。
;;;;     **控えを書いたあと・本体を書き終える前に落ちると本体は空になる。**
;;;;
;;;; **保存は 1 文字ずつの道(`write-char`)で書く。** `~A` の安い道は
;;;; ディスクに対して安全でないとコード中に注記がある(format.c:158-163)★。
;;;; 画面と違って保存は明示的な操作のときだけなので、これで困らない。
;;;;
;;;; ## 打鍵の取りこぼしは**観測できない**(調べた。#218 の報告事項)
;;;;
;;;; 受け皿は 16 個(`PROCESS_RAW_KEY_BUF_SIZE`、process.h:22)で、満杯のとき
;;;; `process_raw_key_push`(process.c:492-498)は **何も数えずに `return` する。**
;;;; 落とした数を持つ場所が無く、Lisp へ出す口(terminal.c:112-118 の 5 個)にも
;;;; 無い。**つまり溢れたことはアプリからは分からない。**
;;;; 描き直しを軽くして**起こりにくく**はしたが、**無くしてはいない。**
;;;; 知らせる形にするには C 側に数え場所と口を足すことになる(= この仕事の範囲外。
;;;; #218 の設計は「C を触らない。新しい口はゼロ」)。
;;;;
;;;; ## 範囲外(設計コメントのとおり)
;;;;
;;;; 色・属性 / 検索・置換 / 取り消し / ファイルの削除・改名。

;;; ---------------------------------------------------------------------------
;;; 大きさの既定
;;;
;;; **画面から読まない。** 核だけを文字列ストリームで試験するときに実画面へ
;;; 依らせないためである(apps/filer.lisp / apps/pager.lisp と同じ作法)。
;;; 実機では皮(下の `editor`)が `term-rows` / `term-cols` から切り下げる。
;;;
;;; **初期宣言は安全な形で書く**(tools/check_defdynamic_safe.py と同じ作法。#187)。
;;; defdynamic は value-form を評価して**無条件に上書きする**ので、素の初期値を
;;; 書くと「2 回目の load」が利用者の設定を無言で消す。この `or` の形は
;;; **「この変数は nil を意味のある値として持たない」**ことに依存している。
(defdynamic *editor-text-lines* (or (dynamic *editor-text-lines*) 16))
(defdynamic *editor-row-cols* (or (dynamic *editor-row-cols*) 40))

;;; 開ける行数の上限。**超えたら開かずに断る。**
;;;
;;; logic §11-3 は「上限を決めることを勧める。半端に開いて編集させるより安全」と
;;; 書いている。**★ 2000 という値は測って決めたものではない**(使える記憶の総量を
;;; 測っていない。logic §16-1)。**破綻の点が分かったら下げる・上げる。**
;;; いま要るのは「際限なく読み込んで、途中で記憶が尽きて落ちる」道を塞ぐことだけ。
(defdynamic *editor-max-lines* (or (dynamic *editor-max-lines*) 2000))

;;; ---------------------------------------------------------------------------
;;; 数を文字列にする
;;;
;;; **`format` を使わない。** 位置を指定して書くには文字列が要るのに、
;;; 文字列へ書くには毎回ストリームを作る(1 回あたり約 1KB の器)ことになる。
;;; 合図の行は**1 打鍵ごとに**作り直すので、その量ではない。
;;; (apps/filer.lisp の `%filer-number-text` と同じもの。**写してある** ——
;;; ファイラを読み込んでいない起動でもこのアプリは動かなければならない。)
(defun %ed-number-text (n)
  (if (< n 0)
      "?"
    (if (= n 0)
        "0"
      (let ((s "") (v n))
        (while (> v 0)
          (setq s (string-append
                    (create-string 1 (elt "0123456789" (mod v 10))) s))
          (setq v (div v 10)))
        s))))

;;; ---------------------------------------------------------------------------
;;; 桁ちょうどに収める(**足りなければ空白で埋める**)
;;;
;;; 埋めるのが要点である。**1 回の書き込みが消去も兼ねる**ので、
;;; 「空白で塗ってから書く」(呼び出し 2 回)が要らない。
;;; `blank` は呼び出し側が 1 回だけ作った空白の文字列で、**毎回作らない**
;;; (作ると `create-string` が 1 打鍵ごとに増える)。
(defun %ed-fit (s w blank)
  (let ((n (length s)))
    (cond ((> n w) (subseq s 0 w))
          ((= n w) s)
          (t (string-append s (subseq blank 0 (- w n)))))))

;;; ---------------------------------------------------------------------------
;;; 鎖の小道具
;;;
;;; **どれも `while` で書く**(再帰は深い文書でスタックを使う)。

;;; a を逆順にして b の前へ繋ぐ。新しい節だけを作る(b は共有する)
(defun %ed-revappend (a b)
  (let ((cur a) (acc b))
    (while cur
      (setq acc (cons (car cur) acc))
      (setq cur (cdr cur)))
    acc))

;;; lst の先頭 k 個を**逆順**で返す
(defun %ed-take-rev (lst k)
  (let ((acc nil) (cur lst) (i k))
    (while (and cur (> i 0))
      (setq acc (cons (car cur) acc))
      (setq cur (cdr cur))
      (setq i (- i 1)))
    acc))

;;; lst の先頭 k 個を順どおりに返す
(defun %ed-take (lst k) (%ed-revappend (%ed-take-rev lst k) nil))

;;; 鎖の長さ。**持ち回る値がずれていないかを試験が突き合わせるため**だけにある。
;;; 本体の処理では使わない(使うと O(n) が戻ってくる)
(defun %ed-length (lst)
  (let ((cur lst) (n 0))
    (while cur (setq cur (cdr cur)) (setq n (+ n 1)))
    n))

;;; ---------------------------------------------------------------------------
;;; 窓に見える行(上から下の順)
;;;
;;; `above` は逆順なので、先頭 back 個を `tail` の前へ consing していくと
;;; **自然に上から下の順になる。** 取り出すのは窓の高さぶんだけで、
;;; **文書の長さに依らない。**
(defun %ed-window-lines (above line below back fwd)
  (let ((tail (cons line (%ed-take below fwd)))
        (cur above) (i back))
    (while (and cur (> i 0))
      (setq tail (cons (car cur) tail))
      (setq cur (cdr cur))
      (setq i (- i 1)))
    tail))

;;; 文書ぜんぶ(保存のときだけ使う。**O(n) で構わない** —— 明示的な操作である)
(defun %ed-document (above line below) (%ed-revappend above (cons line below)))

;;; ---------------------------------------------------------------------------
;;; 行の編集 —— **どれも新しい文字列を作る。元は書き換えない**
;;;
;;; 書き換えてはならない理由(logic §5-9。**実機で確かめてある**):
;;; **文字列リテラルはその場で書き換わり、書き換わったまま残る。**
;;; 読み込んだ行をリテラル由来の入れ物のまま編集すると、プログラム自身が壊れる。
(defun %ed-insert-char (line col ch)
  (string-append (subseq line 0 col)
                 (create-string 1 ch)
                 (subseq line col (length line))))

;;; col の位置の 1 文字を消す。範囲外なら**そのまま返す**(黙って壊さない)
(defun %ed-delete-char (line col)
  (let ((n (length line)))
    (if (or (< col 0) (>= col n))
        line
      (string-append (subseq line 0 col) (subseq line (+ col 1) n)))))

(defun %ed-left-part (line col) (subseq line 0 col))
(defun %ed-right-part (line col) (subseq line col (length line)))

;;; ---------------------------------------------------------------------------
;;; 窓の頭(apps/filer.lisp の `%filer-window-top` と同じ計算)
;;;
;;; カーソルが窓から出たら、出た向きへ 1 行ぶんだけ動かす。
;;; **純粋な計算なので単体で試験できる**(文書も画面も要らない)。
(defun %ed-window-top (crow top h)
  (cond ((< h 1) crow)
        ((< crow top) crow)
        ((>= crow (+ top h)) (+ (- crow h) 1))
        (t top)))

(defun %ed-clamp (want cap) (if (< cap 1) 1 (if (< want cap) want cap)))
(defun %ed-floor1 (v) (if (< v 1) 1 v))

;;; ---------------------------------------------------------------------------
;;; 合図の行
;;;
;;; `"L12 C3 of 40 * C-s=save C-q=quit"`
;;;
;;; **行番号と桁を数字で出すのがこのアプリの検査の要である**(ファイル頭の
;;; 「カーソルの位置は合図の数字が主」を参照)。カーソルの位置は読めないので、
;;; **この数字だけが「カーソルがどこにいるか」の機械で読める証拠**になる。
;;;
;;; 人に合わせて **1 から数える。** 変更の印は**常に 1 文字**(`*` か `-`)に
;;; してある —— 長さが変わると後ろの桁がずれて、格子の試験が読みにくくなる。
;;;
;;; `msg` があれば打てるキーの代わりにそれを出す。**数字の部分は動かさない。**
(defun %ed-status-text (crow ccol nlines dirty msg)
  (string-append "L" (%ed-number-text (+ crow 1))
                 " C" (%ed-number-text (+ ccol 1))
                 " of " (%ed-number-text nlines)
                 " " (if dirty "*" "-")
                 " " (if (null msg) "C-s=save C-q=quit" msg)))

;;; ---------------------------------------------------------------------------
;;; 控えの名前
;;;
;;; `/app/NOTE.TXT` → `/app/NOTE.BAK` / `/app/README` → `/app/README.BAK`
;;;
;;; **拡張子を足さずに差し替える**のは、FAT16 の 8.3 で `NOTE.TXT.BAK` が
;;; 作れないからである(`/app` は FAT32 だが、このアプリは FAT16 のマウントでも
;;; 使われうる)。
;;;
;;; ★ **同じ名前の控えが既にあれば黙って上書きする。** 消す手段も改名する手段も
;;; 無いので、衝突を避ける方法が無い(documents/editor-save-risk.md に書いた)。
(defun %ed-last-dot (s)
  (let ((i 0) (n (length s)) (dot nil) (start 0))
    (while (< i n)
      (let ((c (elt s i)))
        (cond ((char= c #\/) (progn (setq dot nil) (setq start (+ i 1))))
              ((char= c #\.) (if (> i start) (setq dot i) nil))
              (t nil)))
      (setq i (+ i 1)))
    dot))

(defun %ed-backup-path (path)
  (let ((dot (%ed-last-dot path)))
    (if (null dot)
        (string-append path ".BAK")
      (string-append (subseq path 0 dot) ".BAK"))))

;;; ---------------------------------------------------------------------------
;;; 読み込み
;;;
;;; **`read-line` を使う。** 1 行につき呼び出しが 1 回で済む(`read-char` で
;;; 回すと 1 文字につき 1 回。ファイル頭の芯 1)。
;;;
;;; ### 511 文字で断る理由 —— **黙って切り捨てられるため**
;;;
;;; `cc_read_line`(stream_lisp.c:504-537)の器は `READ_LINE_MAX = 512` 固定で、
;;; **溢れた分は何も言わずに捨てられる。** 返ってくる長さは 511 で頭打ちになる。
;;; つまり「ちょうど 511 文字の行」と「切り捨てられた行」は**見分けがつかない。**
;;;
;;; 見分けられないものを編集して保存すると、**利用者のデータが静かに消える。**
;;; この企画がいちばん嫌う形なので、**511 文字以上の行があったら開かない。**
;;; (長い行を扱いたければ `READ_LINE_MAX` を動かすか、`read-char` で読む
;;; 別の道が要る。どちらもこの仕事の範囲外。)
;;;
;;; ### 行末の約束(★ 確かめていないところ)
;;;
;;; `read-line` は改行を**捨てて**返すので、「末尾に改行がある / 無い」の区別は
;;; **読んだ時点で失われる。** このアプリは保存で**必ず各行の後ろへ改行を 1 つ
;;; 書く**ので、末尾に改行の無いファイルを開いて保存すると **1 バイト増える。**
;;; ★ 復帰文字(CR)はそのまま行の中の 1 文字として残る(CRLF のファイルは
;;; 画面でも保存でもそのまま。**確かめていない**。logic §16-2)。
;;;
;;; 返すのは行の鎖。**空のファイルでも 1 行(空文字列)を返す** ——
;;; 0 行の文書は「カーソルの載る行が無い」状態で、場合分けが増えるだけである。
;;; 切り捨てに当たったら `'ed-too-long` を返す。
(defun %ed-read-lines (path)
  (with-open-input-file (src path)
    (let ((acc nil) (going t) (bad nil))
      (while going
        (let ((s (read-line src nil nil)))
          (cond ((null s) (setq going nil))
                ((>= (length s) 511) (progn (setq bad t) (setq going nil)))
                (t (setq acc (cons s acc))))))
      (if bad 'ed-too-long (if (null acc) (list "") (reverse acc))))))

;;; ---------------------------------------------------------------------------
;;; 書き出し
;;;
;;; **1 文字ずつ `write-char` で書く。** 画面で使っている `~A` の安い道は
;;; **ディスクに対して安全でない**とコード中に注記がある(format.c:158-163 の
;;; 「既知の未修正バグ」。★ 再現は見ていない)。保存は明示的な操作のときだけ
;;; なので、1 文字ずつでも困らない。
;;;
;;; **各行の後ろに改行を 1 つ書く**(最後の行も。上の「行末の約束」を参照)。
(defun %ed-write-lines (path lines)
  (with-open-output-file (dst path)
    (let ((cur lines))
      (while cur
        (let ((s (car cur)) (i 0))
          (let ((n (length s)))
            (while (< i n)
              (write-char (elt s i) dst)
              (setq i (+ i 1)))))
        (write-char #\newline dst)
        (setq cur (cdr cur)))))
  t)

;;; ---------------------------------------------------------------------------
;;; 保存 —— **控えを先に書く**
;;;
;;; 返すのは 3 つの記号のどれか:
;;;   'ed-saved         … 控えも本体も書けた
;;;   'ed-backup-failed … **控えが書けなかった。本体には触れていない**
;;;   'ed-main-failed   … **本体の書き込みに失敗した。本体は空か途中である**
;;;
;;; **順序に意味がある。** 控えを先に書くのは、本体を開いた瞬間に中身が空になる
;;; から(mount.c:309-340)で、控えが無い状態で本体を開いたら**元の内容を
;;; 復元する手段がどこにも無くなる。**
;;;
;;; **控えが書けなかったら本体を開かない。** ここで進むと「控えも無い・本体も
;;; 空」になる。
;;;
;;; **それでも残る危険**: 控えを書いたあと・本体を書き終える前に落ちると
;;; **本体は空か途中**である。差し替える手段(改名)が無いので消せない。
;;; → **documents/editor-save-risk.md** に書いた。隠していない。
;;;
;;; `ignore-errors` で包むのは、**失敗して対話ごと落ちると編集中の内容が
;;; まるごと消えるから**である。失敗は合図の行で**声に出して**知らせ、
;;; 編集は続けられる状態で残す(もう一度保存できる)。
(defun %ed-save (path lines)
  (let ((bak (%ed-backup-path path)))
    (if (null (ignore-errors (%ed-write-lines bak lines)))
        'ed-backup-failed
      (if (null (ignore-errors (%ed-write-lines path lines)))
          'ed-main-failed
        'ed-saved))))

;;; 保存の結果を合図の行の文面にする。**失敗を黙らせない。**
;;; 本体の失敗は「控えに残っている」ことまで言う —— 言わないと、
;;; 利用者は空になった本体を見て諦めてしまう。
(defun %ed-save-message (result bak)
  (cond ((eq result 'ed-saved) (string-append "saved (copy: " bak ")"))
        ((eq result 'ed-backup-failed) "COPY FAILED - nothing written")
        (t (string-append "WRITE FAILED - file may be empty! copy: " bak))))

;;; ---------------------------------------------------------------------------
;;; 描く
;;;
;;; **1 行 1 回。** `term-write-at` は #218 で `(format out "~A" str)` に直して
;;; あるので、1 行が**長さによらず 1 回ぶんの費用**になる(ファイル頭の芯 1)。
;;;
;;; 行送りを 1 つ出すのは、文字列出力ストリームへ流したときに**行として
;;; 読めるようにする**ためである(位置指定は文字列には効かない)。
;;; 実画面では「次の行の頭へ進む」だけで害は無い —— **最下行でだけ画面を
;;; 巻き上げる**ので、皮が最下行を空けておく(ファイル頭の「画面の割り付け」)。

;;; 本文の 1 行。`text` が nil なら空行(= 文書の終わりより下)
(defun %ed-draw-line (row text w blank out)
  (term-write-at row 0 (%ed-fit (if (null text) "" text) w blank) out)
  (format out "~%"))

;;; 合図の行。**行 h に出す**(本文は 0..h-1)
(defun %ed-draw-status (h crow ccol nlines dirty msg w blank out)
  (term-write-at h 0
                 (%ed-fit (%ed-status-text crow ccol nlines dirty msg) w blank)
                 out)
  (format out "~%"))

;;; 窓ぜんぶ + 合図。**窓が動いたときと、行の本数が変わったときだけ呼ぶ**
(defun %ed-draw-all (wlines h crow ccol nlines dirty msg w blank out)
  (let ((i 0) (cur wlines))
    (while (< i h)
      (%ed-draw-line i (if (null cur) nil (car cur)) w blank out)
      (if cur (setq cur (cdr cur)) nil)
      (setq i (+ i 1))))
  (%ed-draw-status h crow ccol nlines dirty msg w blank out))

;;; ---------------------------------------------------------------------------
;;; 核。**ここがアプリの全部で、ファイルとストリームしか触らない**
;;;
;;;   path … 開くファイル(絶対パス、または `*cwd*` からの相対)
;;;   in   … 打鍵を読む入力ストリーム
;;;   out  … 表示先の出力ストリーム
;;;
;;; 打てるキー:
;;;   矢印 4 つ        … カーソルを動かす(行をまたぐ)
;;;   Home / End       … 行頭 / 行末
;;;   PageUp/PageDown  … 1 画面ぶん上下
;;;   表示できる文字   … その場に挿入
;;;   改行             … 行を分ける
;;;   Backspace        … 左を消す。**行頭なら前の行と繋げる**
;;;   Delete           … 右を消す。**行末なら次の行と繋げる**
;;;   C-s              … 保存(**控えを先に書く**)
;;;   C-q              … やめる。**変更があれば 1 回止めて聞き返す**
;;;   それ以外         … **何もしない**(描き直しもしない)
;;;
;;; 返り値は **(理由 編集した回数 保存した回数 行数)** の 4 つ:
;;;   'editor-quit     … C-q でやめた
;;;   'editor-eof      … 打鍵が尽きた(文字列入力ストリームでの打ち切りがこれ)
;;;   'editor-too-long … 511 文字以上の行があるので**開かなかった**
;;;
;;; **大きい値を返さない**(対話の輪は結果を必ず印字する。repl.c:67/:70)。
;;; 文書そのものを返すと画面が流れる。記号 1 つと fixnum 3 つだけ。
;;;
;;; **ファイルが開けなければ signal がそのまま飛ぶ**(`with-open-input-file`)。
;;; 握り潰さないのは、皮の `unwind-protect` が素の入力を戻すことを
;;; **実機の試験が陽性対照として使える**ようにするためでもある(#215 で踏んだ族)。
(defun editor-run (path in out)
  (let* ((h (%ed-floor1 (dynamic *editor-text-lines*)))
         (w (%ed-floor1 (dynamic *editor-row-cols*)))
         (maxl (%ed-floor1 (dynamic *editor-max-lines*)))
         (blank (create-string w #\space))
         (bak (%ed-backup-path path))
         (lines (%ed-read-lines path)))
    (if (eq lines 'ed-too-long)
        (progn
          (format out "~A: has a line of 511 characters or more; not opened~%" path)
          (list 'editor-too-long 0 0 0))
      (let ((nlines (%ed-length lines)))
        (if (> nlines maxl)
            (progn
              (format out "~A: too many lines (~D > ~D); not opened~%"
                      path nlines maxl)
              (list 'editor-too-long 0 0 nlines))
          (let ((above nil) (line (car lines)) (below (cdr lines))
                (crow 0) (ccol 0) (wtop 0)
                (dirty nil) (edits 0) (saves 0) (msg nil)
                (quit-armed nil) (reason 'editor-eof) (running t)
                ;; 'full / 'line / 'status / 'none
                (redraw 'full))
            ;; **画面を消すのはここ —— 検査を通ったあと、最初の全画面描画の
            ;; 直前である**(#230。利用者が実機で踏んだ)。
            ;;
            ;; 消さなければならない理由: 差分再描画(#218)は「変わった行だけ
            ;; 書く」ことで 13.3 倍を得ているので、**意図して塗り直さない。**
            ;; 起動時に消さないと、それまでコンソールに出ていた文字の上に本文が
            ;; 重なって出る(ファイラは `%filer-draw` が毎回 `term-clear` を
            ;; 呼ぶのでこの症状が出ない。apps/filer.lisp:284)。
            ;;
            ;; **上の 2 つの断りより前で消してはならない。**(511 文字以上の行 /
            ;; 行数の上限)。核の先頭で消すと「**開けなかっただけで利用者の
            ;; コンソールが消える**」という別の不具合になり、断りの文面も
            ;; 消した直後の画面に取り残される。
            ;;
            ;; **消すのはここ 1 回だけである。** while の中で消すと毎回
            ;; 全画面を塗り直すのと同じことになり、差分再描画の効きが消える。
            ;; 陽性対照と確保量は test/lisp/editor_app_disk_test.lisp §8。
            (progn (term-clear) (term-set-cursor 0 0))
            (while running
              ;; 窓が動いたら全画面。**動いていなければ触らない**
              (let ((newtop (%ed-window-top crow wtop h)))
                (if (= newtop wtop)
                    nil
                  (progn (setq wtop newtop) (setq redraw 'full))))
              (cond
                ((eq redraw 'full)
                 (%ed-draw-all (%ed-window-lines above line below
                                                 (- crow wtop)
                                                 (- (- h 1) (- crow wtop)))
                               h crow ccol nlines dirty msg w blank out))
                ((eq redraw 'line)
                 (progn
                   (%ed-draw-line (- crow wtop) line w blank out)
                   (%ed-draw-status h crow ccol nlines dirty msg w blank out)))
                ((eq redraw 'status)
                 (%ed-draw-status h crow ccol nlines dirty msg w blank out))
                (t nil))
              (setq redraw 'none)
              ;; **カーソルを編集位置へ置く。** 副の示し方である
              ;; (主は合図の数字。ファイル頭を参照)。桁が窓の外なら端に置く
              (term-set-cursor (- crow wtop) (if (< ccol w) ccol (- w 1)))
              (let ((k (term-read-key in)))
                (if (null k)
                    nil
                  (progn
                    ;; 合図は**次の打鍵で消える。** 出したまま残すと、
                    ;; 「保存した」が後の操作にも貼り付いて嘘になる
                    (if (null msg)
                        nil
                      (progn (setq msg nil) (setq redraw 'status)))
                    ;; **C-q 以外の打鍵は「聞き返し」を取り消す。**
                    ;; 取り消さないと、間に別の操作を挟んでも 1 回目の C-q が
                    ;; 効いたままになり、**聞かずに捨てる**ことになる
                    (if (= (char-code k) 17) nil (setq quit-armed nil))))
                (cond
                  ;; 打鍵が尽きた。**待ち続けない**
                  ((null k) (progn (setq reason 'editor-eof) (setq running nil)))
                  ;; --- やめる(C-q = 17) -------------------------------------
                  ((= (char-code k) 17)
                   (if (and dirty (null quit-armed))
                       ;; **1 回止める。** 保存できていない内容を捨てる前に
                       ;; 必ず聞き返す(logic §14-3 の勧め)
                       (progn (setq quit-armed t)
                              (setq msg "modified! C-q again to quit")
                              (setq redraw 'status))
                     (progn (setq reason 'editor-quit) (setq running nil))))
                  ;; --- 保存(C-s = 19) ---------------------------------------
                  ((= (char-code k) 19)
                   (let ((r (%ed-save path (%ed-document above line below))))
                     (progn
                       (if (eq r 'ed-saved)
                           (progn (setq dirty nil) (setq saves (+ saves 1)))
                         nil)
                       (setq msg (%ed-save-message r bak))
                       ;; 変更の印が消えるので合図だけ描き直せばよい
                       (setq redraw 'status))))
                  ;; --- 左 ---------------------------------------------------
                  ((char= k *key-left*)
                   (cond ((> ccol 0)
                          (progn (setq ccol (- ccol 1)) (setq redraw 'status)))
                         (above
                          (progn (setq below (cons line below))
                                 (setq line (car above))
                                 (setq above (cdr above))
                                 (setq crow (- crow 1))
                                 (setq ccol (length line))
                                 (setq redraw 'status)))
                         (t nil)))
                  ;; --- 右 ---------------------------------------------------
                  ((char= k *key-right*)
                   (cond ((< ccol (length line))
                          (progn (setq ccol (+ ccol 1)) (setq redraw 'status)))
                         (below
                          (progn (setq above (cons line above))
                                 (setq line (car below))
                                 (setq below (cdr below))
                                 (setq crow (+ crow 1))
                                 (setq ccol 0)
                                 (setq redraw 'status)))
                         (t nil)))
                  ;; --- 上 ---------------------------------------------------
                  ((char= k *key-up*)
                   (if (null above)
                       nil
                     (progn (setq below (cons line below))
                            (setq line (car above))
                            (setq above (cdr above))
                            (setq crow (- crow 1))
                            (setq ccol (min ccol (length line)))
                            (setq redraw 'status))))
                  ;; --- 下 ---------------------------------------------------
                  ((char= k *key-down*)
                   (if (null below)
                       nil
                     (progn (setq above (cons line above))
                            (setq line (car below))
                            (setq below (cdr below))
                            (setq crow (+ crow 1))
                            (setq ccol (min ccol (length line)))
                            (setq redraw 'status))))
                  ;; --- 行頭 / 行末 ------------------------------------------
                  ((char= k *key-home*)
                   (progn (setq ccol 0) (setq redraw 'status)))
                  ((char= k *key-end*)
                   (progn (setq ccol (length line)) (setq redraw 'status)))
                  ;; --- 1 画面ぶん上下 ---------------------------------------
                  ;; **繰り返しで 1 行ずつ動かす。** 鎖は添字で引けないので、
                  ;; 飛ぶ操作だけは動いた行数に比例する(logic §11-2 のとおり)
                  ((char= k *key-page-up*)
                   (let ((i 0))
                     (while (and (< i h) above)
                       (setq below (cons line below))
                       (setq line (car above))
                       (setq above (cdr above))
                       (setq crow (- crow 1))
                       (setq i (+ i 1)))
                     (setq ccol (min ccol (length line)))
                     (setq redraw 'status)))
                  ((char= k *key-page-down*)
                   (let ((i 0))
                     (while (and (< i h) below)
                       (setq above (cons line above))
                       (setq line (car below))
                       (setq below (cdr below))
                       (setq crow (+ crow 1))
                       (setq i (+ i 1)))
                     (setq ccol (min ccol (length line)))
                     (setq redraw 'status)))
                  ;; --- 行を分ける(改行) -----------------------------------
                  ;; **下の行が全部ずれるので全画面を描き直す**
                  ((char= k #\newline)
                   (progn (setq above (cons (%ed-left-part line ccol) above))
                          (setq line (%ed-right-part line ccol))
                          (setq crow (+ crow 1))
                          (setq nlines (+ nlines 1))
                          (setq ccol 0)
                          (setq dirty t) (setq quit-armed nil)
                          (setq edits (+ edits 1))
                          (setq redraw 'full)))
                  ;; --- 左を消す(Backspace = 8) -----------------------------
                  ((= (char-code k) 8)
                   (cond ((> ccol 0)
                          (progn (setq line (%ed-delete-char line (- ccol 1)))
                                 (setq ccol (- ccol 1))
                                 (setq dirty t) (setq quit-armed nil)
                                 (setq edits (+ edits 1))
                                 (setq redraw 'line)))
                         (above
                          ;; **前の行と繋げる。** 下が全部ずれるので全画面
                          (progn (setq ccol (length (car above)))
                                 (setq line (string-append (car above) line))
                                 (setq above (cdr above))
                                 (setq crow (- crow 1))
                                 (setq nlines (- nlines 1))
                                 (setq dirty t) (setq quit-armed nil)
                                 (setq edits (+ edits 1))
                                 (setq redraw 'full)))
                         (t nil)))
                  ;; --- 右を消す(Delete = 127) ------------------------------
                  ((= (char-code k) 127)
                   (cond ((< ccol (length line))
                          (progn (setq line (%ed-delete-char line ccol))
                                 (setq dirty t) (setq quit-armed nil)
                                 (setq edits (+ edits 1))
                                 (setq redraw 'line)))
                         (below
                          (progn (setq line (string-append line (car below)))
                                 (setq below (cdr below))
                                 (setq nlines (- nlines 1))
                                 (setq dirty t) (setq quit-armed nil)
                                 (setq edits (+ edits 1))
                                 (setq redraw 'full)))
                         (t nil)))
                  ;; --- 表示できる文字を挿入 ---------------------------------
                  ;; 32(空白)から 126(~)まで。**ここだけが文字を増やす道**
                  ((and (>= (char-code k) 32) (< (char-code k) 127))
                   (progn (setq line (%ed-insert-char line ccol k))
                          (setq ccol (+ ccol 1))
                          (setq dirty t) (setq quit-armed nil)
                          (setq edits (+ edits 1))
                          (setq redraw 'line)))
                  ;; --- 知らないキーは**何もしない** -------------------------
                  ;; 描き直しもしない(apps/filer.lisp は描き直すが、
                  ;; エディタは押しっぱなしで受け皿が溢れるのを避けたい。
                  ;; ファイル頭の「打鍵の取りこぼし」を参照)
                  (t nil))))
            (list reason edits saves nlines)))))))

;;; ---------------------------------------------------------------------------
;;; 皮。**これだけが画面とキーボードを知っている**
;;;
;;; `(standard-input)` / `(standard-output)` を**それぞれ 1 回だけ**呼んで核へ渡す。
;;; どちらも未束縛なら新しい流れを作る(1 回あたり約 1.6KB の確保)ので、
;;; ループの中で呼んではならない。
;;;
;;; やることは 4 つ:
;;;   1. 素の入力に入れる(エコーを止め、行の確定を待たない)。**必ず戻す**
;;;   2. 本文の行数を実画面に収まるところまで切り下げる
;;;   3. 行の桁数を実画面の桁に収める
;;;   4. 終わったら画面を消して頭へ戻す
;;;
;;; ### 行数の上限が `(- (term-rows) 2)` である理由
;;;
;;; 使う行は **本文 h + 合図 1**、そして**最下行は空けておく。**
;;; 空けるのは、合図の行の後ろに出す行送りで画面が巻き上がらないようにするため
;;; である(巻き上がると格子が 1 行ずれて、位置指定の意味が静かに壊れる。
;;; framebuffer.c:291-305)。つまり h + 2 <= rows。
;;;
;;; ### 桁が `(- (term-cols) 1)` である理由
;;;
;;; **最終桁に書くと次の行へ折り返す**(framebuffer.c:336-340。実測)。
;;; 合図の行は最下行の 1 つ上なので折り返しても巻き上がらないが、
;;; **折り返した時点で「1 行 = 1 行」でなくなる**ので、桁を 1 つ残す。
;;;
;;; ### 無条件に上書きしない理由
;;;
;;; 利用者と試験が `dynamic-let` で指定した値を黙って消さないためである。
;;; **切り下げなら、指定を尊重したうえで必ず収まる。**
;;;
;;; ### `unwind-protect` で囲む理由 —— #215 で実際に踏んだバグ
;;;
;;; 素直に「終わりに戻す」と書くと、**途中で脱出したときに素の入力が入ったまま
;;; 残る。** 以後、対話の輪で打鍵が画面に出なくなる。無いファイルを渡すと
;;; `with-open-input-file` が signal するので、ここは実際に通る道である。
;;; 陽性対照は test/lisp/editor_app_disk_test.lisp §9。
;;;
;;; ### 終わりに画面を消す理由
;;;
;;; 消さないと、対話の輪の次の 1 行が本文に重なって出る(カーソルは編集位置に
;;; あるため)。**核は消さない** —— 核が消すと、実機の試験が
;;; `term-read-cell` で最後の画面を読み返せなくなる。
(defun editor (path)
  (let ((in (standard-input))
        (out (standard-output))
        (h (%ed-clamp (dynamic *editor-text-lines*) (- (term-rows) 2)))
        (w (%ed-clamp (dynamic *editor-row-cols*) (- (term-cols) 1))))
    (let ((was (term-raw-input t)))
      (unwind-protect
          (dynamic-let ((*editor-text-lines* h) (*editor-row-cols* w))
            (editor-run path in out))
        (progn (term-raw-input was)
               (term-clear)
               (term-set-cursor 0 0))))))
