;; test/lisp/filer_test.lisp
;;
;; [#217] **ファイラの核(apps/filer.lisp の filer-run)を、キー入力なしで試験する。**
;;
;; 核は「場所・打鍵・表示先」を受け取る形にしてある。打鍵は入力ストリームなので、
;; **文字列入力ストリームを渡せばゲストへキーを送る仕組みが無くても試験できる。**
;; これは設計の要である: 調査(docs/investigation/app-terminal-io.logic.md §9-15)は
;; 「キー入力の仕組みは**全体が無試験**」と報告している。対話を直に書くと、
;; その族をもう 1 つ増やすことになる。
;;
;; **ここで見るのは文字の内容である。位置指定は見えない**(文字列出力ストリームに
;; 書くと位置は消える)。格子の検査は test/lisp/filer_app_disk_test.lisp にある。
;; **2 段構えのうちの CI 側がこのファイルである。**
;;
;; 置いた対照(#217 の完了条件に対応する):
;;   §1  既定値が実画面に収まる
;;   §2  数を文字列にする(桁上がり・0・負)
;;   §3  桁に収める
;;   §4  実画面に合わせる丸め
;;   §5  項目の読み方(種別と大きさ)。**陰性対照つき**
;;   §6  1 行の文字列(選択の記号・種別・大きさ)
;;   §7  窓の頭(巻き)。**陰性対照つき**
;;   §8  リストの i 番目と長さ
;;   §9  親の場所(根では無い)
;;   §10 一覧をデータで取る
;;   §11 1 画面ぶんの文字(【完了条件】一覧できる)
;;   §12 上下に選択を動かせる(矢印と j/k の両方)
;;   §13 端では断る
;;   §14 窓が巻く(一覧が窓より長いとき)
;;   §15 ディレクトリに入る
;;   §16 親へ戻る(キーと ".." の両方)
;;   §17 上が読めないときは断る
;;   §18 ファイルを選んで pager-run で開く
;;   §19 始める場所が読めないとき
;;   §20 知らないキーは何もしない / 相対パスの契約

;;; ---------------------------------------------------------------------------
;;; 0. 道具立て
;;;
;;;    **ホスト共有から読む。** 先頭に区切りが無い名前は従来どおりホスト共有へ行く
;;;    (test/lisp/load_disk_test.lisp の契約)。段では /app から読む側が先に走るので、
;;;    ここは「同じ中身・別の経路」で読み直していることになる。
;;;    このファイルを単独で読んでも成立するよう、自分で load する。
;;;
;;;    **ページャを先に読む**(ファイラは §18 で `pager-run` を呼ぶ)。
;;;    /app の割り当ては段の前段(filer_app_disk_test.lisp)が済ませているが、
;;;    **済んでいなければここで割り当てる** —— 二重に積まないのは、
;;;    マウント表は最長一致で引かれ、同じ場所を 2 回積むと解決が先着順に
;;;    依存するようになるからである。
;;; ---------------------------------------------------------------------------

(defglobal *fl-load-pager* (load "apps/pager.lisp"))
(assert-equal t *fl-load-pager*)
(defglobal *fl-load-filer* (load "apps/filer.lisp"))
(assert-equal t *fl-load-filer*)

(if (null (%filecmd-resolve-mount "/app"))
    (mount "/app" (%device-named "ide-primary-slave") ':fat32)
  nil)
(assert-equal t (if (%filecmd-resolve-mount "/app") t nil))

;; 行送り 1 文字の文字列。文字列リテラルに行送りを直に書く手段が無い
;; (エスケープはバックスラッシュと二重引用符だけ)
(defglobal *fl-nl* (create-string 1 #\newline))

;; 1 回ぶん動かして (結果 出力文字列) を返す。
;; **窓の大きさは引数で受けて dynamic-let で束縛する。** 動的変数に直に setq すると
;; 以後のすべての対照に漏れる(動的変数はプロセス間でも共有される。調査 §8-3)
(defun fl-run (lines path keys)
  (dynamic-let ((*filer-list-lines* lines))
    (let ((out (create-string-output-stream)))
      (let ((r (filer-run path (create-string-input-stream keys) out)))
        (list r (get-output-stream-string out))))))

;; 部分文字列の出現数。**「1 回だけ出る」を言えるようにするため**である
;; (「在る」だけでは、記号を全行に置く実装でも通ってしまう)
(defun fl-count (sub s)
  (let ((n 0) (i 0) (len (length s)) (sublen (length sub)) (going t))
    (while (and going (<= (+ i sublen) len))
      (let ((hit (string-index sub s i)))
        (if (null hit)
            (setq going nil)
          (progn (setq n (+ n 1)) (setq i (+ hit 1))))))
    n))

;; 在るか(nil / 非 nil を t / nil に正規化して assert-equal に渡すため)
(defun fl-has (sub s) (if (string-index sub s) t nil))

;; 行送りの数。**窓の高さが効いていることを行の本数で見る**
(defun fl-lines (s) (fl-count *fl-nl* s))

;; 【この道具自身の対照】数え方が合っていること。**数える道具が狂っていたら
;; 下の対照はすべて無意味になる**(規則 8)
(assert-equal 0 (fl-count "x" ""))
(assert-equal 0 (fl-count "x" "abc"))
(assert-equal 1 (fl-count "b" "abc"))
(assert-equal 3 (fl-count "> " "> a> b> c"))
(assert-equal 2 (fl-count "aa" "aaa"))
(assert-equal t (fl-has "bc" "abc"))
(assert-equal nil (fl-has "bd" "abc"))
(assert-equal 2 (fl-lines (string-append "a" (string-append *fl-nl* (string-append "b" *fl-nl*)))))

;;; ---------------------------------------------------------------------------
;;; 1. 既定値が実画面に収まる
;;;
;;;    **#215 / #214 が ★ のまま残したのと同じ問いである。** 口ができた(#216)ので、
;;;    読みではなく機械で見る。収まらないと一覧の頭が履歴へ流れて読めなくなる。
;;;
;;;    見出し 1 + 一覧 n + 合図 1 + **最下行を空ける 1** で rows に収まること。
;;;    最下行を空ける理由は apps/filer.lisp の皮のコメントにある
;;;    (合図の行の後ろの行送りで画面が巻き上がると格子が 1 行ずれる)。
;;; ---------------------------------------------------------------------------

(assert-equal 16 (dynamic *filer-list-lines*))
(assert-equal 40 (dynamic *filer-row-cols*))
(assert-equal t (<= (+ (dynamic *filer-list-lines*) 3) (term-rows)))
(assert-equal t (<= (dynamic *filer-row-cols*) (term-cols)))

;;; ---------------------------------------------------------------------------
;;; 2. 数を文字列にする
;;;
;;;    `format` を使わずに桁を割り出している(1 行ごと・1 描き直しごとに
;;;    ストリームを作らないため)。**桁上がりを明示的に見る。**
;;;    負は呼び出し元に無いが、素通しすると `while` が止まらないので印を返す。
;;; ---------------------------------------------------------------------------

(assert-equal "0" (%filer-number-text 0))
(assert-equal "1" (%filer-number-text 1))
(assert-equal "9" (%filer-number-text 9))
(assert-equal "10" (%filer-number-text 10))
(assert-equal "11" (%filer-number-text 11))
(assert-equal "18" (%filer-number-text 18))
(assert-equal "100" (%filer-number-text 100))
(assert-equal "1000" (%filer-number-text 1000))
(assert-equal "1024" (%filer-number-text 1024))
(assert-equal "99999" (%filer-number-text 99999))
(assert-equal "?" (%filer-number-text -1))

;;; ---------------------------------------------------------------------------
;;; 3. 桁に収める
;;;
;;;    画面の右端を越えさせない。越えると格子の外へ落ちるか次の行へ回り込むかで、
;;;    どちらにしても配置と画面が食い違う。
;;; ---------------------------------------------------------------------------

(assert-equal "abc" (%filer-truncate "abc" 5))
(assert-equal "abc" (%filer-truncate "abc" 3))
(assert-equal "ab" (%filer-truncate "abc" 2))
(assert-equal "" (%filer-truncate "abc" 0))

;;; ---------------------------------------------------------------------------
;;; 4. 実画面に合わせる丸め
;;;
;;;    **皮が使う計算である。** #215 の皮は同じ計算を式のまま埋め込んでいて、
;;;    丸めそのものを単体で試験できなかった。
;;;    「上限を超えない」「上限が 1 未満でも 1 を返す」
;;;    「**指定を黙って上げない**」の 3 つを見る。
;;; ---------------------------------------------------------------------------

(assert-equal 5 (%filer-clamp 5 47))
(assert-equal 47 (%filer-clamp 100 47))
(assert-equal 47 (%filer-clamp 47 47))
(assert-equal 1 (%filer-clamp 5 0))
(assert-equal 1 (%filer-clamp 5 -3))
;; **陰性対照: 常に上限を返す実装では上の 1 件目で落ちる**(5 を 47 にしない)
(assert-equal 1 (%filer-floor1 0))
(assert-equal 1 (%filer-floor1 -5))
(assert-equal 3 (%filer-floor1 3))

;;; ---------------------------------------------------------------------------
;;; 5. 項目の読み方(**種別と大きさ**)
;;;
;;;    項目は `("NAME" :file/:dir size)`(file-cmd.lisp:180-181)。
;;;    **キーワード型が無いのでクォートが要る** —— 素で `:dir` と書くと
;;;    「未定義の変数」になる(実機で踏んだ)。だから期待値も `(list ... ':dir ...)`
;;;    の形で書く。
;;; ---------------------------------------------------------------------------

(defglobal *fl-dir* (list "NOTES" ':dir 0))
(defglobal *fl-file* (list "NOTE.TXT" ':file 11))

(assert-equal "NOTES" (%filer-entry-name *fl-dir*))
(assert-equal "NOTE.TXT" (%filer-entry-name *fl-file*))
(assert-equal 0 (%filer-entry-size *fl-dir*))
(assert-equal 11 (%filer-entry-size *fl-file*))
(assert-equal t (%filer-dir-p *fl-dir*))
;; **陰性対照: すべてディレクトリと答える実装では通らない**
(assert-equal nil (%filer-dir-p *fl-file*))

;;; ---------------------------------------------------------------------------
;;; 6. 1 行の文字列(**選択の示し方**)
;;;
;;;    **記号が主の示し方である**(反転表示が無く、カーソルの位置は Lisp から
;;;    読めない。apps/filer.lisp の頭の説明を参照)。
;;;    選んでいない行は**同じ幅の空白**で始める —— 幅を変えると名前の桁が
;;;    行ごとにずれる。
;;; ---------------------------------------------------------------------------

(assert-equal "> NOTES  <DIR>" (%filer-row-text *fl-dir* t))
(assert-equal "  NOTES  <DIR>" (%filer-row-text *fl-dir* nil))
(assert-equal "> NOTE.TXT  11" (%filer-row-text *fl-file* t))
(assert-equal "  NOTE.TXT  11" (%filer-row-text *fl-file* nil))
;; 大きさ 0 のファイルは "0" と出る(ディレクトリの "<DIR>" と混ざらない)
(assert-equal "  HELLO.TXT  0" (%filer-row-text (list "HELLO.TXT" ':file 0) nil))
;; 合図の文面。**apps/filer.lisp の文面をここに写してある**(変えたらここが落ちる)
(assert-equal "up/down enter u=up q=quit" (%filer-status-text))

;;; ---------------------------------------------------------------------------
;;; 7. 窓の頭(**巻き**)
;;;
;;;    選択が窓から出たら、出た向きへ 1 行ぶんだけ動かす。
;;;    **純粋な計算なので、一覧も画面も要らずに全部の場合を見られる。**
;;;
;;;    陰性対照: **常に 0 を返す実装**(= 巻かない)は 3 件目で落ち、
;;;    **常に sel を返す実装**(= 選択を常に先頭へ置く)は 1 件目で落ちる。
;;; ---------------------------------------------------------------------------

(assert-equal 0 (%filer-window-top 0 0 3))
(assert-equal 0 (%filer-window-top 2 0 3))
(assert-equal 1 (%filer-window-top 3 0 3))
(assert-equal 3 (%filer-window-top 5 0 3))
(assert-equal 2 (%filer-window-top 2 3 3))
(assert-equal 3 (%filer-window-top 4 3 3))
(assert-equal 7 (%filer-window-top 7 0 1))
;; 窓が 0 行以下でも止まらない(核が床を張るので通常は来ないが、素通しさせない)
(assert-equal 4 (%filer-window-top 4 0 0))

;;; ---------------------------------------------------------------------------
;;; 8. リストの i 番目と長さ
;;;
;;;    **無ければ nil を返す**ので、呼び出し側が長さを数え直さずに済む。
;;; ---------------------------------------------------------------------------

(defglobal *fl-three* (list *fl-dir* *fl-file* (list "X" ':file 1)))

(assert-equal 3 (%filer-length *fl-three*))
(assert-equal 0 (%filer-length nil))
(assert-equal *fl-dir* (%filer-nth *fl-three* 0))
(assert-equal *fl-file* (%filer-nth *fl-three* 1))
(assert-equal nil (%filer-nth *fl-three* 3))
(assert-equal nil (%filer-nth nil 0))

;;; ---------------------------------------------------------------------------
;;; 9. 親の場所
;;;
;;;    **「上が無い」の判定をここ 1 箇所に閉じ込めてある。**
;;;    核の `if` に埋めると、根で断ることを確かめるには「根が割り当てられている」
;;;    状態を作らねばならない —— マウント表の根は 9P の解決と同じ場所を使うので、
;;;    試験のために割り当てるわけにはいかない(ホスト共有へ書けなくなる)。
;;;
;;;    ★ **「(at the top)」の 1 行そのものは実機で出していない。**
;;;    出す条件(= 親が nil)はここで固定してあるが、根が割り当てられた状態を
;;;    試験で作れないため、文面の出力までは覆っていない。
;;; ---------------------------------------------------------------------------

(assert-equal nil (%filer-parent "/"))
(assert-equal "/" (%filer-parent "/app"))
(assert-equal "/app" (%filer-parent "/app/NOTES"))
(assert-equal "/app/NOTES" (%filer-parent "/app/NOTES/DEEP"))

;;; ---------------------------------------------------------------------------
;;; 10. 一覧をデータで取る(**`ls` を使わない**)
;;;
;;;     `ls` は項目を返すが**同時に表示もする**(file-cmd.lisp:212-231)ので
;;;     使えない。マウント表で解決して read-dir へ振り分ける 2 つを直に呼ぶ。
;;; ---------------------------------------------------------------------------

(defglobal *fl-entries* (%filer-list "/app"))

(assert-equal 6 (%filer-length *fl-entries*))
(assert-equal "NOTES" (%filer-entry-name (%filer-nth *fl-entries* 0)))
(assert-equal t (%filer-dir-p (%filer-nth *fl-entries* 0)))
(assert-equal "filer.lisp" (%filer-entry-name (%filer-nth *fl-entries* 1)))
(assert-equal nil (%filer-dir-p (%filer-nth *fl-entries* 1)))
;; 割り当てが無い場所は nil(= 入れない)
(assert-equal nil (%filer-list "/no-such-mount"))
(assert-equal nil (%filer-list "/app/no-such-dir"))

;;; ---------------------------------------------------------------------------
;;; 11. 【完了条件】ディレクトリの中身を一覧できる(**種別と大きさを出す**)
;;;
;;;     打鍵を 1 つも渡さない。1 画面描いて、尽きて終わる。
;;;
;;;     **陰性対照がここに 3 つ入っている:**
;;;       - 記号を全行に置く実装 … `"> "` が 1 個でないので落ちる
;;;       - 一覧を出さない実装 … 行の本数(見出し 1 + 6 件 + 合図 1 = 8)で落ちる
;;;       - 種別を出さない実装 … `"<DIR>"` で落ちる
;;; ---------------------------------------------------------------------------

(defglobal *fl-c1* (fl-run 16 "/app" ""))

(assert-equal (list 'filer-eof 0 0 "/app") (car *fl-c1*))
(assert-equal 0 (string-index "/app  6 items" (car (cdr *fl-c1*))))
(assert-equal 1 (fl-count "> " (car (cdr *fl-c1*))))
(assert-equal t (fl-has "> NOTES  <DIR>" (car (cdr *fl-c1*))))
(assert-equal t (fl-has "  filer.lisp  " (car (cdr *fl-c1*))))
(assert-equal t (fl-has "up/down enter u=up q=quit" (car (cdr *fl-c1*))))
(assert-equal 8 (fl-lines (car (cdr *fl-c1*))))

;;; ---------------------------------------------------------------------------
;;; 12. 【完了条件】上下に選択を動かせる
;;;
;;;     矢印(#216 の `*key-down*` / `*key-up*`)と j/k の両方で動く。
;;;     **2 画面ぶんの文字で見る**: 1 画面目は NOTES に記号、2 画面目は
;;;     filer.lisp に記号 —— つまり**記号が動いている。**
;;;
;;;     矢印が文字として届くことは test/c/keymap_test.c と
;;;     test/lisp/term_test.lisp §5 が見ている。
;;; ---------------------------------------------------------------------------

(defglobal *fl-c2* (fl-run 16 "/app" (string-append (create-string 1 *key-down*) "q")))

(assert-equal (list 'filer-quit 1 0 "/app") (car *fl-c2*))
(assert-equal 2 (fl-count "> " (car (cdr *fl-c2*))))
(assert-equal t (fl-has "> NOTES  <DIR>" (car (cdr *fl-c2*))))
(assert-equal t (fl-has "> filer.lisp  " (car (cdr *fl-c2*))))
(assert-equal t (fl-has "  NOTES  <DIR>" (car (cdr *fl-c2*))))

;; j/k も同じように効く(2 つ下げて 1 つ戻す = 選択は 1 つ下)
(defglobal *fl-c3* (fl-run 16 "/app" "jjkq"))

(assert-equal (list 'filer-quit 3 0 "/app") (car *fl-c3*))
(assert-equal t (fl-has "> filer.lisp  " (car (cdr *fl-c3*))))
;; 上向きの矢印も同じ(j で下げてから矢印で戻す)
(defglobal *fl-c4*
  (fl-run 16 "/app" (string-append "j" (string-append (create-string 1 *key-up*) "q"))))
(assert-equal (list 'filer-quit 2 0 "/app") (car *fl-c4*))
;; 3 画面出て、1 画面目と 3 画面目が NOTES を選んでいる(2 画面目は filer.lisp)
(assert-equal 2 (fl-count "> NOTES  <DIR>" (car (cdr *fl-c4*))))
(assert-equal 1 (fl-count "> filer.lisp  " (car (cdr *fl-c4*))))

;;; ---------------------------------------------------------------------------
;;; 13. 端では断る(**動いた回数に数えない**)
;;;
;;;     先頭で上を押しても、末尾で下を押しても動かない。
;;;     **断ったことを「動いた回数」で言えるようにしてある** ——
;;;     数えてしまうと「動いたのに動いていない」状態を試験が見逃す。
;;; ---------------------------------------------------------------------------

(defglobal *fl-c5* (fl-run 16 "/app" "kq"))
(assert-equal (list 'filer-quit 0 0 "/app") (car *fl-c5*))
(assert-equal 2 (fl-count "> NOTES  <DIR>" (car (cdr *fl-c5*))))

;; End で末尾へ飛び、そこで下を押しても動かない(End は 1 回数える)
(defglobal *fl-c6*
  (fl-run 16 "/app" (string-append (create-string 1 *key-end*) "jq")))
(assert-equal (list 'filer-quit 1 0 "/app") (car *fl-c6*))
(assert-equal t (fl-has "> pager.lisp  " (car (cdr *fl-c6*))))

;; Home で先頭へ戻る
(defglobal *fl-c7*
  (fl-run 16 "/app" (string-append (create-string 1 *key-end*)
                                   (string-append (create-string 1 *key-home*) "q"))))
(assert-equal (list 'filer-quit 2 0 "/app") (car *fl-c7*))

;;; ---------------------------------------------------------------------------
;;; 14. 窓が巻く(一覧が窓より長いとき)
;;;
;;;     窓を 3 行にして末尾へ飛ぶ。**2 画面目には先頭の NOTES が出ていない** ——
;;;     つまり窓が動いている。行の本数(見出し 1 + 3 件 + 合図 1 = 5)× 2 画面で、
;;;     **窓の高さが効いていること**も同時に見る。
;;;
;;;     陰性対照: 巻かない実装は「NOTES が 2 回出る」ので落ちる。
;;;     窓の高さを無視する実装は行の本数で落ちる。
;;; ---------------------------------------------------------------------------

(defglobal *fl-c8* (fl-run 3 "/app" (string-append (create-string 1 *key-end*) "q")))

(assert-equal (list 'filer-quit 1 0 "/app") (car *fl-c8*))
(assert-equal 10 (fl-lines (car (cdr *fl-c8*))))
(assert-equal 1 (fl-count "NOTES" (car (cdr *fl-c8*))))
(assert-equal t (fl-has "> pager.lisp  " (car (cdr *fl-c8*))))
(assert-equal t (fl-has "  inspect.lisp  " (car (cdr *fl-c8*))))

;;; ---------------------------------------------------------------------------
;;; 15. 【完了条件】ディレクトリに入る
;;;
;;;     先頭(NOTES)で改行を打つと中へ入る。**見出しがそのディレクトリになり、
;;;     中身("." / ".." / NOTE.TXT)が出る。**
;;;     NOTE.TXT の 11 は Makefile が焼いた "alpha\nbeta\n" の長さである。
;;; ---------------------------------------------------------------------------

(defglobal *fl-c9* (fl-run 16 "/app" (string-append *fl-nl* "q")))

(assert-equal (list 'filer-quit 1 0 "/app/NOTES") (car *fl-c9*))
(assert-equal t (fl-has "/app/NOTES  3 items" (car (cdr *fl-c9*))))
(assert-equal t (fl-has "> .  <DIR>" (car (cdr *fl-c9*))))
(assert-equal t (fl-has "  ..  <DIR>" (car (cdr *fl-c9*))))
(assert-equal t (fl-has "  NOTE.TXT  11" (car (cdr *fl-c9*))))

;; 右向きの矢印でも入れる
(defglobal *fl-c10*
  (fl-run 16 "/app" (string-append (create-string 1 *key-right*) "q")))
(assert-equal (list 'filer-quit 1 0 "/app/NOTES") (car *fl-c10*))

;;; ---------------------------------------------------------------------------
;;; 16. 【完了条件】親へ戻る
;;;
;;;     u でも、一覧の ".." を選んでも戻れる。**".." は素で繋がない** ——
;;;     `%filecmd-absolute-path` に任せているので "/app/NOTES/.." のような
;;;     場所を抱えたまま進まない(抱えると見出しが伸び続ける)。
;;; ---------------------------------------------------------------------------

(defglobal *fl-c11* (fl-run 16 "/app/NOTES" "uq"))

(assert-equal (list 'filer-quit 1 0 "/app") (car *fl-c11*))
(assert-equal t (fl-has "/app  6 items" (car (cdr *fl-c11*))))

;; 左向きの矢印も同じ
(defglobal *fl-c12*
  (fl-run 16 "/app/NOTES" (string-append (create-string 1 *key-left*) "q")))
(assert-equal (list 'filer-quit 1 0 "/app") (car *fl-c12*))

;; ".." を選んで入る(先頭の "." の次なので 1 つ下げてから改行)
(defglobal *fl-c13* (fl-run 16 "/app/NOTES" (string-append "j" (string-append *fl-nl* "q"))))
(assert-equal (list 'filer-quit 2 0 "/app") (car *fl-c13*))
(assert-equal t (fl-has "/app  6 items" (car (cdr *fl-c13*))))

;; "." を選んで入ると同じ場所に留まる(見出しが伸びない)
(defglobal *fl-c14* (fl-run 16 "/app/NOTES" (string-append *fl-nl* "q")))
(assert-equal (list 'filer-quit 1 0 "/app/NOTES") (car *fl-c14*))
(assert-equal nil (fl-has "/app/NOTES/." (car (cdr *fl-c14*))))

;;; ---------------------------------------------------------------------------
;;; 17. 上が読めないときは断る(**場所を変えない**)
;;;
;;;     /app の親は "/" で、そこには何も割り当てられていない。
;;;     **断って留まる**(入ってしまうと「空の画面から戻れない」ほうの
;;;     間違いになる)。動いた回数も増えない。
;;; ---------------------------------------------------------------------------

(defglobal *fl-c15* (fl-run 16 "/app" "uq"))

(assert-equal (list 'filer-quit 0 0 "/app") (car *fl-c15*))
(assert-equal t (fl-has "/: cannot read" (car (cdr *fl-c15*))))
(assert-equal t (fl-has "/app  6 items" (car (cdr *fl-c15*))))

;;; ---------------------------------------------------------------------------
;;; 18. 【完了条件】ファイルを選んで **pager-run** で開く
;;;
;;;     NOTES の末尾(NOTE.TXT)で改行を打つ。**ページャの核が同じストリームへ
;;;     本文を流す。**
;;;
;;;     `pager`(皮)ではなく核を呼んでいることが、ここで見えている:
;;;     皮を呼んでいたら本文は `(standard-output)`(= 実画面)へ行き、
;;;     **この文字列ストリームには出ない。**
;;;
;;;     2 行しかないので "--more--" は出ない(ページャは終端を先読みして
;;;     合図を待たない。#215 の対照 2/5 と同じ性質)。
;;;     開いた本数が 1 になることも見る。
;;; ---------------------------------------------------------------------------

(defglobal *fl-c16*
  (fl-run 16 "/app/NOTES" (string-append (create-string 1 *key-end*)
                                         (string-append *fl-nl* "q"))))

(assert-equal (list 'filer-quit 1 1 "/app/NOTES") (car *fl-c16*))
(assert-equal t (fl-has "alpha" (car (cdr *fl-c16*))))
(assert-equal t (fl-has "beta" (car (cdr *fl-c16*))))
;; **合図を待っていない**(2 行は 1 面に収まる)
(assert-equal nil (fl-has "--more--" (car (cdr *fl-c16*))))
;; 開いたあとは一覧に戻っている
(assert-equal t (fl-has "/app/NOTES  3 items" (car (cdr *fl-c16*))))

;;; ---------------------------------------------------------------------------
;;; 19. 始める場所が読めないとき
;;;
;;;     **1 画面も描かずに言って終わる**(`ls` の "no such file or directory" と
;;;     同じ流儀)。行は 1 本だけ = 一覧も合図も出していない。
;;;     signal にしないのは、綴り間違いで対話の輪を落とさないためである。
;;; ---------------------------------------------------------------------------

(defglobal *fl-c17* (fl-run 16 "/no-such-mount/x" "q"))

(assert-equal (list 'filer-no-dir 0 0 "/no-such-mount/x") (car *fl-c17*))
(assert-equal "/no-such-mount/x: no such directory" (subseq (car (cdr *fl-c17*)) 0 35))
(assert-equal 1 (fl-lines (car (cdr *fl-c17*))))
(assert-equal nil (fl-has "up/down" (car (cdr *fl-c17*))))

;; ★ **文字列でないパスは守っていない。** 正規化は `%filecmd-absolute-path` に
;; 任せており、その下の `CHAR-INDEX`(runtime.c:10275)は**型を見ずに番地として
;; 読む**。つまり数を渡すとヒープ外を読む —— #214 が `%%` の窓口について
;; 書いたのと同じ族である。**だからここでは渡さない**(試験で踏むと的に当たらない
;; 失敗になる)。守りを足すなら file-cmd.lisp 側に置くのが筋で、
;; それはこのアプリの仕事ではない。

;;; ---------------------------------------------------------------------------
;;; 20. 知らないキーは何もしない / 相対パスの契約
;;;
;;;     知らないキーで**描き直すだけ**にしてある(何もしないと、画面が崩れた
;;;     ときに直す手が無くなる)。3 画面出て、場所も選択も動かない。
;;;
;;;     パスの契約は `ls` / `cd` と同じ: **先頭が "/" でなければ `*cwd*` からの
;;;     相対**である(`%filecmd-absolute-path`)。
;;; ---------------------------------------------------------------------------

(defglobal *fl-c18* (fl-run 16 "/app" "zxq"))

(assert-equal (list 'filer-quit 0 0 "/app") (car *fl-c18*))
(assert-equal 3 (fl-count "> NOTES  <DIR>" (car (cdr *fl-c18*))))

(assert-equal "/" (dynamic *cwd*))
(defglobal *fl-c19* (fl-run 16 "app" "q"))
(assert-equal (list 'filer-quit 0 0 "/app") (car *fl-c19*))
