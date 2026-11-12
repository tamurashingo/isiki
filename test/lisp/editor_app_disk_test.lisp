;; test/lisp/editor_app_disk_test.lisp
;;
;; [#218] **エディタをアプリ用の円盤(/app)から読み込んで、実機の画面で動かす。**
;;
;; ここでしか確かめられないものが 3 つある:
;;
;;   1. **位置指定が効いていること。** `term-read-cell` で格子を読み返すのが
;;      唯一の機械的な手段である(文字列出力ストリームへ流すと位置は消える)
;;   2. **確保量。** `term-write-at` の直しの前後も、差分再描画の効きも、
;;      実機でしか測れない(規則 9: 時間ではなく確保量)
;;   3. **素の入力が必ず戻ること。** 皮が `unwind-protect` で戻しているかは
;;      実機の状態(`term-raw-input` の返り値)でしか見えない
;;
;; 核の振る舞い(編集の意味)は test/lisp/editor_test.lisp にある。
;;
;; **この順でなければ意味を持たない。** 段(test/lisp/qemu_boot_app_editor.lisp)は
;; このファイルを**先に**読む。editor_test.lisp がホスト共有から apps/editor.lisp を
;; 読んでしまった後では、「/app から読んだから定義された」と言えなくなる。
;; だから §1 の陽性対照(読み込む前は未定義)が要る。
;;
;; ## 画面を触る試験の作法(test/lisp/term_test.lisp / filer_app_disk_test.lisp と同じ)
;;
;;   1. **描いてから読むまでを 1 つの式の中で済ませる。** 間に画面への出力が
;;      入ると行がずれて、「位置指定が効いていない」と読める偽の失敗になる。
;;      だから下の probe は defglobal で一気に取り、assert はその値に対して行う
;;   2. **読み返した値そのものを記録に残す。** 枠組みは落ちた件しか印字しない
;;
;; ## 題材は **/app/NOTES/NOTE.TXT(読むだけ)**
;;
;; **この段は円盤へ 1 バイトも書かない。** アプリ専用の円盤は make が 1 度だけ
;; 焼いて**段をまたいで共有する**ので、1 件でも足す / 中身を変えると
;; test/lisp/filer_app_disk_test.lisp の期待値が**別の段で**落ちる。
;; 保存の試験(控え・縮む上書き・2 回続けて)は母艦共有の `/9p/tmp/` に対して
;; test/lisp/editor_test.lisp が行う。
;;
;; ---------------------------------------------------------------------------
;; ## 測る前の宣言(規則 11。**走らせる前に書いた**)
;;
;; 計器は確保量(`%%HEAP-USED-BYTES` の差)を主、`%%GC-COLLECT-COUNT` を
;; **その窓が読めるかどうかの検査**として併記する(回収が起きた窓の差は読めない)。
;;
;; | #  | 宣言                                                      | 根拠 |
;; |----|-----------------------------------------------------------|------|
;; | D1 | 1 行: 直す前は **5,000 byte 以上**、直した後は **2,000 byte 以下** | 調査の実測 10,720 / 560(logic §8-1) |
;; | D2 | 全画面: 直した後は直す前の **10 分の 1 以下**            | 同 505,344 / 7,504 = 1/67 |
;; | D3 | どの窓でも回収は **0 回**                                 | 1 画面ぶんはヒープに対して小さい |
;; | D4 | 差分(1 行 + 合図)は全画面の **5 分の 1 以下**           | 行数 ぶんの比。窓は 48 行ある |
;;
;; 起動時に画面を消すこと(#230)の宣言 D5〜D9 は §8 の頭に置いた
;; (置き場所と対照の作り方の説明が長いので、使う場所の隣に書いてある)。
;;
;; **外れたら「外れた」と書く**(規則 16: 期待値を黙って上げない)。
;; 幅に余裕を持たせてあるのは、画面の大きさが起動時の解像度で決まる
;; **実行時の値**だからである(固定値ではない)。
;; ---------------------------------------------------------------------------

;;; ---------------------------------------------------------------------------
;;; 0. 前段: アプリ専用ディスクを名前で引いて割り当てる
;;;
;;;    **装置の名前(blk0/blk1/...)は探査順の連番**なので、チャネルの名前で引く。
;;; ---------------------------------------------------------------------------

(defglobal *ea-app-device* (%device-named "ide-primary-slave"))

(assert-equal t (if *ea-app-device* t nil))
(assert-equal t (if (%device-handle *ea-app-device*) t nil))
(assert-equal t (if (%device-fat32-uuid (%device-handle *ea-app-device*)) t nil))

(if (null (%filecmd-resolve-mount "/app"))
    (mount "/app" *ea-app-device* ':fat32)
  nil)
(assert-equal t (if (%filecmd-resolve-mount "/app") t nil))

;;; ---------------------------------------------------------------------------
;;; 1. 【陽性対照】読み込む前は未定義である
;;;
;;;    **これが無いと、次の節は「どこかで定義されていた」でも通る。**
;;; ---------------------------------------------------------------------------

(assert-error (editor-run "/app/NOTES/NOTE.TXT" 1 2))
(assert-error (editor "/app/NOTES/NOTE.TXT"))
(assert-error (%ed-status-text 0 0 1 nil nil))

;;; ---------------------------------------------------------------------------
;;; 2. 【完了条件】/app から読み込める
;;;
;;;    `.lisp`(拡張子 4 文字)が置けるのは FAT32 だからである。
;;; ---------------------------------------------------------------------------

(defglobal *ea-load* (load "/app/editor.lisp"))
(assert-equal t *ea-load*)

(assert-equal 16 (dynamic *editor-text-lines*))
(assert-equal 40 (dynamic *editor-row-cols*))

;;; ---------------------------------------------------------------------------
;;; 3. 道具立て: 格子の 1 行を文字列として読む
;;;
;;;    読めなかったマス(範囲外)は "?" にする。**全部 nil を返す実装では
;;;    "?????" が返って落ちる**(陰性対照がこの 1 文字に入っている)。
;;; ---------------------------------------------------------------------------

(defun %ea-row (row n)
  (let ((s "") (i 0))
    (while (< i n)
      (let ((c (term-read-cell row i)))
        (setq s (string-append s (if (null c) "?" (create-string 1 c)))))
      (setq i (+ i 1)))
    s))

;; 表示先は**実画面**である(1 回だけ取って持ち回る)
(defglobal *ea-out* (standard-output))

;; 実画面の寸法から決める。**固定値を書かない**(解像度で変わる)
(defglobal *ea-w* (- (term-cols) 1))
(defglobal *ea-h* (- (term-rows) 2))
(defglobal *ea-line* (create-string *ea-w* #\x))
(defglobal *ea-blank* (create-string *ea-w* #\space))

;;; ---------------------------------------------------------------------------
;;; 4. 【完了条件】`term-write-at` の直しの前後を**確保量で**測る(規則 9)
;;;
;;;    直す前の実装を**逐語で写してある**(src/lisp/init.lisp の #218 より前の
;;;    姿)。こうしてあるのは、**前後を同じ 1 周の中で測るため**である ——
;;;    別々の周で測ると「直したから速い」と「周が違うから違う」を分けられない。
;;;
;;;    直したあとの `term-write-at` は `(format out "~A" str)` 1 回で流す。
;;;    `~A` は `os_print_to_sink` が **C の中で**流すので、確保は長さに
;;;    ほとんど関係しない。
;;;
;;;    **陰性対照**: どちらも同じ文字数を同じ場所へ書いている(`*ea-line*` は
;;;    前もって作ってあるので、作る費用は窓に入らない)。差は書き方だけである。
;;; ---------------------------------------------------------------------------

(defun %ea-write-at-old (row col str out)
  (if (null (term-set-cursor row col))
      nil
    (let ((i 0) (n (length str)))
      (while (< i n)
        (write-char (elt str i) out)
        (setq i (+ i 1)))
      t)))

(defun %ea-screen-old (h line out)
  (let ((i 0))
    (while (< i h) (%ea-write-at-old i 0 line out) (setq i (+ i 1)))))

(defun %ea-screen-new (h line out)
  (let ((i 0))
    (while (< i h) (term-write-at i 0 line out) (setq i (+ i 1)))))

;; (確保量 回収回数) を返す
(defglobal *ea-m-old-line*
  (let ((g0 (%%gc-collect-count)) (b0 (%%heap-used-bytes)))
    (%ea-write-at-old 0 0 *ea-line* *ea-out*)
    (list (- (%%heap-used-bytes) b0) (- (%%gc-collect-count) g0))))

(defglobal *ea-m-new-line*
  (let ((g0 (%%gc-collect-count)) (b0 (%%heap-used-bytes)))
    (term-write-at 0 0 *ea-line* *ea-out*)
    (list (- (%%heap-used-bytes) b0) (- (%%gc-collect-count) g0))))

(defglobal *ea-m-old-screen*
  (let ((g0 (%%gc-collect-count)) (b0 (%%heap-used-bytes)))
    (%ea-screen-old *ea-h* *ea-line* *ea-out*)
    (list (- (%%heap-used-bytes) b0) (- (%%gc-collect-count) g0))))

(defglobal *ea-m-new-screen*
  (let ((g0 (%%gc-collect-count)) (b0 (%%heap-used-bytes)))
    (%ea-screen-new *ea-h* *ea-line* *ea-out*)
    (list (- (%%heap-used-bytes) b0) (- (%%gc-collect-count) g0))))

;; D3: どの窓でも回収は 0 回(起きていたら上の差は読めない)
(assert-equal 0 (car (cdr *ea-m-old-line*)))
(assert-equal 0 (car (cdr *ea-m-new-line*)))
(assert-equal 0 (car (cdr *ea-m-old-screen*)))
(assert-equal 0 (car (cdr *ea-m-new-screen*)))

;; D1: 1 行は 5,000 byte 以上 → 2,000 byte 以下へ
(assert-equal t (> (car *ea-m-old-line*) 5000))
(assert-equal t (< (car *ea-m-new-line*) 2000))

;; D2: 全画面は 10 分の 1 以下へ
(assert-equal t (< (car *ea-m-new-screen*) (div (car *ea-m-old-screen*) 10)))

;; **実測値そのものを記録へ残す。** 通っているあいだ何が出ていたかを
;; ログに残さないと、あとから「どれくらい効いたか」を言えなくなる(規則 13)
(format *isiki-test-stream*
        "#editor write-at rows=~D cols=~D w=~D h=~D | 1-line old=~D new=~D | screen old=~D new=~D~%"
        (term-rows) (term-cols) *ea-w* *ea-h*
        (car *ea-m-old-line*) (car *ea-m-new-line*)
        (car *ea-m-old-screen*) (car *ea-m-new-screen*))

;;; ---------------------------------------------------------------------------
;;; 5. 【完了条件】差分再描画
;;;
;;;    2 つのことを見る:
;;;
;;;    (a) **確保量で効いている**(全画面 対 1 行 + 合図)
;;;    (b) **本当に変わった行しか触っていない** —— 全画面を描いたあとで
;;;        別の行へ印を付け、1 行だけ描き直して、**印が残っていること**を
;;;        格子で見る。全部を描き直す実装はここで落ちる(**陽性対照**)。
;;; ---------------------------------------------------------------------------

(defglobal *ea-wlines* (%ed-window-lines nil "x" nil 0 0))

(defglobal *ea-m-draw-full*
  (let ((g0 (%%gc-collect-count)) (b0 (%%heap-used-bytes)))
    (%ed-draw-all *ea-wlines* *ea-h* 0 0 1 nil nil *ea-w* *ea-blank* *ea-out*)
    (list (- (%%heap-used-bytes) b0) (- (%%gc-collect-count) g0))))

(defglobal *ea-m-draw-line*
  (let ((g0 (%%gc-collect-count)) (b0 (%%heap-used-bytes)))
    (%ed-draw-line 0 "x" *ea-w* *ea-blank* *ea-out*)
    (%ed-draw-status *ea-h* 0 0 1 nil nil *ea-w* *ea-blank* *ea-out*)
    (list (- (%%heap-used-bytes) b0) (- (%%gc-collect-count) g0))))

(assert-equal 0 (car (cdr *ea-m-draw-full*)))
(assert-equal 0 (car (cdr *ea-m-draw-line*)))
;; D4: 差分は全画面の 5 分の 1 以下
(assert-equal t (< (car *ea-m-draw-line*) (div (car *ea-m-draw-full*) 5)))

(format *isiki-test-stream*
        "#editor redraw bytes: full=~D diff=~D (h=~D w=~D)~%"
        (car *ea-m-draw-full*) (car *ea-m-draw-line*) *ea-h* *ea-w*)

;; (b) **変わった行しか触っていない**ことの証拠。
;;     桁 20・窓 4 行の小さい割り付けで、行 2 に印を付けてから行 0 だけ描き直す
(defglobal *ea-blank20* (create-string 20 #\space))

(defglobal *ea-diff*
  (progn
    (%ed-draw-all (list "one" "two" "three") 4 0 0 3 nil nil 20 *ea-blank20* *ea-out*)
    (term-write-at 2 0 "ZZZZ" *ea-out*)
    (%ed-draw-line 0 "ONE!" 20 *ea-blank20* *ea-out*)
    (list (%ea-row 0 8) (%ea-row 1 8) (%ea-row 2 8) (%ea-row 3 8))))

;; 行 0 は描き直されている
(assert-equal "ONE!    " (elt *ea-diff* 0))
;; 行 1 はそのまま
(assert-equal "two     " (elt *ea-diff* 1))
;; **行 2 の印が残っている = 触っていない**(全部描き直す実装なら "three   ")
(assert-equal "ZZZZe   " (elt *ea-diff* 2))
;; 文書より下は空行で埋まっている(全画面のときだけ消える)
(assert-equal "        " (elt *ea-diff* 3))

(format *isiki-test-stream*
        "#editor diff-rows row0=~S row1=~S row2=~S row3=~S~%"
        (elt *ea-diff* 0) (elt *ea-diff* 1) (elt *ea-diff* 2) (elt *ea-diff* 3))

;;; ---------------------------------------------------------------------------
;;; 6. 【完了条件】位置指定が効いていて、本文と合図が**格子の上に**在る
;;;
;;;    打鍵を 1 つも渡さない。核は 1 画面描いてから打鍵を読み、尽きて終わる ——
;;;    **格子には最後に描いた 1 画面が残る。**
;;;
;;;    題材は /app/NOTES/NOTE.TXT = "alpha\nbeta\n"(Makefile が焼いた 11 バイト)。
;;;    **読むだけで、書かない。**
;;;
;;;    **陰性対照がここに入っている:**
;;;      - 位置を無視して上から流す実装 … 合図が行 16 に無いので落ちる
;;;      - 全部 nil を返す実装 … "?" が並んで落ちる
;;;      - 文書より下を消さない実装 … 行 2 が空白でないので落ちる
;;; ---------------------------------------------------------------------------

(defglobal *ea-note* "/app/NOTES/NOTE.TXT")

(defglobal *ea-screen1*
  (let ((r (editor-run *ea-note* (create-string-input-stream "") *ea-out*)))
    (list r (%ea-row 0 8) (%ea-row 1 8) (%ea-row 2 8) (%ea-row 16 30))))

(assert-equal (list 'editor-eof 0 0 2) (elt *ea-screen1* 0))
(assert-equal "alpha   " (elt *ea-screen1* 1))
(assert-equal "beta    " (elt *ea-screen1* 2))
(assert-equal "        " (elt *ea-screen1* 3))
(assert-equal "L1 C1 of 2 - C-s=save C-q=quit" (elt *ea-screen1* 4))

(format *isiki-test-stream*
        "#editor screen row0=~S row1=~S row2=~S status=~S~%"
        (elt *ea-screen1* 1) (elt *ea-screen1* 2)
        (elt *ea-screen1* 3) (elt *ea-screen1* 4))

;;; ---------------------------------------------------------------------------
;;; 7. 【完了条件】カーソルを動かすと、**合図の数字が格子の上で変わる**
;;;
;;;    **これが「カーソルを動かせる」ことの機械的な証拠である。**
;;;    カーソルそのものの位置を読む口は無い(#216 の 5 つに無い)ので、
;;;    数字を出す以外に機械で言う方法が無い
;;;    (apps/editor.lisp 頭の「カーソルの位置は合図の数字が主」)。
;;;
;;;    ★ **実機の打鍵そのものは自動で試験できない**(ゲストへキーを送る仕組みが
;;;    無い)。ここで固定しているのは「同じ文字が届けば動く」ことまでである。
;;;    矢印が文字として届くことは test/c/keymap_test.c と term_test.lisp が見ている。
;;; ---------------------------------------------------------------------------

(defglobal *ea-screen2*
  (let ((r (editor-run *ea-note*
                       (create-string-input-stream (create-string 1 *key-down*))
                       *ea-out*)))
    (list r (%ea-row 16 12))))

(assert-equal (list 'editor-eof 0 0 2) (elt *ea-screen2* 0))
(assert-equal "L2 C1 of 2 -" (elt *ea-screen2* 1))

(defglobal *ea-screen3*
  (let ((r (editor-run *ea-note*
                       (create-string-input-stream (create-string 1 *key-right*))
                       *ea-out*)))
    (list r (%ea-row 16 12))))

(assert-equal (list 'editor-eof 0 0 2) (elt *ea-screen3* 0))
(assert-equal "L1 C2 of 2 -" (elt *ea-screen3* 1))

(format *isiki-test-stream*
        "#editor cursor down=~S right=~S~%"
        (elt *ea-screen2* 1) (elt *ea-screen3* 1))

;;; ---------------------------------------------------------------------------
;;; 8. 【完了条件 / 陽性対照】**起動時に 1 回だけ**画面を消す(#230)
;;;
;;;    利用者が実機で踏んだ不具合である: エディタを起動すると、それまで
;;;    コンソールに出ていた文字の上に本文やコマンドが重なって出る。
;;;    差分再描画(§5)は**意図して塗り直さない**ので、
;;;    **起動時に 1 回消す**以外に前の表示を落とす機会が無い。
;;;
;;;    ## 見張りの文字を置く場所 —— **エディタが描かない行**
;;;
;;;    核が触るのは 行 0..h-1(本文)と 行 h(合図)だけで、
;;;    **行 h+1 以降は 1 マスも書かない**(ファイル頭「画面の割り付け」)。
;;;    だから見張りを最下行に置けば、消えたのは `term-clear` が走ったからだと
;;;    言える。**本文の行に置くと、描き直しで上書きされたのと見分けがつかない**
;;;    —— 置き場所そのものが陰性対照になっている。
;;;
;;;    ## 断る経路で `out` に文字列ストリームを渡す理由
;;;
;;;    断りの文面を実画面へ出すと**行送りで画面が巻き上がり**、見張りの行が
;;;    1 行ずれて「消えた」と読める**偽の失敗**になる。文字列ストリームへ流せば
;;;    実画面に触りうるのは `term-clear` だけなので、**見張りの生死が
;;;    「消したか・消さなかったか」だけを指す。**
;;;
;;;    ## 測る前の宣言(規則 11。**走らせる前に書いた**)
;;;
;;;    | #  | 宣言                                                      | 根拠 |
;;;    |----|-----------------------------------------------------------|------|
;;;    | D5 | 起動すると見張りは**空白**になっている                    | 起動時に 1 回消す |
;;;    | D6 | 511 文字以上の行で断ったときは見張りが**残っている**      | 断る検査より後で消す |
;;;    | D7 | 行数の上限で断ったときも見張りが**残っている**            | 同じ |
;;;    | D8 | **1 打鍵あたり**の増分は**全画面 1 回ぶん(§5 の full)より小さい** | 消すのは 1 回だけ・右移動は合図だけ描く |
;;;    | D10| 行を分ける打鍵(全画面)4 つは、右移動 4 つより**全画面 1 回ぶん以上**高い | 打鍵ごとに全画面を描く実装ではこの差が消える |
;;;    | D9 | D8 / D10 の窓で回収は **0 回**                            | §4 は 495,008 byte の窓でも 0 回だった |
;;;
;;;    ### **D8 は 1 回外した。外れたまま書き残す**(規則 16)
;;;
;;;    最初に宣言したのは「**打鍵 4 つぶんの増分**が全画面 1 回ぶんより小さい」で、
;;;    実測は **37,696 対 35,968 で外れた**(1 打鍵あたり 9,424 byte)。
;;;    外れた理由は描き直しではなく**周回そのもの**である —— 1 周には打鍵の読み取り
;;;    (`term-read-key`)と `cond` の判定が入り、解釈実行ではそれ自体が
;;;    合図 1 回(§5 の diff = 2,704)より高い。**直したのは宣言のほうで、
;;;    期待値を緩めたのではない** —— 言いたかったのは
;;;    「**1 打鍵で全画面 1 枚ぶんを払っていない**」であり、
;;;    それは打鍵あたりで比べなければ言えない。
;;;    打鍵あたりでなお足りない差を埋めるために D10 を足した(こちらは
;;;    **同じ周回費用どうしの比較**なので、周回の費用が差で消える)。
;;;
;;;    **外れたら「外れた」と書く**(規則 16: 期待値を黙って上げない)。
;;; ---------------------------------------------------------------------------

;; 見張りの行は**実画面の寸法から導く**(固定値を書かない。規則 13)。
;; 合図の行(16)より下でなければ意味を持たないので、そこも検査する
(defglobal *ea-watch-row* (- (term-rows) 1))
(assert-equal t (> *ea-watch-row* 16))

;; D5: 起動したら見張りが消える。
;;     **置いてから読み返すまでを 1 つの式で済ませる**(間に出力が入ると行がずれる)。
;;     先頭の "WATCH" は**探針そのものの陰性対照**である —— 置けていなければ
;;     「消えた」も意味を持たない
(defglobal *ea-watch-start*
  (progn
    (term-write-at *ea-watch-row* 0 "WATCH" *ea-out*)
    (let ((before (%ea-row *ea-watch-row* 5)))
      (let ((r (editor-run *ea-note* (create-string-input-stream "") *ea-out*)))
        (list before (%ea-row *ea-watch-row* 5) r (%ea-row 0 8))))))

(assert-equal "WATCH" (elt *ea-watch-start* 0))
(assert-equal "     " (elt *ea-watch-start* 1))
;; 消したあとに本文が描かれている(**消すだけして描かない**実装はここで落ちる)
(assert-equal (list 'editor-eof 0 0 2) (elt *ea-watch-start* 2))
(assert-equal "alpha   " (elt *ea-watch-start* 3))

;; 断る経路の題材。**/app へは 1 バイトも書かない**ので母艦共有へ置く
;; (test/lisp/editor_test.lisp §12 と同じ置き場。`~A` はディスクに対して
;;  安全でないと注記があるので 1 文字ずつ書く)
(defglobal *ea-long-path* "/9p/tmp/editor_clear_long.txt")

(defglobal *ea-long-put*
  (progn
    (with-open-output-file (dst *ea-long-path*)
      (let ((s (create-string 600 #\a)) (i 0))
        (while (< i 600) (write-char (elt s i) dst) (setq i (+ i 1)))
        (write-char #\newline dst)))
    t))
(assert-equal t *ea-long-put*)
(assert-equal 'ed-too-long (%ed-read-lines *ea-long-path*))

;; D6: 511 文字以上の行で断ったときは消さない
(defglobal *ea-watch-refuse-long*
  (progn
    (term-write-at *ea-watch-row* 0 "WATCH" *ea-out*)
    (let ((o (create-string-output-stream)))
      (let ((r (editor-run *ea-long-path* (create-string-input-stream "") o)))
        (list (%ea-row *ea-watch-row* 5) r (get-output-stream-string o))))))

(assert-equal "WATCH" (elt *ea-watch-refuse-long* 0))
(assert-equal (list 'editor-too-long 0 0 0) (elt *ea-watch-refuse-long* 1))
(assert-equal t (if (string-index "not opened" (elt *ea-watch-refuse-long* 2) 0) t nil))

;; D7: 行数の上限で断ったときも消さない(**もう 1 つの早期離脱**)
(defglobal *ea-watch-refuse-max*
  (progn
    (term-write-at *ea-watch-row* 0 "WATCH" *ea-out*)
    (let ((o (create-string-output-stream)))
      (let ((r (dynamic-let ((*editor-max-lines* 1))
                 (editor-run *ea-note* (create-string-input-stream "") o))))
        (list (%ea-row *ea-watch-row* 5) r (get-output-stream-string o))))))

(assert-equal "WATCH" (elt *ea-watch-refuse-max* 0))
(assert-equal (list 'editor-too-long 0 0 2) (elt *ea-watch-refuse-max* 1))
(assert-equal t (if (string-index "too many lines" (elt *ea-watch-refuse-max* 2) 0) t nil))

;;; D8 / D10: **消すのは起動時の 1 回だけ**であることを確保量で言う(規則 9)。
;;;
;;;     打鍵なしの 1 周・右へ 4 回・行を分けるを 4 回、の 3 つを同じ尺度で測る。
;;;     ファイルを読む費用は**どの窓にも等しく入る**ので差で消える(陰性対照)。
;;;
;;;     D8  … (run4 - run0) / 4 < full。**1 打鍵で全画面 1 枚ぶんを払っていない。**
;;;           打鍵ごとに消して全画面を描く実装なら、1 打鍵あたりが full を超える
;;;     D10 … run-split4 - run4 > full。右移動は合図だけ・行を分けると全画面
;;;           なので、**4 打鍵ぶんで全画面 4 枚ぶんの差が開く。**
;;;           打鍵ごとに全画面を描く実装では**この差がほぼ 0 になる** ——
;;;           周回の費用が両辺で等しいので、差は描き直しの違いだけを指す。
;;;           ★ **閾値の `full` は §5 の値(h=48 / w=159)で、核が実際に描く
;;;           全画面(皮を通さないので h=16 / w=40)より大きい。** つまり
;;;           この閾値は**安全側に厳しい**(実測 43,232 = 1 打鍵あたり 10,808 で、
;;;           これが核の全画面 1 枚ぶんにあたる)。**大きいほうで比べて通る**ので
;;;           そのままにしてある —— 核が窓の寸法を引数で受ける形を崩さずに
;;;           §5 と同じ尺度を使うには、こう置くしかない
(defglobal *ea-m-run0*
  (let ((g0 (%%gc-collect-count)) (b0 (%%heap-used-bytes)))
    (editor-run *ea-note* (create-string-input-stream "") *ea-out*)
    (list (- (%%heap-used-bytes) b0) (- (%%gc-collect-count) g0))))

(defglobal *ea-m-run4*
  (let ((g0 (%%gc-collect-count)) (b0 (%%heap-used-bytes)))
    (editor-run *ea-note*
                (create-string-input-stream (create-string 4 *key-right*))
                *ea-out*)
    (list (- (%%heap-used-bytes) b0) (- (%%gc-collect-count) g0))))

(defglobal *ea-m-run-split4*
  (let ((g0 (%%gc-collect-count)) (b0 (%%heap-used-bytes)))
    (editor-run *ea-note*
                (create-string-input-stream (create-string 4 #\newline))
                *ea-out*)
    (list (- (%%heap-used-bytes) b0) (- (%%gc-collect-count) g0))))

;; D9: 窓が読めること
(assert-equal 0 (car (cdr *ea-m-run0*)))
(assert-equal 0 (car (cdr *ea-m-run4*)))
(assert-equal 0 (car (cdr *ea-m-run-split4*)))
;; D8: **1 打鍵あたり**の増分 < 全画面 1 回ぶん
(assert-equal t (< (div (- (car *ea-m-run4*) (car *ea-m-run0*)) 4)
                   (car *ea-m-draw-full*)))
;; D10: 全画面を描く打鍵と合図だけの打鍵の差が、全画面 1 回ぶん以上開いている
(assert-equal t (> (- (car *ea-m-run-split4*) (car *ea-m-run4*))
                   (car *ea-m-draw-full*)))

;; **読み返した値そのものを記録へ残す**(規則 13。枠組みは落ちた件しか印字しない)
(format *isiki-test-stream*
        "#editor clear-on-start watch-row=~D start=~S refuse-long=~S refuse-max=~S | run0=~D run4=~D split4=~D per-key=~D full=~D~%"
        *ea-watch-row*
        (elt *ea-watch-start* 1) (elt *ea-watch-refuse-long* 0)
        (elt *ea-watch-refuse-max* 0)
        (car *ea-m-run0*) (car *ea-m-run4*) (car *ea-m-run-split4*)
        (div (- (car *ea-m-run4*) (car *ea-m-run0*)) 4) (car *ea-m-draw-full*))

;;; ---------------------------------------------------------------------------
;;; 9. 【完了条件 / 陽性対照】素の入力が**必ず**戻る
;;;
;;;    #215 で実際に踏んだバグがこれである。「終わりに戻す」と素直に書くと、
;;;    **途中で脱出したとき素の入力が入ったまま残り、以後 REPL で打鍵が
;;;    画面に出なくなる。**
;;;
;;;    ## 脱出させ方: **開けないファイルを渡す**
;;;
;;;    `with-open-input-file` が <simple-error> を signal する。
;;;    **これは #215 で踏んだのとまったく同じ経路である**(わざと壊した
;;;    身代わりではなく、実際に起こりうる並びをそのまま使っている)。
;;;
;;;    **探針そのものの陰性対照も置く**: 入れてから切ると t が返る ——
;;;    つまりこの探針は 2 つの状態を見分けている。
;;;
;;;    **ここから先は皮を通すので画面が消える**(皮は終わりに `term-clear` する)。
;;;    格子を見る節(§4〜§8)より**後に置くこと。**
;;; ---------------------------------------------------------------------------

(assert-equal nil (term-raw-input nil))
(assert-equal nil (term-raw-input t))
(assert-equal t (term-raw-input nil))

(assert-error
  (with-standard-input (create-string-input-stream "")
    (with-standard-output (create-string-output-stream)
      (editor "/no-such-mount/x.txt"))))

(defglobal *ea-raw-after-escape* (term-raw-input nil))
(assert-equal nil *ea-raw-after-escape*)

;; 普通に終わった場合も戻る(脱出のときだけ戻る実装では通らない)
(defglobal *ea-shell*
  (with-standard-input (create-string-input-stream (create-string 1 (code-char 17)))
    (with-standard-output (create-string-output-stream)
      (editor *ea-note*))))
(defglobal *ea-raw-after-normal* (term-raw-input nil))

(assert-equal (list 'editor-quit 0 0 2) *ea-shell*)
(assert-equal nil *ea-raw-after-normal*)

(format *isiki-test-stream*
        "#editor raw-input: after-escape=~S after-normal=~S shell=~S~%"
        *ea-raw-after-escape* *ea-raw-after-normal* *ea-shell*)

;;; ---------------------------------------------------------------------------
;;; 10. 皮が実画面に合わせて切り下げる
;;;
;;;    皮が使う式(`%ed-clamp` と上限 `(- (term-rows) 2)` / `(- (term-cols) 1)`)を
;;;    ここに写してある。**式を変えたらここが落ちる。それが狙いである。**
;;;
;;;    行を 2 つ空ける理由: 本文 h + 合図 1 で、**最下行は空けておく**
;;;    (合図の行の後ろの行送りで画面が巻き上がると、格子が 1 行ずれる)。
;;;    桁を 1 つ空ける理由: **最終桁に書くと次の行へ折り返す**。
;;; ---------------------------------------------------------------------------

(assert-equal t (>= (- (term-rows) 2) 1))
(assert-equal t (<= (+ (%ed-clamp (dynamic *editor-text-lines*) (- (term-rows) 2)) 2)
                    (term-rows)))
(assert-equal 16 (%ed-clamp (dynamic *editor-text-lines*) (- (term-rows) 2)))
(assert-equal 40 (%ed-clamp (dynamic *editor-row-cols*) (- (term-cols) 1)))
