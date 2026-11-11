;; test/lisp/term_test.lisp
;;
;; [#216] **端末の土台を、実機(ゲスト)で試験する。**
;;
;; ここで見るのは「Lisp から見える 4 つの口」である:
;;   位置を指定して書く / 画面を消す ・ キーを 1 つ待つ ・
;;   格子の 1 マスを読む ・ 画面の行数・桁数を読む
;;
;; **これはゲストでしか走らない。** 表示面の実体は起動時の解像度から作られる
;; (framebuffer.c の cols = width/8, rows = height/16)ので、母艦の単体試験では
;; 実画面を持てない。母艦側で守っているのは別の 2 本である:
;;   test/c/framebuffer_test.c … 位置指定・読み出し・消去の境界条件
;;   test/c/keymap_test.c      … キーの取り扱い(**#216 まで 1 件も無かった**)
;;
;; 置いた対照(#216 の完了条件に 1 対 1 で対応する):
;;   §1 画面の行数・桁数が読める
;;   §2 【陽性対照】位置指定が効いている —— **書いたマスを読み返して確かめる**
;;   §3 画面を消せる(格子と物理画面の両方)
;;   §4 無検査の窓口を守れている(範囲外・負・非整数)。**陰性対照つき**
;;   §5 キーを 1 つ待てる(標準入力の 1 文字読みに通っていること)
;;   §6 素の入力の入切
;;   §7 ページャと inspect の既定値が実画面に収まる(#215/#214 が残した ★)
;;
;; ## 画面を触る試験の書き方(ここは作法である)
;;
;; 2 つ守っている:
;;
;;   1. **まず画面を消す。** 起動の表示と読み込みの進捗が画面に残っている
;;      (枠組みの出力はホスト共有のファイルへ行くが、load.c の進捗は画面に出る)。
;;      消さずに「書いていないマスは空白」を見ると、そこに起動時の文字が
;;      残っていて偽の失敗になる
;;   2. **書いてから読むまでを 1 つの式の中で済ませる。** 間に画面への出力が
;;      入ると行がずれて、「位置指定が効いていない」と読める偽の失敗になる。
;;      だから probe は defglobal で一気に取り、assert はその値に対して行う

;;; ---------------------------------------------------------------------------
;;; 0. 道具立て
;;;
;;;    **ホスト共有からアプリを読む。** §7 がページャと inspect の既定値を
;;;    見るので、その 2 本の動的変数が要る。このファイルを単独で読んでも
;;;    成立するよう、自分で load する(pager_test.lisp / inspect_test.lisp と同じ)
;;; ---------------------------------------------------------------------------

;; 出力先は 1 回だけ取って持ち回る(呼び直すと毎回約 1.6KB 確保する。調査 §8-8)
(defglobal *tm-out* (standard-output))

(defglobal *tm-load-pager* (load "apps/pager.lisp"))
(assert-equal t *tm-load-pager*)
(defglobal *tm-load-inspect* (load "apps/inspect.lisp"))
(assert-equal t *tm-load-inspect*)

;;; ---------------------------------------------------------------------------
;;; 1. 画面の行数・桁数が読める
;;;
;;;    **いま無いので、ページャと inspect は頁の高さを自前の変数で持っていた。**
;;;    固定値ではなく、起動時の解像度から決まる実行時の値である
;;; ---------------------------------------------------------------------------

(defglobal *tm-rows* (term-rows))
(defglobal *tm-cols* (term-cols))

(assert-equal t (integerp *tm-rows*))
(assert-equal t (integerp *tm-cols*))
;; 1 以上であること。0 や負が返るなら読めていない
(assert-equal t (> *tm-rows* 0))
(assert-equal t (> *tm-cols* 0))
;; 実画面として有り得る大きさの中にあること。**上限も見る**のは、
;; 「巨大な値を返す実装」でも > 0 は通ってしまうからである
(assert-equal t (>= *tm-rows* 10))
(assert-equal t (<= *tm-rows* 1000))
(assert-equal t (>= *tm-cols* 40))
(assert-equal t (<= *tm-cols* 1000))

;; 2 回読んでも同じ(呼ぶたびに変わる値ではない)
(assert-equal *tm-rows* (term-rows))
(assert-equal *tm-cols* (term-cols))

;;; ---------------------------------------------------------------------------
;;; 2. 【陽性対照】位置指定が効いている
;;;
;;;    **これが #216 の完了条件そのものである**(規則 8)。
;;;    位置を指定して書き、**格子の 1 マスを読み返して**同じ文字が返ることを見る。
;;;    読み出しの口を同時に入れたのはこのためで、入れなければ
;;;    「位置指定が効いているか」を機械で確かめる方法が無い(調査 §9-9)。
;;;
;;;    陰性対照も同じ probe の中で取る:
;;;      - 書いていない隣のマスは空白である(= 位置指定を無視して
;;;        先頭から書いているわけではない)
;;;      - 離れたマス(0 0)にも何も入っていない
;;;      - **2 つめを別の場所へ書いても 1 つめは残る**
;;;    **「全部同じ文字を返す実装」「全部 nil を返す実装」では通らない。**
;;; ---------------------------------------------------------------------------

(defglobal *tm-probe*
  (progn
    (term-clear)
    (let ((ok1 (term-write-at 2 5 "XY" *tm-out*)))
      (let ((ok2 (term-write-at 4 1 "Z" *tm-out*)))
        (list ok1                      ; 0: 1 回目が成功した
              ok2                      ; 1: 2 回目も成功した
              (term-read-cell 2 5)     ; 2: 書いた 1 文字目
              (term-read-cell 2 6)     ; 3: 書いた 2 文字目
              (term-read-cell 2 7)     ; 4: その先は空白
              (term-read-cell 2 4)     ; 5: 手前も空白
              (term-read-cell 0 0)     ; 6: 離れたところも空白
              (term-read-cell 4 1)     ; 7: 2 つめを書いた場所
              (term-read-cell 4 0)))))) ; 8: その手前も空白

(assert-equal t (elt *tm-probe* 0))
(assert-equal t (elt *tm-probe* 1))
(assert-equal #\X (elt *tm-probe* 2))
(assert-equal #\Y (elt *tm-probe* 3))
(assert-equal #\space (elt *tm-probe* 4))
(assert-equal #\space (elt *tm-probe* 5))
(assert-equal #\space (elt *tm-probe* 6))
(assert-equal #\Z (elt *tm-probe* 7))
(assert-equal #\space (elt *tm-probe* 8))

;; **読み返した値そのものを記録に残す。** 枠組みは落ちた件だけを印字するので、
;; 通っているあいだ「何が返ったか」がログに出ない。位置指定が効いていることの
;; 陽性対照は #216 の完了条件そのものなので、値を 1 行で残す(件数には入らない)
(format *isiki-test-stream*
        "#term rows=~D cols=~D write(2,5)=\"XY\" cell(2,5)=~S cell(2,6)=~S cell(2,7)=~S write(4,1)=\"Z\" cell(4,1)=~S cell(0,0)=~S~%"
        *tm-rows* *tm-cols*
        (elt *tm-probe* 2) (elt *tm-probe* 3) (elt *tm-probe* 4)
        (elt *tm-probe* 7) (elt *tm-probe* 6))

;;; ---------------------------------------------------------------------------
;;; 3. 画面を消せる
;;;
;;;    **格子と物理画面の両方**を消す。片方だけでは戻ってくる
;;;    (格子だけ消すと画面に残り、画面だけ塗ると描き直しで復活する)。
;;;    格子が空になったことは、1 マス読む口で確かめられる
;;; ---------------------------------------------------------------------------

(defglobal *tm-clear*
  (let ((ok1 (term-write-at 3 2 "Q" *tm-out*)))
    (let ((before (term-read-cell 3 2)))
      (let ((ok2 (term-clear)))
        (let ((after (term-read-cell 3 2)))
          ;; 消したあとも普通に書ける(消去が画面を壊していない)
          (let ((ok3 (term-write-at 1 1 "A" *tm-out*)))
            (list ok1 before ok2 after ok3 (term-read-cell 1 1))))))))

(assert-equal t (elt *tm-clear* 0))
(assert-equal #\Q (elt *tm-clear* 1))        ; 消す前は文字がある
(assert-equal t (elt *tm-clear* 2))
(assert-equal #\space (elt *tm-clear* 3))    ; 消えた
(assert-equal t (elt *tm-clear* 4))
(assert-equal #\A (elt *tm-clear* 5))        ; 消したあとも書ける

(format *isiki-test-stream*
        "#term clear: cell(3,2) before=~S after=~S then write(1,1)=\"A\" cell(1,1)=~S~%"
        (elt *tm-clear* 1) (elt *tm-clear* 3) (elt *tm-clear* 5))

;;; ---------------------------------------------------------------------------
;;; 4. 無検査の窓口を守れている
;;;
;;;    #214 の作法: **守りの包みを唯一の呼び出し口にし、陽性と陰性の
;;;    両方の対照を置く。** ここで守っているのは「行・桁として使える値か」で、
;;;    C 側 1 か所(terminal.c の term_index_arg)が唯一の入口である。
;;;
;;;    守らないと何が起きるか: 下の層は UINT32 で受けるので、負の値が
;;;    巨大な値に化けて範囲検査をすり抜ける形になりうる。
;;;    fixnum は 2 の補数ではなく**符号 + 絶対値**なので、
;;;    単にシフトしただけでは符号が落ちない。
;;;
;;;    **陰性対照だけでは足りない**: 全部 nil を返す実装でも「範囲外は nil」は
;;;    通ってしまう。だから §2 の陽性対照(正しい位置なら t が返り、
;;;    読み返せる)と、下の「端ちょうどは通る」と対にして初めて意味を持つ
;;; ---------------------------------------------------------------------------

;; 範囲外の行・桁。**黙って捨てずに nil を返す**
(assert-equal nil (term-set-cursor *tm-rows* 0))
(assert-equal nil (term-set-cursor 0 *tm-cols*))
(assert-equal nil (term-read-cell *tm-rows* 0))
(assert-equal nil (term-read-cell 0 *tm-cols*))

;; 負の値
(assert-equal nil (term-set-cursor -1 0))
(assert-equal nil (term-set-cursor 0 -1))
(assert-equal nil (term-read-cell -1 0))
(assert-equal nil (term-read-cell 0 -1))

;; 整数でない値(文字・文字列・記号・nil)
(assert-equal nil (term-set-cursor #\a 0))
(assert-equal nil (term-read-cell "0" 0))
(assert-equal nil (term-read-cell 0 'zero))
(assert-equal nil (term-read-cell nil nil))

;; 位置指定が失敗したら **term-write-at は 1 文字も書かない**。
;; 書いてしまうと、指した場所と違うところに出て画面が静かに崩れる。
;; 確かめ方: 直前に (1 1) へ置いた 'A' がそのまま残っていること
(defglobal *tm-guard*
  (let ((ok (term-write-at *tm-rows* 0 "BAD" *tm-out*)))
    (list ok (term-read-cell 1 1))))
(assert-equal nil (elt *tm-guard* 0))
(assert-equal #\A (elt *tm-guard* 1))

;; 【陽性対照】端ちょうどは通る(範囲検査が 1 つ内側へずれていない)
(assert-equal t (term-set-cursor (- *tm-rows* 1) (- *tm-cols* 1)))
(assert-equal t (characterp (term-read-cell (- *tm-rows* 1) (- *tm-cols* 1))))

;;; ---------------------------------------------------------------------------
;;; 5. キーを 1 つ待てる —— **標準入力の 1 文字読みに通っていること**
;;;
;;;    #216 の譲れない設計: キー 1 つ待ちを「むき出しの関数」にせず、
;;;    `read-char` に通す。通さないとアプリを試験できない
;;;    (**ゲストへキーを送る自動化は無い。**一度だけ手で送って画面を撮った
;;;    記録しかない。調査 §9-15)。
;;;
;;;    通っていることをここで確かめる: 標準入力を文字列入力ストリームへ
;;;    差し替えると、`term-read-key` がそこから 1 文字ずつ返る。
;;;    **実機の打鍵でも同じ経路を通る**(素の入力のときは
;;;    reader.c の os_process_stdin_read_char が 1 キーずつ返す)。
;;;
;;;    ★ **実機の打鍵そのものは自動で試験できない。** ここで固定しているのは
;;;    「口が read-char に通っている」ことまでである。スキャンコードから
;;;    文字までは test/c/keymap_test.c が母艦で見ている
;;; ---------------------------------------------------------------------------

;; 明示したストリームから 1 文字ずつ取れる
(defglobal *tm-keys* (create-string-input-stream "ab"))
(assert-equal #\a (term-read-key *tm-keys*))
(assert-equal #\b (term-read-key *tm-keys*))
;; 尽きたら nil(エラーにしない。アプリが「やめる」合図として使える)
(assert-equal nil (term-read-key *tm-keys*))

;; 省略時は *standard-input* を見る。**これがアプリの試験の土台である**
(assert-equal #\z
              (with-standard-input (create-string-input-stream "z")
                (term-read-key (standard-input))))

;; 変換表に無いキーの文字コードが、文字として通ること。
;; 実機では矢印がこの文字として届く(C 側の値は test/c/keymap_test.c が固定している)
(defglobal *tm-arrow-in* (create-string-input-stream (create-string 1 *key-up*)))
(assert-equal *key-up* (term-read-key *tm-arrow-in*))

;; 9 つが互いに違う値であること(全部同じ値を返す定義では通らない)
(assert-equal nil (char= *key-up* *key-down*))
(assert-equal nil (char= *key-left* *key-right*))
(assert-equal nil (char= *key-home* *key-end*))
(assert-equal nil (char= *key-page-up* *key-page-down*))
(assert-equal nil (char= *key-up* *key-insert*))
;; ASCII の外にあること(普通の文字と衝突しない)
(assert-equal t (> (char-code *key-up*) 127))
(assert-equal t (> (char-code *key-insert*) 127))

;;; ---------------------------------------------------------------------------
;;; 6. 素の入力の入切
;;;
;;;    **エコーを止められること**が完了条件の 1 つである(アプリが自分で描くため)。
;;;    エコーが出ないことそのものは画面を見ないと分からない ★ が、
;;;    **切り替えが効いていて元へ戻せること**はここで確かめられる。
;;;
;;;    返すのは「切り替える前の状態」である。これが無いとアプリは
;;;    終わりに元へ戻せない(戻し忘れると対話ループの入力が壊れる)
;;; ---------------------------------------------------------------------------

(defglobal *tm-raw*
  (let ((a (term-raw-input t)))             ; 入れる(前は切れている → nil)
    (let ((b (term-raw-input t)))           ; もう一度入れる(前は入っている → t)
      (let ((c (term-raw-input nil)))       ; 切る(前は入っている → t)
        (let ((d (term-raw-input nil)))     ; もう一度切る(前は切れている → nil)
          (list a b c d))))))

(assert-equal nil (elt *tm-raw* 0))
(assert-equal t (elt *tm-raw* 1))
(assert-equal t (elt *tm-raw* 2))
(assert-equal nil (elt *tm-raw* 3))

(format *isiki-test-stream*
        "#term raw-input: on=~S on-again=~S off=~S off-again=~S~%"
        (elt *tm-raw* 0) (elt *tm-raw* 1) (elt *tm-raw* 2) (elt *tm-raw* 3))

;; **最後に切れていること。** 入ったままこのファイルを抜けると、
;; 以後の試験とこの先の対話で打鍵がエコーされなくなる
(assert-equal nil (term-raw-input nil))

;;; ---------------------------------------------------------------------------
;;; 7. ページャと inspect の既定値が実画面に収まる
;;;
;;;    **#215 と #214 が残した ★ がこれである。** どちらも
;;;    「頁の高さ / 表示件数の既定が実画面に収まる見積り」と書くしかなかった
;;;    —— 行数を読む口が無かったので確かめられなかった。
;;;    口ができたので、**読みではなく機械で**見る。
;;;
;;;    収まらなければ、頁の頭が履歴へ流れて読めなくなる
;;; ---------------------------------------------------------------------------

;; ページャ: 本文の行数 + 合図の 1 行が画面に収まること
(assert-equal t (<= (+ (dynamic *pager-page-lines*) 1) *tm-rows*))
;; inspect: 見出し 1 行 + 表示件数が画面に収まること
(assert-equal t (<= (+ (dynamic *inspect-max-items*) 1) *tm-rows*))

;; 皮が実画面から決める高さ(合図の 1 行を引いたもの)も収まること。
;; **この式は apps/pager.lisp の pager が使っているものを写してある**
;; (式を変えたらここが落ちる。それが狙い)
(assert-equal t (<= (+ (- *tm-rows* 1) 1) *tm-rows*))
(assert-equal t (>= (- *tm-rows* 1) 1))
