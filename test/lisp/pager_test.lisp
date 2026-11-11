;; test/lisp/pager_test.lisp
;;
;; [#215] **ページャの核(apps/pager.lisp の pager-run)を、キー入力なしで試験する。**
;;
;; 核は「本文・合図・表示先」の 3 つをストリームで受け取る形にしてある。だから
;; **文字列入力ストリームを渡せば、ゲストへキーを送る仕組みが無くても試験できる。**
;; これは設計の要である: 調査(docs/investigation/app-terminal-io.logic.md §9-15)は
;; 「割り込みの層はどの単体試験にもリンクされておらず、**キー入力の仕組みは
;; 全体が無試験**」と報告している。対話を直に書くと、その族をもう 1 つ増やす。
;;
;; 対照は 5 つ(#215 の完了条件):
;;   空のファイル / 1 行 / 頁ちょうど / 頁 + 1 行 / 途中で打ち切る
;;
;; [#216] **§10 を足した: 1 打鍵で進むこと。**
;;   合図の読み取りを read-line から read-char に変えた(apps/pager.lisp)。
;;   **上の 5 つの対照は 1 文字も直していない** —— 文字列入力ストリームに
;;   改行を渡す形は read-char でもそのまま通る。これが「壊していない」ことの
;;   対照であり、同時に「同じページャが 1 打鍵で動く」ことの対照でもある
;;
;; 入力の差し替え(create-string-input-stream)は既に揃っているもので、
;; **新しい組み込みは 1 つも足していない。**
;;
;; /app から読み込んで動かす側は test/lisp/pager_app_disk_test.lisp。

;;; ---------------------------------------------------------------------------
;;; 0. 道具立て
;;;
;;;    **ホスト共有から読む。** 先頭に区切りが無い名前は従来どおりホスト共有へ行く
;;;    (test/lisp/load_disk_test.lisp の契約)。段では /app から読む側が先に走るので、
;;;    ここは「同じ中身・別の経路」で読み直していることになる。
;;;    このファイルを単独で読んでも成立するよう、自分で load する。
;;; ---------------------------------------------------------------------------

(defglobal *pg-load-result* (load "apps/pager.lisp"))
(assert-equal t *pg-load-result*)

;; 文字列の中に行送りを直に書く手段が無い(文字列リテラルのエスケープは
;; バックスラッシュと二重引用符だけ)ので、1 文字の文字列を作って継ぎ合わせる。
;; **桁揃えに ~nA は使えない**(num が無視される。format.c:204-215)ので、
;; 文字列の組み立ては STRING-APPEND でやるのがこの系の作法である
(defglobal *pg-nl* (create-string 1 #\newline))

;; 本文の資料。**行数だけが違う 4 本**を、同じ継ぎ方で作る
(defglobal *pg-t0* "")
(defglobal *pg-t1* (string-append "one" *pg-nl*))
(defglobal *pg-t3* (string-append "l1" *pg-nl* "l2" *pg-nl* "l3" *pg-nl*))
(defglobal *pg-t4* (string-append *pg-t3* "l4" *pg-nl*))
(defglobal *pg-t7* (string-append *pg-t4* "l5" *pg-nl* "l6" *pg-nl* "l7" *pg-nl*))

;; 続きがある印。**apps/pager.lisp が出す文面をここに写してある。**
;; 文面を変えたらここが落ちる(それが狙い)
(defglobal *pg-more1* "--more-- (1) ")
(defglobal *pg-more2* "--more-- (2) ")

;; 1 回ぶん動かして (結果 出力文字列) を返す。
;; **頁の高さは引数で受けて dynamic-let で束縛する。** 動的変数に直に setq すると
;; 以後のすべての対照に漏れる(動的変数はプロセス間でも共有される。調査 §8-3)
(defun pg-run (height src-text in-text)
  (dynamic-let ((*pager-page-lines* height))
    (let ((out (create-string-output-stream)))
      (let ((r (pager-run (create-string-input-stream src-text)
                          (create-string-input-stream in-text)
                          out)))
        (list r (get-output-stream-string out))))))

;; 「改行 n 個」の入力。打鍵で頁を送る側の資料
(defun pg-returns (n)
  (let ((s "") (i 0))
    (while (< i n)
      (progn
        (setq s (string-append s *pg-nl*))
        (setq i (+ i 1))))
    s))

;;; ---------------------------------------------------------------------------
;;; 1. 既定の頁の高さ(【完了条件】頁の高さを変えられる、の基点)
;;;
;;;    **画面から読んでいない**ことを、値そのもので固定する。
;;;    行数・桁数を Lisp から読む口は無い(調査 §9-7)ので、ここは自前の値である
;;; ---------------------------------------------------------------------------

(assert-equal 20 (dynamic *pager-page-lines*))

;;; ---------------------------------------------------------------------------
;;; 2. 【対照 1/5】空のファイル
;;;
;;;    **何も出さず、合図も待たずに終わる。** 待ってしまうと、空のファイルを
;;;    開いた実機の利用者が「改行を打つまで戻ってこない」ことになる
;;; ---------------------------------------------------------------------------

(defglobal *pg-c1* (pg-run 3 *pg-t0* ""))
(assert-equal '(pager-end 0 0) (car *pg-c1*))
(assert-equal "" (car (cdr *pg-c1*)))

;;; ---------------------------------------------------------------------------
;;; 3. 【対照 2/5】1 行
;;;
;;;    頁より短い。1 面だけ出して終わる(合図は待たない)
;;; ---------------------------------------------------------------------------

(defglobal *pg-c2* (pg-run 3 *pg-t1* ""))
(assert-equal '(pager-end 1 1) (car *pg-c2*))
(assert-equal *pg-t1* (car (cdr *pg-c2*)))

;;; ---------------------------------------------------------------------------
;;; 4. 【対照 3/5】頁ちょうど
;;;
;;;    **ここが先読みの要。** 3 行出した時点で「あと 1 文字あるか」を見ずに
;;;    印を出すと、もう何も無いのに利用者を待たせる。
;;;    だから preview-char(消費しない 1 文字の先読み)で終端を確かめている。
;;;
;;;    **合図を 1 つも読んでいないことを、残り物で確かめる**(規則 8 の陽性対照)。
;;;    合図のストリームに改行 1 つを入れておき、動かした後にそれがそのまま
;;;    残っていることを見る。読んでしまっていれば残らない
;;; ---------------------------------------------------------------------------

(defglobal *pg-in-c3* (create-string-input-stream *pg-nl*))
(defglobal *pg-out-c3* (create-string-output-stream))
(defglobal *pg-res-c3*
  (dynamic-let ((*pager-page-lines* 3))
    (pager-run (create-string-input-stream *pg-t3*) *pg-in-c3* *pg-out-c3*)))

(assert-equal '(pager-end 1 3) *pg-res-c3*)
(assert-equal *pg-t3* (get-output-stream-string *pg-out-c3*))
;; 残してあった改行がそのまま残っている = 合図を読んでいない
(assert-equal "" (read-line *pg-in-c3* nil nil))
(assert-equal nil (read-line *pg-in-c3* nil nil))

;;; ---------------------------------------------------------------------------
;;; 5. 【対照 4/5】頁 + 1 行
;;;
;;;    **改行で次の頁へ進む**(#215 の完了条件そのもの)。
;;;    印のあとに自分で行送りを出していないことも、出力の形で固定する
;;;    (実機では打鍵のエコーがその行送りになる。調査 §4-3-10)
;;; ---------------------------------------------------------------------------

(defglobal *pg-c4* (pg-run 3 *pg-t4* (pg-returns 1)))
(assert-equal '(pager-end 2 4) (car *pg-c4*))
(assert-equal (string-append *pg-t3* *pg-more1* "l4" *pg-nl*)
              (car (cdr *pg-c4*)))

;;; ---------------------------------------------------------------------------
;;; 6. 【対照 5/5】途中で打ち切る
;;;
;;;    2 通りある。**どちらも「最後まで出した」と区別できること**が肝である
;;;    (返り値の 1 つめが 'pager-quit になる)
;;;
;;;    (a) "q" の行。**実機の利用者が途中でやめる唯一の手段**
;;;        (脱出キーも制御キーも無い。調査 §9-3/9-4)
;;;    (b) 合図の入力が尽きた。文字列入力ストリームでの打ち切りがこれで、
;;;        実機のキーボードでは起こらない
;;; ---------------------------------------------------------------------------

;; (a) 2 面目を出したところで "q"
(defglobal *pg-c5a* (pg-run 3 *pg-t7* (string-append *pg-nl* "q" *pg-nl*)))
(assert-equal '(pager-quit 2 6) (car *pg-c5a*))
(assert-equal (string-append *pg-t3* *pg-more1*
                             "l4" *pg-nl* "l5" *pg-nl* "l6" *pg-nl* *pg-more2*)
              (car (cdr *pg-c5a*)))

;; 大文字でも同じ(変換表に大文字固定キーは無いが、修飾キーつきの打鍵は届く。調査 §9-3)
(defglobal *pg-c5a2* (pg-run 3 *pg-t7* (string-append "Q" *pg-nl*)))
(assert-equal '(pager-quit 1 3) (car *pg-c5a2*))

;; (b) 合図が尽きた
(defglobal *pg-c5b* (pg-run 3 *pg-t7* ""))
(assert-equal '(pager-quit 1 3) (car *pg-c5b*))
(assert-equal (string-append *pg-t3* *pg-more1*) (car (cdr *pg-c5b*)))

;; 打ち切らずに最後まで送ると 'pager-end になる(上の 2 つの陰性対照)
(defglobal *pg-c5c* (pg-run 3 *pg-t7* (pg-returns 3)))
(assert-equal '(pager-end 3 7) (car *pg-c5c*))

;;; ---------------------------------------------------------------------------
;;; 7. 【完了条件】頁の高さを変えられる
;;;
;;;    **同じ本文を違う高さで送ると、頁数が変わる。**
;;;    高さが効いていなければ 3 本とも同じ頁数になる(規則 8 の対照)
;;; ---------------------------------------------------------------------------

(assert-equal '(pager-end 4 7) (car (pg-run 2 *pg-t7* (pg-returns 3))))
(assert-equal '(pager-end 3 7) (car (pg-run 3 *pg-t7* (pg-returns 3))))
(assert-equal '(pager-end 2 7) (car (pg-run 5 *pg-t7* (pg-returns 3))))
(assert-equal '(pager-end 1 7) (car (pg-run 7 *pg-t7* (pg-returns 3))))
;; 高さが本文より大きければ 1 面で、合図は要らない
(assert-equal '(pager-end 1 7) (car (pg-run 99 *pg-t7* "")))

;; **束縛を抜けたら既定値へ戻っている**(dynamic-let で束縛しているので漏れない)
(assert-equal 20 (dynamic *pager-page-lines*))

;;; ---------------------------------------------------------------------------
;;; 8. 0 以下の高さは 1 に丸める
;;;
;;;    丸めないと「1 行も出ないのに終端でもない」状態で合図を待ち続ける
;;;    (終わらないループになる)。**防御は入れた側で確かめる**
;;; ---------------------------------------------------------------------------

(assert-equal '(pager-end 3 3) (car (pg-run 1 *pg-t3* (pg-returns 2))))
(assert-equal '(pager-end 3 3) (car (pg-run 0 *pg-t3* (pg-returns 2))))

;;; ---------------------------------------------------------------------------
;;; 9. 最後の行に行送りが無い本文
;;;
;;;    ファイルの最後は行送りで終わっていないことがある。**行数の数え方が
;;;    そこで 1 つずれないこと**を固定する(read-line は終端で got_any のまま返す)
;;; ---------------------------------------------------------------------------

(defglobal *pg-c9* (pg-run 3 (string-append "l1" *pg-nl* "l2") ""))
(assert-equal '(pager-end 1 2) (car *pg-c9*))
(assert-equal (string-append "l1" *pg-nl* "l2" *pg-nl*) (car (cdr *pg-c9*)))

;;; ---------------------------------------------------------------------------
;;; 10. 【#216 の完了条件】1 打鍵で進む
;;;
;;;     **これが「足した口が効いている」ことの対照である**(規則 8)。
;;;     合図は 1 文字で判定されるので、改行でないキー(空白)1 つでも頁が進む。
;;;     以前は `read-line` だったので、改行が来るまで 1 頁も進まなかった。
;;;
;;;     実機では素の入力(エコーなし・行確定なし)に入っているので、
;;;     この 1 文字が**打鍵 1 回**に対応する。文字列入力ストリームでは
;;;     同じ経路に 1 文字を流し込んでいることになる(どちらも read-char)。
;;; ---------------------------------------------------------------------------

;; 空白 1 つで次の頁へ。**改行は 1 つも渡していない**
(defglobal *pg-c10* (pg-run 3 *pg-t4* " "))
(assert-equal '(pager-end 2 4) (car *pg-c10*))

;; 改行でないキーで送ったときは、**印のあとに行送りを 1 つ出す。**
;; 出さないと、素の入力ではエコーが無いので次の頁の 1 行目が印の後ろに続く
(assert-equal (string-append *pg-t3* *pg-more1* *pg-nl* "l4" *pg-nl*)
              (car (cdr *pg-c10*)))

;; 【陰性対照】改行で送ったときは行送りを**出さない**(§5 の出力と同じ形)。
;; 行入力では利用者の改行が割り込みの中でエコーされるので、出すと空行が入る
(defglobal *pg-c10b* (pg-run 3 *pg-t4* (pg-returns 1)))
(assert-equal (string-append *pg-t3* *pg-more1* "l4" *pg-nl*)
              (car (cdr *pg-c10b*)))

;; 打鍵 2 つで 3 面(空白と改行を混ぜても数は変わらない)
(assert-equal '(pager-end 3 7) (car (pg-run 3 *pg-t7* "  ")))
(assert-equal '(pager-end 3 7) (car (pg-run 3 *pg-t7* (string-append " " *pg-nl*))))

;; 1 打鍵で打ち切れる(**行の確定を待たない**)。"q" のあとに改行は無い
(assert-equal '(pager-quit 1 3) (car (pg-run 3 *pg-t7* "q")))
