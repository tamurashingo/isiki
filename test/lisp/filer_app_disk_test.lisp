;; test/lisp/filer_app_disk_test.lisp
;;
;; [#217] **ファイラをアプリ用の円盤(/app)から読み込んで、実機の画面で動かす。**
;;
;; ここでしか確かめられないものが 2 つある:
;;
;;   1. **位置指定が効いていること。** `term-read-cell` で格子を読み返すのが
;;      唯一の機械的な手段である(文字列出力ストリームへ流すと位置は消える)。
;;      選択の記号が**格子の上で動く**ことも、ここでしか言えない
;;   2. **素の入力が必ず戻ること。** 皮が `unwind-protect` で戻しているかは
;;      実機の状態(`term-raw-input` の返り値)でしか見えない
;;
;; 核の振る舞い(一覧・移動・入る・戻る・開く)は test/lisp/filer_test.lisp にある。
;;
;; **この順でなければ意味を持たない。** 段(test/lisp/qemu_boot_app_filer.lisp)は
;; このファイルを**先に**読む。filer_test.lisp がホスト共有から apps/filer.lisp を
;; 読んでしまった後では、「/app から読んだから定義された」と言えなくなる。
;; だから §1 の陽性対照(読み込む前は未定義)が要る。
;;
;; 置き場所の約束は apps/pager.lisp / apps/hello.lisp と同じで、原本は apps/ に
;; あり、ビルドがアプリ専用の FAT32 ディスク(Makefile の APP_DISK_IMG)へ焼き込む。
;; **自動の割り当ては 1 つも無い**ので、読む前に mount を呼ぶ前段が必ず要る。
;;
;; ## 画面を触る試験の作法(test/lisp/term_test.lisp と同じ)
;;
;;   1. **描いてから読むまでを 1 つの式の中で済ませる。** 間に画面への出力が
;;      入ると行がずれて、「位置指定が効いていない」と読める偽の失敗になる。
;;      だから下の probe は defglobal で一気に取り、assert はその値に対して行う
;;   2. **画面を消すのはアプリに任せる。** `%filer-draw` が毎回 `term-clear` を
;;      呼ぶので、起動の表示や load の進捗が残っていても消える
;;      (= 「書いていないマスは空白」を見てよい)

;;; ---------------------------------------------------------------------------
;;; 0. 前段: アプリ専用ディスクを名前で引いて割り当てる
;;;
;;;    **装置の名前(blk0/blk1/...)は探査順の連番**なので、付けるディスクの構成が
;;;    変わると同じディスクの名前が動く。構成に依らず同じディスクを指すため、
;;;    チャネルの名前で引く(%device-named、src/lisp/device.lisp)。
;;; ---------------------------------------------------------------------------

(defglobal *fd-app-device* (%device-named "ide-primary-slave"))

;; **陽性対照: 名前引きが当たっていること。** 当たっていなければ以後の mount は
;; (nil . :fat32) を積むだけで、「読めない」が別の理由で出てしまう
(assert-equal t (if *fd-app-device* t nil))
(assert-equal t (if (%device-handle *fd-app-device*) t nil))
;; アプリのディスクは FAT32 である(ここが nil なら焼き込みか結線が狂っている)
(assert-equal t (if (%device-fat32-uuid (%device-handle *fd-app-device*)) t nil))

(mount "/app" *fd-app-device* ':fat32)

;;; ---------------------------------------------------------------------------
;;; 1. 【陽性対照】読み込む前は未定義である
;;;
;;;    **これが無いと、次の節は「どこかで定義されていた」でも通る。**
;;;    ファイラの関数が 1 つも無いことを先に固定しておく。
;;;
;;;    皮(filer)を呼ぶ側の対照も置く。**定義されていたら打鍵を待って止まる**が、
;;;    この段でこのファイルが最初に走るかぎり、そうはならない
;;;    (pager_app_disk_test.lisp §1 と同じ形)。
;;; ---------------------------------------------------------------------------

(assert-error (filer-run "/app" 1 2))
(assert-error (filer "/app"))
(assert-error (%filer-list "/app"))

;;; ---------------------------------------------------------------------------
;;; 2. 【完了条件】/app から読み込める
;;;
;;;    `.lisp`(拡張子 4 文字)が置けるのは FAT32 だからである。
;;;
;;;    **ページャはまだ読まない。** ファイラはファイルを開くのに `pager-run` を
;;;    呼ぶ(核を呼ぶ。皮ではない)ので、読まないあいだは「開く」だけが
;;;    未定義のままである —— §7 がその状態を**脱出の対照**として使う。
;;;    読み込むのは §8 で、**同じ打鍵が signal から成功へ変わる**ことを見る。
;;; ---------------------------------------------------------------------------

(defglobal *fd-load-filer* (load "/app/filer.lisp"))
(assert-equal t *fd-load-filer*)

;; 読み込んだ結果として、核と皮と一覧の大きさが在る
(assert-equal 16 (dynamic *filer-list-lines*))
(assert-equal 40 (dynamic *filer-row-cols*))

;;; ---------------------------------------------------------------------------
;;; 2b. 題材の確認: /app の一覧
;;;
;;;     **期待値を画面で見る前に、データで固定しておく。** こうしておくと、
;;;     下の格子の試験が落ちたときに「画面が違う」のか「円盤の中身が違う」のかを
;;;     切り分けられる(apps/*.lisp を足すとここが先に落ちる)。
;;;
;;;     NOTES は Makefile の APP_DISK_SUBDIR で、**cp より先に作ってあるので
;;;     一覧の先頭に来る**(FAT の項目は作成順)。読むだけのフィクスチャである。
;;; ---------------------------------------------------------------------------

(defglobal *fd-entries* (%filer-list "/app"))

(assert-equal 7 (%filer-length *fd-entries*))
(assert-equal "NOTES" (%filer-entry-name (%filer-nth *fd-entries* 0)))
(assert-equal t (%filer-dir-p (%filer-nth *fd-entries* 0)))
;; **陰性対照: すべてディレクトリと答える実装では通らない**
(assert-equal nil (%filer-dir-p (%filer-nth *fd-entries* 1)))
(assert-equal "editor.lisp" (%filer-entry-name (%filer-nth *fd-entries* 1)))
;; NOTES の中身(3 件 = "." / ".." / NOTE.TXT)。大きさは 11 バイト("alpha\nbeta\n")
(defglobal *fd-notes* (%filer-list "/app/NOTES"))
(assert-equal 3 (%filer-length *fd-notes*))
(assert-equal "NOTE.TXT" (%filer-entry-name (%filer-nth *fd-notes* 2)))
(assert-equal 11 (%filer-entry-size (%filer-nth *fd-notes* 2)))

;;; ---------------------------------------------------------------------------
;;; 3. 道具立て: 格子の 1 行を文字列として読む
;;;
;;;    **`term-read-cell` だけが画面の検査を可能にする。** 1 マスずつ assert を
;;;    並べると件数が膨らむうえ読みにくいので、行を文字列へ組み直して比べる。
;;;
;;;    読めなかったマス(範囲外)は "?" にする。**全部 nil を返す実装では
;;;    "?????" が返って落ちる**(陰性対照がこの 1 文字に入っている)。
;;; ---------------------------------------------------------------------------

(defun %fd-row (row n)
  (let ((s "") (i 0))
    (while (< i n)
      (let ((c (term-read-cell row i)))
        (setq s (string-append s (if (null c) "?" (create-string 1 c)))))
      (setq i (+ i 1)))
    s))

;; 表示先は**実画面**である(1 回だけ取って持ち回る。呼び直すと毎回約 1.6KB 確保)。
;; 枠組みの合否はホスト共有のファイルへ行くので、画面はアプリが独占できる
(defglobal *fd-out* (standard-output))

;;; ---------------------------------------------------------------------------
;;; 4. 【完了条件 / 陽性対照】位置指定が効いていて、選択の記号が画面に在る
;;;
;;;    打鍵を 1 つも渡さない(空の文字列入力ストリーム)。核は 1 画面描いてから
;;;    打鍵を読み、尽きているので終わる —— **格子には最後に描いた 1 画面が残る。**
;;;
;;;    見るもの:
;;;      行 0        見出し(場所と項目数)
;;;      行 1        選択中の項目。**頭に "> " が在る**
;;;      行 2        次の項目。**頭は空白**(= 記号を全行に置いていない)
;;;      行 8        合図(7 件 + 見出し 1 の下)
;;;      行 9        **空**(= 合図の行より下を書いていない)
;;;
;;;    [#218] **行が 1 つ下がったのは apps/editor.lisp が増えたからである。**
;;;    `/app` の中身は `$(wildcard apps/*.lisp)` で決まるので、アプリを足すと
;;;    ここが動く。**期待値を上げたのではなく、題材が変わった**(規則 16)。
;;;
;;;    **陰性対照がこの 5 行に入っている:**
;;;      - 位置を無視して上から流す実装 … 行 0 以外が空白になり落ちる
;;;      - 全部 nil を返す実装 … "?" が並んで落ちる
;;;      - 記号を全行に置く実装 … 行 2 の頭で落ちる
;;;      - 種別と大きさを出さない実装 … 行 1 の "<DIR>" で落ちる
;;; ---------------------------------------------------------------------------

(defglobal *fd-screen1*
  (let ((r (filer-run "/app" (create-string-input-stream "") *fd-out*)))
    (list r
          (%fd-row 0 13)
          (%fd-row 1 14)
          (%fd-row 2 14)
          (%fd-row 8 25)
          (%fd-row 9 8))))

;; 1 画面描いて、打鍵が尽きて終わった(動いた回数 0・開いた本数 0・場所は /app)
(assert-equal (list 'filer-eof 0 0 "/app") (elt *fd-screen1* 0))
(assert-equal "/app  7 items" (elt *fd-screen1* 1))
(assert-equal "> NOTES  <DIR>" (elt *fd-screen1* 2))
(assert-equal "  editor.lisp " (elt *fd-screen1* 3))
(assert-equal "up/down enter u=up q=quit" (elt *fd-screen1* 4))
(assert-equal "        " (elt *fd-screen1* 5))

;; **読み返した値そのものを記録に残す**(term_test.lisp と同じ理由)。
;; 枠組みは落ちた件だけを印字するので、通っているあいだ「何が返ったか」が
;; ログに出ない。画面の検査は #217 の完了条件そのものなので、値を行で残す
(format *isiki-test-stream*
        "#filer screen rows=~D cols=~D row0=~S row1=~S row2=~S row8=~S row9=~S~%"
        (term-rows) (term-cols)
        (elt *fd-screen1* 1) (elt *fd-screen1* 2) (elt *fd-screen1* 3)
        (elt *fd-screen1* 4) (elt *fd-screen1* 5))

;;; ---------------------------------------------------------------------------
;;; 5. 【完了条件 / 陽性対照】選択の記号が**格子の上で動く**
;;;
;;;    下向きの矢印を 1 つ渡す。**記号が行 1 から行 2 へ移り、行 1 の頭が
;;;    空白に戻る**ことを格子で見る。
;;;
;;;    **これが「上下に選択を動かせる」ことの機械的な証拠である。**
;;;    カーソルを主の示し方にしていたら、ここは確かめられなかった
;;;    (カーソルの位置を読む口が無い。apps/filer.lisp の頭の説明を参照)。
;;;
;;;    矢印が文字として届くことは test/c/keymap_test.c と term_test.lisp §5 が
;;;    見ている。★ **実機の打鍵そのものは自動で試験できない**(ゲストへキーを
;;;    送る仕組みが無い)。ここで固定しているのは「同じ文字が届けば動く」ことまで。
;;; ---------------------------------------------------------------------------

(defglobal *fd-screen2*
  (let ((r (filer-run "/app"
                      (create-string-input-stream (create-string 1 *key-down*))
                      *fd-out*)))
    (list r (%fd-row 1 14) (%fd-row 2 14))))

(assert-equal (list 'filer-eof 1 0 "/app") (elt *fd-screen2* 0))
(assert-equal "  NOTES  <DIR>" (elt *fd-screen2* 1))
(assert-equal "> editor.lisp " (elt *fd-screen2* 2))

(format *isiki-test-stream*
        "#filer move row1=~S row2=~S~%" (elt *fd-screen2* 1) (elt *fd-screen2* 2))

;;; ---------------------------------------------------------------------------
;;; 6. 【完了条件 / 陽性対照】ディレクトリに入ると画面がそのディレクトリになる
;;;
;;;    改行を 1 つ渡す。選択は先頭(NOTES、ディレクトリ)なので中へ入り、
;;;    **次の画面は /app/NOTES の一覧になる。**
;;;
;;;    行 3 が **"  NOTE.TXT  11"** であることが「**種別と大きさを出す**」の
;;;    陽性対照である(ディレクトリは `<DIR>`、ファイルはバイト数)。
;;;    11 は Makefile が焼いた "alpha\nbeta\n" の長さで、**読むだけなので動かない。**
;;; ---------------------------------------------------------------------------

(defglobal *fd-screen3*
  (let ((r (filer-run "/app"
                      (create-string-input-stream (create-string 1 #\newline))
                      *fd-out*)))
    (list r
          (%fd-row 0 19)
          (%fd-row 1 10)
          (%fd-row 2 11)
          (%fd-row 3 14)
          (%fd-row 4 25))))

(assert-equal (list 'filer-eof 1 0 "/app/NOTES") (elt *fd-screen3* 0))
(assert-equal "/app/NOTES  3 items" (elt *fd-screen3* 1))
(assert-equal "> .  <DIR>" (elt *fd-screen3* 2))
(assert-equal "  ..  <DIR>" (elt *fd-screen3* 3))
(assert-equal "  NOTE.TXT  11" (elt *fd-screen3* 4))
(assert-equal "up/down enter u=up q=quit" (elt *fd-screen3* 5))

(format *isiki-test-stream*
        "#filer enter row0=~S row1=~S row3=~S~%"
        (elt *fd-screen3* 1) (elt *fd-screen3* 2) (elt *fd-screen3* 4))

;;; ---------------------------------------------------------------------------
;;; 7. 【完了条件 / 陽性対照】素の入力が**必ず**戻る
;;;
;;;    #215 で実際に踏んだバグがこれである。「終わりに戻す」と素直に書くと、
;;;    **途中で脱出したとき素の入力が入ったまま残り、以後 REPL で打鍵が
;;;    画面に出なくなる。**
;;;
;;;    ## 脱出させ方: **ページャを読み込む前にファイルを開かせる**
;;;
;;;    ファイルを開く道は `pager-run` を呼ぶ 1 本だけで、ここまで
;;;    apps/pager.lisp を読んでいないので**未定義の関数**になる ——
;;;    主ループの奥から signal が飛び、`unwind-protect` が無ければ
;;;    皮の「戻す」行まで来ない。
;;;
;;;    **これは #215 で踏んだのと同じ族の経路である**(作者が通ると思っていない
;;;    場所から signal が飛ぶ)。わざと壊した身代わりではなく、
;;;    **実際に起こりうる並びをそのまま使っている**のがこの対照の値である。
;;;
;;;    数を渡して型の検査で落とす手は**使わない。** パスの正規化は
;;;    `%filecmd-absolute-path` に任せており、その下の `CHAR-INDEX` は
;;;    **型を見ずに番地として読む**(runtime.c:10275)。試験でヒープ外を
;;;    読ませても的に当たらない失敗になるだけである。
;;;
;;;    **探針そのものの陰性対照も置く**(term_test.lisp §6 と同じ): 入れてから
;;;    切ると t が返る —— つまりこの探針は 2 つの状態を見分けている。
;;;    見分けていなければ「いつも nil」で下の対照が無意味に通ってしまう。
;;; ---------------------------------------------------------------------------

;; 入る前に切れていること(ここまでの節は皮を通っていない)
(assert-equal nil (term-raw-input nil))

;; 探針が状態を見分けることの対照
(assert-equal nil (term-raw-input t))
(assert-equal t (term-raw-input nil))

;; 打鍵: 1 つ下げて(editor.lisp を選ぶ)改行。**ファイルなので開こうとする**
(defglobal *fd-open-keys*
  (string-append (create-string 1 *key-down*)
                 (string-append (create-string 1 #\newline) "qq")))

;; 【陽性対照】主ループの奥から signal が飛ぶ
(assert-error
  (with-standard-input (create-string-input-stream *fd-open-keys*)
    (with-standard-output (create-string-output-stream)
      (filer "/app"))))

;; **戻っている。** term-raw-input は「切り替える前の状態」を返すので、
;; nil が返れば「呼ぶ前は切れていた」= 皮がちゃんと戻した、ということである
(defglobal *fd-raw-after-escape* (term-raw-input nil))
(assert-equal nil *fd-raw-after-escape*)

;; 普通に終わった場合も戻る(脱出のときだけ戻る実装では通らない)。
;; 皮は (standard-input) / (standard-output) をそれぞれ 1 回だけ呼ぶので、
;; 文字列のストリームへ差し替えれば打鍵なしで動かせる
(defglobal *fd-shell*
  (with-standard-input (create-string-input-stream "q")
    (with-standard-output (create-string-output-stream)
      (filer "/app"))))
(defglobal *fd-raw-after-normal* (term-raw-input nil))

(assert-equal (list 'filer-quit 0 0 "/app") *fd-shell*)
(assert-equal nil *fd-raw-after-normal*)

(format *isiki-test-stream*
        "#filer raw-input: after-escape=~S after-normal=~S~%"
        *fd-raw-after-escape* *fd-raw-after-normal*)

;;; ---------------------------------------------------------------------------
;;; 8. 【完了条件】ファイルを選んで **pager-run** で開く(皮ごし)
;;;
;;;    **§7 と同じ打鍵である。** 違うのは apps/pager.lisp を読み込んだことだけ。
;;;    signal が成功に変わる —— つまり**開く道は `pager-run` の 1 本だけ**で、
;;;    §7 の脱出がそこから来ていたことも同時に示している。
;;;
;;;    ここは**皮ごし**に通している(核は test/lisp/filer_test.lisp §18)。
;;;    皮は実画面から行数と桁数を決めるので、**実機の寸法で一度も通していない
;;;    経路を残さない**ためである。
;;;
;;;    打鍵 4 つの行き先: 下(選択を editor.lisp へ)→ 改行(開く)→
;;;    q(**ページャ**が頁の合図として読んでやめる)→ q(ファイラがやめる)。
;;;    **合図のストリームが 1 本であることがここに出ている。**
;;;
;;;    出た文字は見ない。文字列出力ストリームの器は 1024 バイトで、
;;;    20 行ぶんは確実に超えて黙って切り捨てられる
;;;    (stream.h の STREAM_STRING_OUTPUT_CAP)。見るのは返り値である。
;;; ---------------------------------------------------------------------------

(defglobal *fd-load-pager* (load "/app/pager.lisp"))
(assert-equal t *fd-load-pager*)
(assert-equal 20 (dynamic *pager-page-lines*))

(defglobal *fd-opened*
  (with-standard-input (create-string-input-stream *fd-open-keys*)
    (with-standard-output (create-string-output-stream)
      (filer "/app"))))
(defglobal *fd-raw-after-open* (term-raw-input nil))

;; 動いた回数 1(下へ 1 つ)・**開いた本数 1**・場所は /app のまま
(assert-equal (list 'filer-quit 1 1 "/app") *fd-opened*)
(assert-equal nil *fd-raw-after-open*)

(format *isiki-test-stream*
        "#filer open: ~S (the same keys signalled before apps/pager.lisp was loaded)~%"
        *fd-opened*)

;;; ---------------------------------------------------------------------------
;;; 9. 皮が実画面に合わせて切り下げる
;;;
;;;    皮が使う式(`%filer-clamp` と上限 `(- (term-rows) 3)`)をここに写してある。
;;;    **式を変えたらここが落ちる。それが狙いである。**
;;;
;;;    行を 3 つ空ける理由: 見出し 1 + 一覧 n + 合図 1 で、**最下行は空けておく**
;;;    (合図の行の後ろの行送りで画面が巻き上がると、格子が 1 行ずれる)。
;;; ---------------------------------------------------------------------------

(assert-equal t (>= (- (term-rows) 3) 1))
(assert-equal t (<= (+ (%filer-clamp (dynamic *filer-list-lines*) (- (term-rows) 3)) 3)
                    (term-rows)))
;; 既定の 16 行はこの実画面に収まる(= 切り下げが働いていない)。
;; **収まらない画面では上の不等式だけが残る**ので、どちらでも嘘にならない
(assert-equal 16 (%filer-clamp (dynamic *filer-list-lines*) (- (term-rows) 3)))
(assert-equal 40 (%filer-clamp (dynamic *filer-row-cols*) (term-cols)))
