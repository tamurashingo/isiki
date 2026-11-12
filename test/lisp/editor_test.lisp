;; test/lisp/editor_test.lisp
;;
;; [#218] **エディタの核(apps/editor.lisp の editor-run)と部品を、
;; キー入力なしで試験する。**
;;
;; 核は「ファイル・打鍵・表示先」を受け取る形にしてある。打鍵は入力ストリームなので、
;; **文字列入力ストリームを渡せばゲストへキーを送る仕組みが無くても試験できる。**
;; #218 の完了条件「文字列入力ストリームを渡した試験が CI で走る」がこれである。
;;
;; **ここで見るのは文字の内容である。位置指定は見えない**(文字列出力ストリームに
;; 書くと位置は消える)。格子の検査は test/lisp/editor_app_disk_test.lisp にある。
;; **2 段構えのうちの CI 側がこのファイルである。**
;;
;; ## 器が 1024 バイトであることに合わせてある
;;
;; 文字列出力ストリームの器は 1024 バイト(stream.h の STREAM_STRING_OUTPUT_CAP)で、
;; **超えた分は黙って切り捨てられる。** だから下の対照は**窓を 3 行・桁を 20** に
;; 絞ってある(1 回の全画面で 4 行 ×21 = 84 バイト)。
;; 合図の文面そのものは §10 が `%ed-status-text` を直に見るので、ここで桁を
;; 詰めても検査は失われない。
;;
;; ## 出た行数が「どこを描き直したか」を言う(差分再描画の検査)
;;
;;   全画面   … 本文 h 行 + 合図 1 行 = **h+1 行**
;;   1 行だけ … その行 + 合図     = **2 行**
;;   合図だけ … **1 行**
;;   何もしない … **0 行**
;;
;; 行数を数えれば、**どの経路を通ったかが外から言える。** §25 がこれを使う。
;;
;; ## 保存の試験が母艦共有(9P)を使う理由 ★
;;
;; **アプリ専用の円盤(/app)へ書いてはならない。** あの円盤は make が 1 度だけ
;; 焼いて**段をまたいで共有する**ので、1 件でも足すと
;; test/lisp/filer_app_disk_test.lisp の「/app は 7 件」が別の段で落ちる。
;; 母艦共有の `/9p/tmp/` なら使い捨てで、ほかの段が中身を見ていない。
;;
;; **母艦共有は `/9p` に在る**(src/c/mount.c:15 の `MOUNT_9P_PREFIX`)。
;; `load` は先頭に区切りの無い名前を母艦共有として扱うが、
;; **ファイルのストリームはマウント表で引く**ので `/9p/` を書かないと解決できない
;; (test/lisp/aot_leaf_gc_test.lisp などが同じ書き方をしている)。
;;
;; ★ **したがって「FAT 上の既存ファイルを上書きする」経路はここでは通っていない。**
;; 同じ `with-open-output-file` の口を使っており、FAT 側の上書き(#199)は
;; test/lisp/fat16_test.lisp:651-705 と fat32 の対応箇所が**別に**押さえている。
;; エディタ固有の部分(控えを先に書く・縮む上書き・2 回続けて保存)は
;; ここで 9P に対して見ている。
;;
;; 置いた対照(#218 の完了条件に対応する):
;;   §1  既定値
;;   §2  数を文字列にする
;;   §3  桁ちょうどに収める(**埋めるほうが要点**)
;;   §4  丸め
;;   §5  鎖の小道具
;;   §6  窓に見える行
;;   §7  文書ぜんぶ
;;   §8  行の編集。**元を書き換えない陰性対照つき**
;;   §9  窓の頭
;;   §10 合図の文面(**行番号と桁が出ること**)
;;   §11 控えの名前
;;   §12 読み込み(空・末尾改行なし・**511 文字で断る**)
;;   §13 書き出して読み返す(往復)
;;   §14 【完了条件】開いて表示する
;;   §15 【完了条件】カーソルを動かせる
;;   §16 【完了条件】文字を挿入できる
;;   §17 【完了条件】文字を削除できる
;;   §18 【完了条件】行を分割・結合できる
;;   §19 【完了条件】保存できる。**控えが先に書かれている**
;;   §20 縮む上書き / 2 回続けて保存
;;   §21 保存の失敗を黙らせない
;;   §22 やめるときに聞き返す
;;   §23 知らないキーは何もしない
;;   §24 開けないファイルは signal(皮の unwind-protect の前提)
;;   §25 差分再描画が効いている(**出た行数で言う**)

;;; ---------------------------------------------------------------------------
;;; 0. 道具立て
;;;
;;;    **ホスト共有から読む。** 段では /app から読む側が先に走るので、
;;;    ここは「同じ中身・別の経路」で読み直していることになる。
;;;    このファイルを単独で読んでも成立するよう、自分で load する。
;;; ---------------------------------------------------------------------------

(defglobal *et-load* (load "apps/editor.lisp"))
(assert-equal t *et-load*)

;; 打鍵 1 つぶんの文字列。**文字列リテラルに制御文字を直に書く手段が無い**
;; (エスケープはバックスラッシュと二重引用符だけ)
(defglobal *et-nl* (create-string 1 #\newline))
(defglobal *et-save* (create-string 1 (code-char 19)))   ; C-s
(defglobal *et-quit* (create-string 1 (code-char 17)))   ; C-q
(defglobal *et-bs* (create-string 1 (code-char 8)))      ; Backspace
(defglobal *et-del* (create-string 1 (code-char 127)))   ; Delete
(defglobal *et-unknown* (create-string 1 (code-char 1))) ; C-a(割り当てていない)
(defglobal *et-left* (create-string 1 *key-left*))
(defglobal *et-right* (create-string 1 *key-right*))
(defglobal *et-up* (create-string 1 *key-up*))
(defglobal *et-down* (create-string 1 *key-down*))
(defglobal *et-home* (create-string 1 *key-home*))
(defglobal *et-end* (create-string 1 *key-end*))

;; 1 回ぶん動かして (結果 出力文字列) を返す。
;; **窓の大きさは引数で受けて dynamic-let で束縛する。** 動的変数に直に setq すると
;; 以後のすべての対照に漏れる(動的変数はプロセス間でも共有される)
(defun et-run (h w path keys)
  (dynamic-let ((*editor-text-lines* h) (*editor-row-cols* w))
    (let ((out (create-string-output-stream)))
      (let ((r (editor-run path (create-string-input-stream keys) out)))
        (list r (get-output-stream-string out))))))

;; 出た行数(行送りの数)。**どこを描き直したかを言う計器である**
(defun et-lines (s)
  (let ((n 0) (i 0) (len (length s)))
    (while (< i len)
      (if (char= (elt s i) #\newline) (setq n (+ n 1)) nil)
      (setq i (+ i 1)))
    n))

(defun et-has (sub s) (if (null (string-index sub s 0)) nil t))

;; 題材を置く / 読み返す。**エディタの関数を使わない** ——
;; 使うと「書けていないのに読めたことになる」(同じ間違いが打ち消し合う)
(defun et-put (path lines nl-at-end)
  (with-open-output-file (dst path)
    (let ((cur lines))
      (while cur
        (let ((s (car cur)) (i 0) (n (length (car cur))))
          (while (< i n)
            (write-char (elt s i) dst)
            (setq i (+ i 1))))
        (if (or (cdr cur) nl-at-end) (write-char #\newline dst) nil)
        (setq cur (cdr cur)))))
  t)

(defun et-get (path)
  (with-open-input-file (src path)
    (let ((acc nil) (going t))
      (while going
        (let ((s (read-line src nil nil)))
          (if (null s) (setq going nil) (setq acc (cons s acc)))))
      (reverse acc))))

;;; ---------------------------------------------------------------------------
;;; 1. 既定値
;;; ---------------------------------------------------------------------------

(assert-equal 16 (dynamic *editor-text-lines*))
(assert-equal 40 (dynamic *editor-row-cols*))
(assert-equal 2000 (dynamic *editor-max-lines*))

;;; ---------------------------------------------------------------------------
;;; 2. 数を文字列にする(桁上がり・0・負)
;;; ---------------------------------------------------------------------------

(assert-equal "0" (%ed-number-text 0))
(assert-equal "7" (%ed-number-text 7))
(assert-equal "10" (%ed-number-text 10))
(assert-equal "2048" (%ed-number-text 2048))
;; 負は呼び出し元に無いが**素通しせず印を返す**(素通しすると while が回り続ける)
(assert-equal "?" (%ed-number-text -1))

;;; ---------------------------------------------------------------------------
;;; 3. 桁ちょうどに収める
;;;
;;;    **足りなければ空白で埋めるのが要点である。** 埋めるから
;;;    「1 回の書き込みが消去も兼ねる」= 差分再描画が 1 呼び出しで済む。
;;;    陰性対照: 埋めない実装(= 切り詰めるだけ)は 2 件目で落ちる。
;;; ---------------------------------------------------------------------------

(defglobal *et-blank8* (create-string 8 #\space))

(assert-equal "abc" (%ed-fit "abcdef" 3 *et-blank8*))
(assert-equal "ab      " (%ed-fit "ab" 8 *et-blank8*))
(assert-equal "abcdefgh" (%ed-fit "abcdefgh" 8 *et-blank8*))
(assert-equal "        " (%ed-fit "" 8 *et-blank8*))
(assert-equal 8 (length (%ed-fit "x" 8 *et-blank8*)))

;;; ---------------------------------------------------------------------------
;;; 4. 丸め(皮が実画面に合わせるときに使う)
;;; ---------------------------------------------------------------------------

(assert-equal 5 (%ed-clamp 5 10))
(assert-equal 10 (%ed-clamp 20 10))
;; 上限が 1 未満でも 1 を返す(0 行の窓は描くものが無い)
(assert-equal 1 (%ed-clamp 20 0))
(assert-equal 1 (%ed-floor1 0))
(assert-equal 1 (%ed-floor1 -3))
(assert-equal 7 (%ed-floor1 7))

;;; ---------------------------------------------------------------------------
;;; 5. 鎖の小道具
;;; ---------------------------------------------------------------------------

(assert-equal (list "b" "a" "x") (%ed-revappend (list "a" "b") (list "x")))
(assert-equal nil (%ed-revappend nil nil))
(assert-equal (list "b" "a") (%ed-take-rev (list "a" "b" "c") 2))
(assert-equal (list "a" "b") (%ed-take (list "a" "b" "c") 2))
;; 足りなければ在るだけ(**途中で止まる**)
(assert-equal (list "a") (%ed-take (list "a") 5))
(assert-equal nil (%ed-take (list "a") 0))
(assert-equal 3 (%ed-length (list "a" "b" "c")))
(assert-equal 0 (%ed-length nil))

;;; ---------------------------------------------------------------------------
;;; 6. 窓に見える行
;;;
;;;    above は**逆順**(直前の行が car)。カーソルの行は line。
;;;    **窓の高さぶんしか取り出さない**ことが、文書の長さに依らない根拠である。
;;; ---------------------------------------------------------------------------

(defglobal *et-above* (list "b" "a"))    ; 上から a, b
(defglobal *et-below* (list "d" "e"))

(assert-equal (list "a" "b" "c" "d" "e")
              (%ed-window-lines *et-above* "c" *et-below* 2 2))
(assert-equal (list "b" "c" "d")
              (%ed-window-lines *et-above* "c" *et-below* 1 1))
(assert-equal (list "c") (%ed-window-lines *et-above* "c" *et-below* 0 0))
;; 在るより多く求めても在るだけ
(assert-equal (list "a" "b" "c" "d" "e")
              (%ed-window-lines *et-above* "c" *et-below* 9 9))

;;; ---------------------------------------------------------------------------
;;; 7. 文書ぜんぶ(保存のときだけ使う)
;;; ---------------------------------------------------------------------------

(assert-equal (list "a" "b" "c" "d" "e")
              (%ed-document *et-above* "c" *et-below*))
(assert-equal (list "only") (%ed-document nil "only" nil))

;;; ---------------------------------------------------------------------------
;;; 8. 行の編集 —— **元を書き換えない**
;;;
;;;    **陰性対照がここに入っている。** 器を使い回して 1 文字ずつずらす実装
;;;    (調査が 6.8 倍高いと測ったほう)は、**元の文字列が変わる**ので
;;;    `*et-src*` の対照で落ちる。
;;;    文字列リテラルはその場で書き換わって**残る**(logic §5-9。実測)ので、
;;;    これは「速い / 遅い」ではなく**正しさ**の問題である。
;;; ---------------------------------------------------------------------------

(assert-equal "aXbc" (%ed-insert-char "abc" 1 #\X))
(assert-equal "Xabc" (%ed-insert-char "abc" 0 #\X))
(assert-equal "abcX" (%ed-insert-char "abc" 3 #\X))
(assert-equal "X" (%ed-insert-char "" 0 #\X))

(assert-equal "ac" (%ed-delete-char "abc" 1))
(assert-equal "bc" (%ed-delete-char "abc" 0))
(assert-equal "ab" (%ed-delete-char "abc" 2))
;; 範囲外はそのまま返す(黙って壊さない)
(assert-equal "abc" (%ed-delete-char "abc" 3))
(assert-equal "abc" (%ed-delete-char "abc" -1))
(assert-equal "" (%ed-delete-char "" 0))

(assert-equal "ab" (%ed-left-part "abcde" 2))
(assert-equal "cde" (%ed-right-part "abcde" 2))
(assert-equal "" (%ed-left-part "abcde" 0))
(assert-equal "" (%ed-right-part "abcde" 5))

;; **元が変わっていないこと**(作り直している証拠)
(defglobal *et-src* (create-string 3 #\a))
(defglobal *et-ins* (%ed-insert-char *et-src* 1 #\X))
(assert-equal "aXaa" *et-ins*)
(assert-equal "aaa" *et-src*)

;;; ---------------------------------------------------------------------------
;;; 9. 窓の頭(巻き)
;;;
;;;    カーソルが窓から出たら、出た向きへ 1 行ぶんだけ動かす。
;;;    陰性対照: いつでも窓の頭をカーソルに合わせる実装は 1 件目で落ちる
;;;    (カーソルが窓の中にいるのに窓が動いてしまう)。
;;; ---------------------------------------------------------------------------

(assert-equal 0 (%ed-window-top 2 0 4))
(assert-equal 2 (%ed-window-top 2 5 4))
(assert-equal 3 (%ed-window-top 6 0 4))
(assert-equal 0 (%ed-window-top 3 0 4))
(assert-equal 5 (%ed-window-top 5 0 0))

;;; ---------------------------------------------------------------------------
;;; 10. 合図の文面
;;;
;;;     **行番号と桁が数字で出ることが、このアプリの検査の要である。**
;;;     カーソルの位置を読む口が無いので、**この数字だけが機械で読める証拠**
;;;     になる(apps/editor.lisp 頭の「カーソルの位置は合図の数字が主」)。
;;;     人に合わせて **1 から数える。**
;;; ---------------------------------------------------------------------------

(assert-equal "L1 C1 of 3 - C-s=save C-q=quit" (%ed-status-text 0 0 3 nil nil))
(assert-equal "L12 C3 of 40 * C-s=save C-q=quit" (%ed-status-text 11 2 40 t nil))
(assert-equal "L2 C1 of 9 * saved" (%ed-status-text 1 0 9 t "saved"))
;; 変更の印は**常に 1 文字**(長さが変わると後ろの桁がずれて格子が読みにくい)
(assert-equal t (et-has " - " (%ed-status-text 0 0 1 nil nil)))
(assert-equal t (et-has " * " (%ed-status-text 0 0 1 t nil)))

;;; ---------------------------------------------------------------------------
;;; 11. 控えの名前
;;;
;;;     **拡張子を足さずに差し替える**(FAT16 の 8.3 で NOTE.TXT.BAK が作れない)。
;;; ---------------------------------------------------------------------------

(assert-equal "/app/NOTE.BAK" (%ed-backup-path "/app/NOTE.TXT"))
(assert-equal "/app/README.BAK" (%ed-backup-path "/app/README"))
;; 先頭が点だけの名前は拡張子ではない(名前ごと消してしまわない)
(assert-equal "/app/.hidden.BAK" (%ed-backup-path "/app/.hidden"))
;; 途中のディレクトリに点があっても、見るのは**最後の名前の中だけ**
(assert-equal "tmp/a.b/c.BAK" (%ed-backup-path "tmp/a.b/c.txt"))
(assert-equal 9 (%ed-last-dot "tmp/a.b/c.txt"))

;;; ---------------------------------------------------------------------------
;;; 12. 読み込み
;;;
;;;     **511 文字で断るのがここの主役である。** `cc_read_line` の器は 512 固定で、
;;;     溢れた分は**何も言わずに捨てられる**(stream_lisp.c:504-537)。
;;;     「ちょうど 511 文字」と「切り捨てられた」は見分けがつかないので、
;;;     **見分けられないものを編集させない。**
;;; ---------------------------------------------------------------------------

(defglobal *et-f3* "/9p/tmp/editor_ci_3lines.txt")
(assert-equal t (et-put *et-f3* (list "alpha" "beta" "gamma") t))
(assert-equal (list "alpha" "beta" "gamma") (%ed-read-lines *et-f3*))

;; 空のファイルでも **1 行(空文字列)** を返す(カーソルの載る行が要る)
(defglobal *et-f0* "/9p/tmp/editor_ci_empty.txt")
(assert-equal t (et-put *et-f0* nil nil))
(assert-equal (list "") (%ed-read-lines *et-f0*))

;; ★ 末尾に改行が無くても同じ結果になる = **区別が読んだ時点で失われる**
(defglobal *et-fnn* "/9p/tmp/editor_ci_nonl.txt")
(assert-equal t (et-put *et-fnn* (list "alpha" "beta") nil))
(assert-equal (list "alpha" "beta") (%ed-read-lines *et-fnn*))

;; **511 文字以上の行があったら開かない**
(defglobal *et-flong* "/9p/tmp/editor_ci_long.txt")
(assert-equal t (et-put *et-flong* (list (create-string 600 #\a)) t))
(assert-equal 'ed-too-long (%ed-read-lines *et-flong*))
;; 510 文字は通る(**境界のすぐ下。陰性対照**)
(defglobal *et-f510* "/9p/tmp/editor_ci_510.txt")
(assert-equal t (et-put *et-f510* (list (create-string 510 #\b)) t))
(assert-equal 510 (length (car (%ed-read-lines *et-f510*))))

;; 核も**開かずに断る**(画面を 1 行も描かない)
(defglobal *et-c-long* (et-run 3 20 *et-flong* ""))
(assert-equal (list 'editor-too-long 0 0 0) (car *et-c-long*))
(assert-equal t (et-has "not opened" (car (cdr *et-c-long*))))

;; 行数の上限でも断る(**半端に開かせない**)
(defglobal *et-c-max*
  (dynamic-let ((*editor-max-lines* 2)) (et-run 3 20 *et-f3* "")))
(assert-equal (list 'editor-too-long 0 0 3) (car *et-c-max*))
(assert-equal t (et-has "too many lines" (car (cdr *et-c-max*))))

;;; ---------------------------------------------------------------------------
;;; 13. 書き出して読み返す(往復)
;;;
;;;     **各行の後ろに改行を 1 つ書く**(最後の行も)。
;;; ---------------------------------------------------------------------------

(defglobal *et-frt* "/9p/tmp/editor_ci_roundtrip.txt")
(assert-equal t (%ed-write-lines *et-frt* (list "one" "two")))
(assert-equal (list "one" "two") (et-get *et-frt*))
;; 空の行も残る
(assert-equal t (%ed-write-lines *et-frt* (list "one" "" "two")))
(assert-equal (list "one" "" "two") (et-get *et-frt*))

;;; ---------------------------------------------------------------------------
;;; 14. 【完了条件】ファイルを開いて表示する
;;;
;;;     打鍵を 1 つも渡さない。1 画面描いて、尽きて終わる。
;;;     **陰性対照:**
;;;       - 文書より下を空行で埋めない実装 … 行数(h+1 = 4)で落ちる
;;;       - 合図を出さない実装 … "L1 C1 of 3" で落ちる
;;; ---------------------------------------------------------------------------

(defglobal *et-c1* (et-run 3 20 *et-f3* ""))

(assert-equal (list 'editor-eof 0 0 3) (car *et-c1*))
(assert-equal 4 (et-lines (car (cdr *et-c1*))))
(assert-equal 0 (string-index "alpha" (car (cdr *et-c1*))))
(assert-equal t (et-has "beta" (car (cdr *et-c1*))))
(assert-equal t (et-has "gamma" (car (cdr *et-c1*))))
(assert-equal t (et-has "L1 C1 of 3 -" (car (cdr *et-c1*))))
;; **桁ちょうどに埋まっている**(1 行 20 文字 + 行送り)
(assert-equal 0 (string-index "alpha               " (car (cdr *et-c1*))))

;;; ---------------------------------------------------------------------------
;;; 15. 【完了条件】カーソルを動かせる(矢印)
;;;
;;;     **見るのは合図の数字である。** カーソルそのものの位置は読めない。
;;; ---------------------------------------------------------------------------

;; 右へ 1 つ
(defglobal *et-c2* (et-run 3 20 *et-f3* *et-right*))
(assert-equal t (et-has "L1 C2 of 3 -" (car (cdr *et-c2*))))

;; 下へ 1 つ
(defglobal *et-c3* (et-run 3 20 *et-f3* *et-down*))
(assert-equal t (et-has "L2 C1 of 3 -" (car (cdr *et-c3*))))

;; 行末から右 → **次の行の頭へ回る**(行をまたぐ)
(defglobal *et-c4* (et-run 3 20 *et-f3* (string-append *et-end* *et-right*)))
(assert-equal t (et-has "L1 C6 of 3 -" (car (cdr *et-c4*))))
(assert-equal t (et-has "L2 C1 of 3 -" (car (cdr *et-c4*))))

;; 行頭から左 → **前の行の末尾へ回る**("beta" は 4 文字なので C5)
(defglobal *et-c5*
  (et-run 3 20 *et-f3* (string-append *et-down* (string-append *et-down* *et-left*))))
(assert-equal t (et-has "L3 C1 of 3 -" (car (cdr *et-c5*))))
(assert-equal t (et-has "L2 C5 of 3 -" (car (cdr *et-c5*))))

;; 端では動かない(先頭で上・末尾で下)。**断っても数えない**
(defglobal *et-c6* (et-run 3 20 *et-f3* (string-append *et-up* *et-left*)))
(assert-equal (list 'editor-eof 0 0 3) (car *et-c6*))
;; **何も描き直していない**(全画面 4 行だけ)
(assert-equal 4 (et-lines (car (cdr *et-c6*))))

;; 下の行が短いと桁が詰められる("gamma"(5) → "beta"(4) で C5 が C5 のまま、
;; さらに下は無いので動かない)。ここでは行末から下へ降りる形で見る
(defglobal *et-c7*
  (et-run 3 20 *et-f3* (string-append *et-down* (string-append *et-end* *et-down*))))
(assert-equal t (et-has "L2 C5 of 3 -" (car (cdr *et-c7*))))
(assert-equal t (et-has "L3 C5 of 3 -" (car (cdr *et-c7*))))

;;; ---------------------------------------------------------------------------
;;; 16. 【完了条件】文字を挿入できる
;;; ---------------------------------------------------------------------------

(defglobal *et-c8* (et-run 3 20 *et-f3* "X"))
(assert-equal (list 'editor-eof 1 0 3) (car *et-c8*))
(assert-equal t (et-has "Xalpha" (car (cdr *et-c8*))))
;; **変更の印が立つ**
(assert-equal t (et-has "L1 C2 of 3 *" (car (cdr *et-c8*))))

;; 行の途中へ入る
(defglobal *et-c9* (et-run 3 20 *et-f3* (string-append *et-right* "X")))
(assert-equal t (et-has "aXlpha" (car (cdr *et-c9*))))

;;; ---------------------------------------------------------------------------
;;; 17. 【完了条件】文字を削除できる
;;; ---------------------------------------------------------------------------

;; Delete は**右**を消す
(defglobal *et-c10* (et-run 3 20 *et-f3* *et-del*))
(assert-equal (list 'editor-eof 1 0 3) (car *et-c10*))
(assert-equal t (et-has "lpha" (car (cdr *et-c10*))))

;; Backspace は**左**を消す(行頭では何もしない = 上に行が無い)
(defglobal *et-c11* (et-run 3 20 *et-f3* (string-append *et-right* *et-bs*)))
(assert-equal t (et-has "lpha" (car (cdr *et-c11*))))

(defglobal *et-c12* (et-run 3 20 *et-f3* *et-bs*))
(assert-equal (list 'editor-eof 0 0 3) (car *et-c12*))
(assert-equal 4 (et-lines (car (cdr *et-c12*))))

;;; ---------------------------------------------------------------------------
;;; 18. 【完了条件】行を分割・結合できる
;;;
;;;     **行の本数が変わるので全画面を描き直す**(下の行が全部ずれるため)。
;;;     §25 が行数でそれを見る。
;;; ---------------------------------------------------------------------------

;; 行の途中で改行 → 4 行になる
(defglobal *et-c13* (et-run 3 20 *et-f3* (string-append *et-right* *et-nl*)))
(assert-equal (list 'editor-eof 1 0 4) (car *et-c13*))
(assert-equal t (et-has "L2 C1 of 4 *" (car (cdr *et-c13*))))

;; 行頭で Backspace → 前の行と繋がって 2 行になる
(defglobal *et-c14* (et-run 3 20 *et-f3* (string-append *et-down* *et-bs*)))
(assert-equal (list 'editor-eof 1 0 2) (car *et-c14*))
(assert-equal t (et-has "alphabeta" (car (cdr *et-c14*))))
(assert-equal t (et-has "L1 C6 of 2 *" (car (cdr *et-c14*))))

;; 行末で Delete → 次の行と繋がる(同じ結果に別の道で着く)
(defglobal *et-c15* (et-run 3 20 *et-f3* (string-append *et-end* *et-del*)))
(assert-equal (list 'editor-eof 1 0 2) (car *et-c15*))
(assert-equal t (et-has "alphabeta" (car (cdr *et-c15*))))

;;; ---------------------------------------------------------------------------
;;; 19. 【完了条件】保存できる。**控えが先に書かれている**
;;;
;;;     控えに入るのは**保存しようとした内容**であって、元の内容ではない。
;;;     理由は documents/editor-save-risk.md に書いた —— 本体を開いた時点で
;;;     元の内容は失われるので、**守る価値があるのは「いま書こうとしている分」**
;;;     のほうである。
;;; ---------------------------------------------------------------------------

(defglobal *et-fsv* "/9p/tmp/editor_ci_save.txt")
(defglobal *et-fsv-bak* "/9p/tmp/editor_ci_save.BAK")
(assert-equal t (et-put *et-fsv* (list "alpha" "beta" "gamma") t))

(defglobal *et-c16*
  (et-run 3 20 *et-fsv* (string-append "X" (string-append *et-save* *et-quit*))))

(assert-equal (list 'editor-quit 1 1 3) (car *et-c16*))
;; **保存したら印が消える**
(assert-equal t (et-has "L1 C2 of 3 - saved" (car (cdr *et-c16*))))
;; 本体が書き換わっている(**既存ファイルへの上書き**)
(assert-equal (list "Xalpha" "beta" "gamma") (et-get *et-fsv*))
;; **控えが在る。中身は同じ**
(assert-equal (list "Xalpha" "beta" "gamma") (et-get *et-fsv-bak*))

;;; ---------------------------------------------------------------------------
;;; 20. 縮む上書き / 2 回続けて保存
;;;
;;;     **長さが減る方向**が #199 で直ったばかりの場所である。
;;;     **2 回続けて保存**するのは「2 回目で落ちる」形の不具合が過去に出ている
;;;     ため(logic §15)。
;;; ---------------------------------------------------------------------------

(assert-equal t (et-put *et-fsv* (list "alpha" "beta" "gamma") t))

;; 行頭で Backspace を 2 回 → 3 行が 1 行になる。そのあと保存を 2 回
(defglobal *et-c17*
  (et-run 3 20 *et-fsv*
          (string-append *et-down*
            (string-append *et-bs*
              (string-append *et-down*
                (string-append *et-home*
                  (string-append *et-bs*
                    (string-append *et-save*
                      (string-append *et-save* *et-quit*)))))))))

(assert-equal (list 'editor-quit 2 2 1) (car *et-c17*))
(assert-equal (list "alphabetagamma") (et-get *et-fsv*))
(assert-equal 1 (%ed-length (et-get *et-fsv*)))
(assert-equal (list "alphabetagamma") (et-get *et-fsv-bak*))

;;; ---------------------------------------------------------------------------
;;; 21. 保存の失敗を黙らせない
;;;
;;;     **控えが書けなければ本体を開かない。** 開いた時点で中身は空になるので、
;;;     ここで進むと「控えも無い・本体も空」になる。
;;; ---------------------------------------------------------------------------

(assert-equal 'ed-backup-failed (%ed-save "/no-such-mount/f.txt" (list "a")))
(assert-equal 'ed-saved (%ed-save *et-frt* (list "ok")))
(assert-equal (list "ok") (et-get *et-frt*))

;; 文面が**声に出している**こと(黙って nil を返さない)
(assert-equal "COPY FAILED - nothing written"
              (%ed-save-message 'ed-backup-failed "x.BAK"))
(assert-equal t (et-has "WRITE FAILED" (%ed-save-message 'ed-main-failed "x.BAK")))
(assert-equal t (et-has "saved" (%ed-save-message 'ed-saved "x.BAK")))

;;; ---------------------------------------------------------------------------
;;; 22. やめるときに聞き返す
;;;
;;;     **変更があれば 1 回止める。** 保存できなかったときに元が戻らない
;;;     (documents/editor-save-risk.md)ので、捨てる前に一度止めるほうが
;;;     被害が小さい(logic §14-3)。
;;; ---------------------------------------------------------------------------

;; 変更が無ければ 1 回で終わる
(defglobal *et-c18* (et-run 3 20 *et-f3* *et-quit*))
(assert-equal (list 'editor-quit 0 0 3) (car *et-c18*))

;; 変更があると 1 回目は止まる(**打鍵が尽きて終わる = やめていない**)
;;
;; **ここだけ桁を 40 にしてある。** 聞き返しの文面は
;; "L1 C2 of 3 * modified! C-q again to quit" でちょうど 40 桁あり、
;; 20 桁だと合図が切り詰められて文面が画面に出ない(**切り詰め自体は正しい
;; 振る舞い**なので、試験の側を実際の幅に合わせる)
(defglobal *et-c19* (et-run 3 40 *et-f3* (string-append "X" *et-quit*)))
(assert-equal (list 'editor-eof 1 0 3) (car *et-c19*))
(assert-equal t (et-has "C-q again to quit" (car (cdr *et-c19*))))

;; 2 回続けて押せばやめる
(defglobal *et-c20*
  (et-run 3 20 *et-f3* (string-append "X" (string-append *et-quit* *et-quit*))))
(assert-equal (list 'editor-quit 1 0 3) (car *et-c20*))

;; **間に別の操作を挟むと聞き返しは取り消される**(聞かずに捨てない)
(defglobal *et-c21*
  (et-run 3 20 *et-f3*
          (string-append "X" (string-append *et-quit*
                               (string-append *et-right* *et-quit*)))))
(assert-equal (list 'editor-eof 1 0 3) (car *et-c21*))

;;; ---------------------------------------------------------------------------
;;; 23. 知らないキーは何もしない
;;;
;;;     **描き直しもしない。** 押しっぱなしで受け皿(16 個)が溢れるのを
;;;     少しでも避けるためである。apps/filer.lisp は描き直すが、ここは変えてある。
;;;     陰性対照: 描き直す実装は行数が 4 を超えて落ちる。
;;; ---------------------------------------------------------------------------

(defglobal *et-c22* (et-run 3 20 *et-f3* (string-append *et-unknown* *et-unknown*)))
(assert-equal (list 'editor-eof 0 0 3) (car *et-c22*))
(assert-equal 4 (et-lines (car (cdr *et-c22*))))

;;; ---------------------------------------------------------------------------
;;; 24. 開けないファイルは signal
;;;
;;;     握り潰さない。皮の `unwind-protect` が素の入力を戻すことを、
;;;     実機の試験が**この道で**陽性対照にする(#215 で踏んだ族)。
;;; ---------------------------------------------------------------------------

(assert-error (et-run 3 20 "/no-such-mount/x.txt" ""))

;;; ---------------------------------------------------------------------------
;;; 25. 差分再描画が効いている(**出た行数で言う**)
;;;
;;;     窓は 3 行なので、全画面は 4 行(本文 3 + 合図 1)。
;;;
;;;       打鍵なし            … 4 行(全画面 1 回)
;;;       カーソルを動かす    … 4 + **1**(合図だけ)
;;;       文字を挿入          … 4 + **2**(その行 + 合図)
;;;       行を分ける          … 4 + **4**(本数が変わるので全画面)
;;;       知らないキー        … 4 + **0**
;;;
;;;     **これが「変わった行だけ描き直す」の機械的な証拠である。**
;;;     全部を描き直す実装は、どの行も 4 ずつ増えるので 3 件が落ちる。
;;;     確保量での裏取りは test/lisp/editor_app_disk_test.lisp §5(実機)。
;;; ---------------------------------------------------------------------------

(defglobal *et-r-none* (et-lines (car (cdr (et-run 3 20 *et-f3* "")))))
(defglobal *et-r-move* (et-lines (car (cdr (et-run 3 20 *et-f3* *et-right*)))))
(defglobal *et-r-ins* (et-lines (car (cdr (et-run 3 20 *et-f3* "X")))))
(defglobal *et-r-split* (et-lines (car (cdr (et-run 3 20 *et-f3* *et-nl*)))))
(defglobal *et-r-unk* (et-lines (car (cdr (et-run 3 20 *et-f3* *et-unknown*)))))

(assert-equal 4 *et-r-none*)
(assert-equal 5 *et-r-move*)
(assert-equal 6 *et-r-ins*)
(assert-equal 8 *et-r-split*)
(assert-equal 4 *et-r-unk*)

;; **読み返した値そのものを記録に残す**(枠組みは落ちた件しか印字しない)
(format *isiki-test-stream*
        "#editor redraw lines: none=~D move=~D insert=~D split=~D unknown=~D~%"
        *et-r-none* *et-r-move* *et-r-ins* *et-r-split* *et-r-unk*)
