;;;; apps/filer.lisp
;;;;
;;;; **ディレクトリを見て移動し、ファイルを開く道具**(issue #217)。
;;;; 調査は docs/investigation/app-terminal-io.{logic,refs}.md。
;;;;
;;;; ## 組み込みは 0 個
;;;;
;;;; 要る口はすべて既存である。**C を 1 行も触っていない。**
;;;;
;;;;   - 一覧 … `%filecmd-resolve-mount` / `%filecmd-read-dir`(src/lisp/file-cmd.lisp)
;;;;   - 画面 … `term-*`(src/lisp/init.lisp。#216 で入った)
;;;;   - ファイルを開く … `pager-run`(apps/pager.lisp。#215)
;;;;
;;;; ## 範囲: **削除と改名は含めない**(ユーザー判断 2026-11-11)
;;;;
;;;; **閲覧・移動・ファイルを開くことに絞っている。** 削除と改名は FAT 側の実装が
;;;; 要るうえ、#199(上書き保存の壊れ)と同じ層を触る。だから別の仕事にした。
;;;; **この方針のおかげで、このアプリは円盤へ 1 バイトも書かない**(読みだけ)。
;;;;
;;;; ## `ls` を使わない理由
;;;;
;;;; `ls`(file-cmd.lisp:212-231)は項目のリストを返すが、**同時に
;;;; `%device-output-stream` へ表示もする**。ファイラは自分で位置を決めて描くので、
;;;; 勝手に表示されると画面が崩れる。だから `ls` の下にある 2 つ
;;;; (`%filecmd-resolve-mount` / `%filecmd-read-dir`)を直に使う。
;;;;
;;;; 項目の形は `("NAME" :file/:dir size)`(file-cmd.lisp:180-181)で、
;;;; **種別と大きさがそのまま入っている。**
;;;;
;;;; ## 置き場所
;;;;
;;;; **アプリ専用の FAT32 ディスク(Makefile の APP_DISK_IMG)の根。**
;;;; apps/pager.lisp / apps/inspect.lisp / apps/hello.lisp と同じ約束で、
;;;; ホスト側の原本がこの apps/ にあり、ビルドがイメージへ焼き込む。
;;;; ゲストからは
;;;;   (mount "/app" (%device-named "ide-primary-slave") ':fat32)
;;;;   (load "/app/pager.lisp")   ; ファイルを開くのに使う
;;;;   (load "/app/filer.lisp")
;;;;
;;;; **常駐費用はゼロである。** apps/*.lisp は
;;;; `python3 tools/gen_embedded_lisp.py --print-inputs`(同梱の一覧)にも
;;;; Makefile の `TRANSPILE_LISP_SRC`(AOT の一覧)にも**入っていない**。
;;;; したがって起動イメージも起動時間も不動領域も 1 バイトも増えない
;;;; (規則 13: この 2 つのコマンドで導出できる)。
;;;;
;;;; **9P の無い起動でも読まれうる**ので、ここで使えるのはカーネルへ同梱した層
;;;; (src/lisp/init.lisp)と AOT 済みの層(file-cmd.lisp は M15 で AOT 済み)、
;;;; それに同じ円盤に置いた apps/pager.lisp だけである。
;;;;
;;;; ## 守っていること
;;;;
;;;; 1. **キーワードは必ずクォートする。** この処理系にキーワード型は無いので
;;;;    `:dir` と書くと「未定義の変数」になる。`':dir` と書く
;;;;    (下の `%filer-dir-p` が唯一の比較箇所である)
;;;; 2. **`(format t ...)` を使わない。** 第 1 引数は MAGIC_STREAM を要求するので
;;;;    `t` では通らない(format.c:164-168)。出力先は常に引数で受け取った
;;;;    ストリームで、薄い皮だけが `(standard-output)` を呼ぶ
;;;; 3. **`~nA` で桁を揃えない。** num が無視される(format.c:204-215)。
;;;;    行は `STRING-APPEND` で組み立てて、**位置を指定して書く**
;;;; 4. **読み取り口は 1 つ持ち回る。** `(standard-input)` は皮の中で 1 回だけ呼ぶ。
;;;;    呼び直すと毎回約 1.6KB 確保するうえ、先読みの 1 文字が捨てられる
;;;; 5. **主ループは関数の中に書く。** トップレベルの繰り返しは 1 反復ごとに
;;;;    再帰的に評価されてスタックを壊す(調査 §8-10)
;;;; 6. **素の入力は `unwind-protect` で戻す**(下の `filer` 参照。#215 で実際に
;;;;    踏んだバグ)
;;;;
;;;; ## 選択の示し方: **記号 "> " が主、カーソルは副**
;;;;
;;;; 反転表示が無い(色も属性も無い。#216 の調査)。選べるのは
;;;; **記号を置く**か**カーソルをそこに置く**かの 2 つで、**両方やる**ことにした。
;;;; ただし主従をはっきり決めてある ——
;;;;
;;;;   **記号が主である。理由: カーソルの位置は Lisp から読めない。**
;;;;   端末の口は 5 つ(`%%TERM-SIZE` / `%%TERM-SET-CURSOR` / `%%TERM-CLEAR` /
;;;;   `%%TERM-READ-CELL` / `%%TERM-RAW-INPUT`)で、**位置を読む口が無い。**
;;;;   つまりカーソルだけで示すと「選択がどこを指しているか」を機械で
;;;;   確かめる方法が無くなる —— 試験が人の目になる。
;;;;   記号なら `term-read-cell` で格子から読み返せるし、文字列出力ストリームに
;;;;   流したときも文字として出る。**2 段構えの試験が両方とも成り立つ。**
;;;;
;;;;   カーソルは**副**として選択行へ置く。実機の利用者には分かりやすく、
;;;;   置くのに 1 行しか要らず、**記号が主なので試験は失わない。**
;;;;
;;;; ## 最下行を使わない
;;;;
;;;; 1 行書くたびに行送りを 1 つ出している(文字列出力ストリームへ流したときに
;;;; 行として読めるようにするため。位置指定は文字列には効かない)。
;;;; **最下行で行送りを出すと画面が 1 行巻き上がり、格子が静かにずれる。**
;;;; だから皮は「見出し 1 + 一覧 n + 合図 1 + 空けておく 1」が実画面に収まる
;;;; ところまで n を切り下げる(下の `filer`)。
;;;;
;;;; ## 核と皮を分けた理由(#215 / #214 と同じ)
;;;;
;;;; 核 `filer-run` は**場所・合図・表示先**を受け取る。合図は入力ストリームなので、
;;;; **文字列入力ストリームを渡せばキー入力なしに試験できる。**
;;;; 試験は test/lisp/filer_test.lisp(文字の内容)と
;;;; test/lisp/filer_app_disk_test.lisp(実機の格子を `term-read-cell` で見る)。
;;;; **位置指定が効いていることは後者でしか機械で確かめられない。**

;;; ---------------------------------------------------------------------------
;;; 一覧の大きさ(行数と桁数)
;;;
;;; **画面から読まない。** 核だけを文字列ストリームで試験するときに実画面へ
;;; 依らせないためである(#215 の `*pager-page-lines*` と同じ作法)。
;;; 実機では皮(下の `filer`)が `term-rows` / `term-cols` から切り下げる。
;;;
;;; 既定の 16 行は控えめにしてある。**実画面より大きいと行き過ぎる。**
;;; 16 + 見出し 1 + 合図 1 = 18 行で、過去の記録にある 50 行にも収まる。
;;; 既定の桁 40 は、名前 33 文字(FAT の長い名前の実例)+ 記号 2 + 大きさで
;;; 足りる見積もりである。収まることは test/lisp/filer_test.lisp が機械で見る。
;;;
;;; **初期宣言は安全な形で書く**(tools/check_defdynamic_safe.py と同じ作法。#187)。
;;; defdynamic は value-form を評価して**無条件に上書きする**ので、素の初期値を
;;; 書くと「2 回目の load」が利用者の設定を無言で消す。
;;; この `or` の形は **「この変数は nil を意味のある値として持たない」**ことに
;;; 依存している(行数・桁数に nil は無い)。前提が崩れたら書き換えが要る。
(defdynamic *filer-list-lines* (or (dynamic *filer-list-lines*) 16))
(defdynamic *filer-row-cols* (or (dynamic *filer-row-cols*) 40))

;;; ---------------------------------------------------------------------------
;;; 項目の読み方
;;;
;;; 項目は `("NAME" :file/:dir size)`(file-cmd.lisp:180-181)。
;;; **ここだけが形を知っている。** 形が変わったら直す場所は 4 つで済む。
(defun %filer-entry-name (e) (car e))
(defun %filer-entry-kind (e) (car (cdr e)))
(defun %filer-entry-size (e) (car (cdr (cdr e))))

;;; **`':dir` をクォートする唯一の箇所。** キーワード型が無いので `:dir` と
;;; 書くと未定義の変数になる(実機で踏んだ)。比較は `eq` でよい(記号どうし)。
(defun %filer-dir-p (e) (eq (%filer-entry-kind e) ':dir))

;;; ---------------------------------------------------------------------------
;;; 数を文字列にする
;;;
;;; **`format` を使わない。** 位置を指定して書くには文字列が要るのに、
;;; 文字列へ書くには毎回ストリームを作る(1 回あたり約 1KB の器)ことになる。
;;; 1 行ごと・1 描き直しごとに作る量ではないので、桁を自分で割り出す。
;;;
;;; 負は呼び出し元に無い(大きさは非負)。**それでも素通しせず印を返す** ——
;;; 返さないと `while` が回り続ける。
(defun %filer-number-text (n)
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
;;; 桁に収める
;;;
;;; **画面の右端を越えさせない。** 越えると格子の外へ落ちるか次の行へ回り込むかで、
;;; どちらにしても**こちらが決めた配置と画面が食い違う。**
;;; 切り詰めるほうを選んだのは、崩れたまま続けるより読めるからである。
(defun %filer-truncate (s w)
  (if (> (length s) w) (subseq s 0 w) s))

;;; ---------------------------------------------------------------------------
;;; 丸め(皮が実画面に合わせるときに使う)
;;;
;;; **関数にしてあるのは試験できるようにするため**である。#215 の皮は同じ計算を
;;; 式のまま埋め込んでいて、丸めそのものを単体で試験できなかった。
;;; 「上限を超えない」「上限が 1 未満でも 1 を返す」「指定を黙って上げない」
;;; の 3 つを test/lisp/filer_test.lisp が見る。
(defun %filer-clamp (want cap)
  (if (< cap 1) 1 (if (< want cap) want cap)))

;;; 1 を下回らせない。**核が自前で床を張るため**である(核は画面を読まないので
;;; 上限を知らないが、0 行や負の桁を渡されたら描くものが無くなる)。
(defun %filer-floor1 (v) (if (< v 1) 1 v))

;;; ---------------------------------------------------------------------------
;;; リストの i 番目
;;;
;;; **`elt` に頼らない。** 項目のリストは cons で、`length`/`elt` は終端に出会う
;;; まで止まらない(runtime.c:11239-11245)。ここで要るのは「無ければ nil」で、
;;; それを自分で持っていたほうが呼び出し側が短くなる。
;;; **繰り返しで書く**(再帰にすると深い一覧でスタックを使う)。
(defun %filer-nth (lst i)
  (let ((cur lst) (k i))
    (while (and cur (> k 0))
      (setq cur (cdr cur))
      (setq k (- k 1)))
    (if (null cur) nil (car cur))))

;;; 項目の数。**繰り返しで数える**(理由は上と同じ)。
(defun %filer-length (lst)
  (let ((cur lst) (n 0))
    (while cur
      (setq cur (cdr cur))
      (setq n (+ n 1)))
    n))

;;; ---------------------------------------------------------------------------
;;; 1 行の文字列
;;;
;;; `("NOTES" :dir 0)` → `"> NOTES  <DIR>"` / `("TEST.LSP" :file 18)` → `"  TEST.LSP  18"`
;;;
;;; **種別と大きさを出すのは完了条件である。** ディレクトリに大きさを出さないのは
;;; FAT の項目が常に 0 を持つからで(fat32_test.lisp の `':dir 0` 参照)、
;;; 0 と書くと「空のディレクトリ」と読めてしまう。
(defun %filer-row-text (e selected)
  (string-append (if selected "> " "  ")
                 (%filer-entry-name e)
                 "  "
                 (if (%filer-dir-p e)
                     "<DIR>"
                   (%filer-number-text (%filer-entry-size e)))))

;;; 合図の行。**文面をここ 1 箇所に置く**(試験が写して突き合わせる)。
(defun %filer-status-text () "up/down enter u=up q=quit")

;;; ---------------------------------------------------------------------------
;;; 窓の頭
;;;
;;; 選択が窓から出たら、出た向きへ 1 行ぶんだけ動かす。
;;; **純粋な計算なので単体で試験できる**(一覧の中身も画面も要らない)。
(defun %filer-window-top (sel top n)
  (cond ((< n 1) sel)
        ((< sel top) sel)
        ((>= sel (+ top n)) (+ (- sel n) 1))
        (t top)))

;;; ---------------------------------------------------------------------------
;;; 親の場所
;;;
;;; 無ければ nil。**「上が無い」の判定をここ 1 箇所に閉じ込めてある。**
;;; `%filecmd-absolute-path` は根で ".." を無視する(file-cmd.lisp の
;;; `%filecmd-normalize-components-iter`)ので、**同じ場所が返ったら上は無い。**
;;;
;;; 関数に切り出したのは**試験できるようにするため**である。核の中の `if` に
;;; 埋めると、根で断ることを確かめるには「根が割り当てられている」状態を
;;; 作らなければならない —— マウント表の根は 9P の解決と同じ場所を使うので、
;;; 試験のために割り当てるわけにはいかない(ホスト共有へ書けなくなる)。
(defun %filer-parent (path)
  (let ((up (%filecmd-absolute-path path "..")))
    (if (string= up path) nil up)))

;;; ---------------------------------------------------------------------------
;;; 一覧をデータで取る
;;;
;;; **`ls` を使わない**(表示もしてしまう。ファイル頭のコメント参照)。
;;; マウント表で解決して read-dir へ振り分ける 2 つを直に呼ぶ。
;;;
;;; nil が返るのは「マウントが無い」「ディレクトリが無い」「中身が 0 件」の 3 つで、
;;; **FAT の read-dir はこの 3 つを区別できない**(マウントの根は "." / ".." を
;;; 持たないため。file-cmd.lisp の `ls` のコメントにある既知の制約)。
;;; だから呼び出し側は nil を「入れない」として扱う —— 入れてしまうと
;;; 「空の画面から戻れない」ほうの間違いになる。
;;; FAT のサブディレクトリは必ず "." / ".." を持つので、そこは取りこぼさない。
(defun %filer-list (path)
  (let ((resolved (%filecmd-resolve-mount path)))
    (if (null resolved)
        nil
      (%filecmd-read-dir (car resolved)
                         (car (cdr resolved))
                         (car (cdr (cdr resolved)))))))

;;; ---------------------------------------------------------------------------
;;; 1 画面ぶん描く
;;;
;;;   見出し    行 0        … 場所と項目数
;;;   一覧      行 1..shown … 窓の中の項目(選択行の頭に "> ")
;;;   合図      行 shown+1  … 打てるキー
;;;
;;; 返すのは **描いた一覧の行数**(shown)で、皮が終わりにカーソルを置く先を
;;; 決めるのに使う。
;;;
;;; **毎回まず消す。** 消さないと、短い一覧へ移ったときに前の一覧の残りが
;;; 下に残る(格子にあるものは勝手に消えない)。
;;; 文字列出力ストリームへ流す試験では `term-clear` は**文字列に何も足さない**
;;; (消すのは格子と物理画面だけ)ので、試験の期待値は変わらない。
;;;
;;; **1 行ごとに行送りを 1 つ出す。** 位置指定は文字列出力ストリームには効かない
;;; ので、行送りが無いと全部が 1 行に繋がって読めなくなる。
;;; 実画面では位置指定のほうが効くので、行送りは「次の行の頭へ進む」だけで害は無い
;;; —— **最下行でだけ画面を巻き上げるので、皮が最下行を空けておく。**
(defun %filer-draw (path entries sel top n w out)
  (term-clear)
  (let ((count (%filer-length entries)) (i 0) (shown 0))
    (term-write-at 0 0
                   (%filer-truncate
                     (string-append path "  "
                                    (%filer-number-text count) " items") w)
                   out)
    (format out "~%")
    (if (null entries)
        (progn
          ;; **空でも 1 行は出す。** 何も出さないと「描いたのに空だった」と
          ;; 「描いていない」が画面で区別できなくなる
          (term-write-at 1 0 "(no entries)" out)
          (format out "~%")
          (setq shown 1))
      (progn
        (while (and (< i n) (< (+ top i) count))
          (term-write-at (+ i 1) 0
                         (%filer-truncate
                           (%filer-row-text (%filer-nth entries (+ top i))
                                            (= (+ top i) sel))
                           w)
                         out)
          (format out "~%")
          (setq i (+ i 1)))
        (setq shown i)))
    (term-write-at (+ shown 1) 0 (%filer-truncate (%filer-status-text) w) out)
    (format out "~%")
    ;; **副の示し方: 選択行へカーソルを置く。** 主は記号 "> " である
    ;; (ファイル頭の「選択の示し方」参照。カーソルの位置は読めないので
    ;; これだけでは機械で確かめられない)
    (if (null entries)
        (term-set-cursor 1 0)
      (term-set-cursor (+ (- sel top) 1) 0))
    shown))

;;; ---------------------------------------------------------------------------
;;; ファイルを 1 つ開く
;;;
;;; **`pager-run`(核)を呼ぶ。`pager`(皮)は呼ばない。** 皮は
;;; `(standard-input)` / `(standard-output)` を取り直すので、ファイラが持っている
;;; 口と二重になる(ファイラが文字列ストリームで動いているときに
;;; 実画面とキーボードへ逃げてしまう)。核は 3 つのストリームを受け取るので
;;; **そのまま渡せる。**
;;;
;;; 合図のストリームは**ファイラと同じもの**を渡す。ページャが頁を送るために
;;; 読む打鍵と、ファイラが選択を動かすために読む打鍵は同じ 1 本の流れである。
;;;
;;; 開く前に画面を消して頭へ戻すのは、ページャが位置を指定せず
;;; **上から順に流すだけ**だからである(#215 の設計)。
;;;
;;; 頁の高さは `*pager-page-lines*` の既定(20)に委ねる。**こちらで束縛しない**
;;; のは、ページャを読み込んでいない状態でその動的変数に触りたくないからである。
;;; 既定の 20 が実画面に収まることは test/lisp/term_test.lisp が機械で見ている。
(defun %filer-open-file (path in out)
  (term-clear)
  (term-set-cursor 0 0)
  (with-open-input-file (src path)
    (pager-run src in out)))

;;; ---------------------------------------------------------------------------
;;; 核。**ここがアプリの全部で、場所とストリームしか触らない**
;;;
;;;   path … 始める場所(絶対パス、または `*cwd*` からの相対。`ls`/`cd` と同じ契約)
;;;   in   … 打鍵を読む入力ストリーム
;;;   out  … 表示先の出力ストリーム
;;;
;;; 打てるキー:
;;;   下 / j          … 選択を 1 つ下へ
;;;   上 / k          … 選択を 1 つ上へ
;;;   Home / End      … 一覧の端へ
;;;   改行 / 右       … 開く(ディレクトリなら入る / ファイルならページャ)
;;;   u / U / 左      … 親へ戻る
;;;   q / Q           … やめる
;;;   それ以外        … 何もしない(**描き直すだけ**)
;;;
;;; 返り値は **(理由 動いた回数 開いた本数 いまの場所) の 4 つ**:
;;;   'filer-quit   … q でやめた
;;;   'filer-eof    … 打鍵が尽きた(文字列入力ストリームでの打ち切りがこれ)
;;;   'filer-no-dir … 始める場所が読めなかった(**1 画面も描いていない**)
;;;
;;; **大きい値を返さない。** 対話の輪は結果を必ず印字する(repl.c:67/:70)ので、
;;; 項目のリストを返すと画面が流れる。記号 1 つと fixnum 2 つと短い文字列だけ。
;;;
;;; 「動いた回数」は**受け付けた操作の数**である(選択の移動とディレクトリの移動)。
;;; 断った操作(端で押した / 読めないディレクトリ)は数えない ——
;;; **断ったことを試験が言えるようにするため**である。
(defun filer-run (path in out)
  (let* ((n (%filer-floor1 (dynamic *filer-list-lines*)))
         (w (%filer-floor1 (dynamic *filer-row-cols*)))
         (cur (%filecmd-absolute-path (dynamic *cwd*) path))
         (entries (%filer-list cur))
         (sel 0) (top 0) (moves 0) (opens 0) (shown 0)
         (reason 'filer-eof) (running t))
    (if (null entries)
        ;; **始める場所が読めない。** 画面を触らずに言って終わる
        ;; (`ls` の "no such file or directory" と同じ流儀)
        (progn
          (format out "~A: no such directory~%" cur)
          (list 'filer-no-dir 0 0 cur))
      (progn
        (while running
          (setq top (%filer-window-top sel top n))
          (setq shown (%filer-draw cur entries sel top n w out))
          (let ((k (term-read-key in))
                (lastpos (- (%filer-length entries) 1)))
            (cond
              ;; 打鍵が尽きた。**待ち続けない**
              ((null k) (setq reason 'filer-eof) (setq running nil))
              ((or (char= k #\q) (char= k #\Q))
               (setq reason 'filer-quit) (setq running nil))
              ((or (char= k *key-down*) (char= k #\j))
               (if (< sel lastpos)
                   (progn (setq sel (+ sel 1)) (setq moves (+ moves 1)))
                 nil))
              ((or (char= k *key-up*) (char= k #\k))
               (if (> sel 0)
                   (progn (setq sel (- sel 1)) (setq moves (+ moves 1)))
                 nil))
              ((char= k *key-home*)
               (if (> sel 0)
                   (progn (setq sel 0) (setq moves (+ moves 1)))
                 nil))
              ((char= k *key-end*)
               (if (< sel lastpos)
                   (progn (setq sel lastpos) (setq moves (+ moves 1)))
                 nil))
              ;; 親へ戻る
              ((or (char= k #\u) (char= k #\U) (char= k *key-left*))
               (let ((up (%filer-parent cur)))
                 (if (null up)
                     (format out "(at the top)~%")
                   (let ((kids (%filer-list up)))
                     (if (null kids)
                         (format out "~A: cannot read~%" up)
                       (progn (setq cur up) (setq entries kids)
                              (setq sel 0) (setq top 0)
                              (setq moves (+ moves 1))))))))
              ;; 開く
              ((or (char= k #\newline) (char= k *key-right*))
               (let ((e (%filer-nth entries sel)))
                 (if (null e)
                     nil
                   ;; **名前の連結は `%filecmd-absolute-path` に任せる。**
                   ;; "." / ".." がそのまま正しく解ける(FAT のサブディレクトリは
                   ;; この 2 つを必ず持っているので、素で繋ぐと "/app/NOTES/.." の
                   ;; ような場所を抱えたまま先へ進んでしまう)
                   (let ((full (%filecmd-absolute-path
                                 cur (%filer-entry-name e))))
                     (if (%filer-dir-p e)
                         (let ((kids (%filer-list full)))
                           (if (null kids)
                               (format out "~A: cannot read~%" full)
                             (progn (setq cur full) (setq entries kids)
                                    (setq sel 0) (setq top 0)
                                    (setq moves (+ moves 1)))))
                       (progn
                         (%filer-open-file full in out)
                         (setq opens (+ opens 1))))))))
              ;; 知らないキーは**何もしない。** 画面は次の反復で描き直される
              (t nil))))
        ;; **合図の行の下へカーソルを戻す。** 戻さないと、このあとの対話の
        ;; 1 行目が選択行に重なって出る(カーソルは選択行に置いたままだから)
        (term-set-cursor (+ shown 2) 0)
        (list reason moves opens cur)))))

;;; ---------------------------------------------------------------------------
;;; 皮。**これだけが画面とキーボードを知っている**
;;;
;;; `(standard-input)` / `(standard-output)` を**それぞれ 1 回だけ**呼んで核へ渡す。
;;; どちらも未束縛なら新しい流れを作る(1 回あたり約 1.6KB の確保)ので、
;;; ループの中で呼んではならない。
;;;
;;; やることは 3 つ:
;;;   1. 素の入力に入れる(エコーを止め、行の確定を待たない)。**必ず戻す**
;;;   2. 一覧の行数を実画面に収まるところまで切り下げる
;;;   3. 行の桁数を実画面の桁に収める
;;;
;;; ### 行数の上限が `(- (term-rows) 3)` である理由
;;;
;;; 使う行は **見出し 1 + 一覧 n + 合図 1**、そして**最下行は空けておく。**
;;; 空けるのは、合図の行の後ろに出す行送りで画面が巻き上がらないようにするため
;;; である(巻き上がると格子が 1 行ずれて、位置指定の意味が静かに壊れる)。
;;; つまり n + 3 <= rows。
;;;
;;; ### 無条件に上書きしない理由
;;;
;;; 利用者と試験が `dynamic-let` で指定した値を黙って消さないためである
;;; (test/lisp/filer_test.lisp は 3 行の窓で巻きを見る)。
;;; **切り下げなら、指定を尊重したうえで必ず収まる。**
;;;
;;; ### `unwind-protect` で囲む理由 —— #215 で実際に踏んだバグ
;;;
;;; 素直に「終わりに戻す」と書くと、**途中で脱出したときに素の入力が入ったまま
;;; 残る。** 以後、対話の輪で打鍵が画面に出なくなる。#215 では無いファイルを
;;; 渡したときの signal で起き、test/lisp/pager_app_disk_test.lisp §5 が捕まえた。
;;; ここでも同じ形にしてある(陽性対照は test/lisp/filer_app_disk_test.lisp §4)。
(defun filer (path)
  (let ((in (standard-input))
        (out (standard-output))
        (n (%filer-clamp (dynamic *filer-list-lines*) (- (term-rows) 3)))
        (w (%filer-clamp (dynamic *filer-row-cols*) (term-cols))))
    (let ((was (term-raw-input t)))
      (unwind-protect
          (dynamic-let ((*filer-list-lines* n) (*filer-row-cols* w))
            (filer-run path in out))
        (term-raw-input was)))))
