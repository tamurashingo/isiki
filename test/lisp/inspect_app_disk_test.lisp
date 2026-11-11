;; test/lisp/inspect_app_disk_test.lisp
;;
;; [#214] **探索の道具をアプリ用の円盤(/app)から読み込んで、実機で動かす。**
;;
;; 完了条件のうち「`/app` に置く」がここ。核の振る舞い(種類ごとの列挙・たどる・
;; 戻る・循環・任意の値)は test/lisp/inspect_test.lisp にある。
;;
;; **この順でなければ意味を持たない。** 段(test/lisp/qemu_boot_app_inspect.lisp)は
;; このファイルを**先に**読む。inspect_test.lisp がホスト共有から
;; apps/inspect.lisp を読んでしまった後では、「/app から読んだから定義された」と
;; 言えなくなる(定義が先にあっても通ってしまう)。
;; だから下の「読み込む前は未定義である」という陽性対照(規則 8)が要る。
;; **これは #215 のページャがやったことをそのまま真似ている。**
;;
;; 置き場所の約束は apps/pager.lisp / apps/hello.lisp と同じで、原本は apps/ にあり
;; ビルドがアプリ専用の FAT32 ディスク(Makefile の APP_DISK_IMG)へ焼き込む。
;; **自動の割り当ては 1 つも無い**ので、読む前に mount を呼ぶ前段が必ず要る
;; (test/lisp/load_disk_test.lisp と同じ)。

;;; ---------------------------------------------------------------------------
;;; 0. 前段: アプリ専用ディスクを名前で引いて割り当てる
;;;
;;;    **装置の名前(blk0/blk1/...)は探査順の連番**なので、付けるディスクの構成が
;;;    変わると同じディスクの名前が動く。構成に依らず同じディスクを指すため、
;;;    チャネルの名前で引く(%device-named、src/lisp/device.lisp)。
;;;
;;;    ページャの段とこの段は別のブートなので、**mount はここでもう一度要る**
;;;    (割り当ては起動をまたがない)。
;;; ---------------------------------------------------------------------------

(defglobal *insd-app-device* (%device-named "ide-primary-slave"))

;; **陽性対照: 名前引きが当たっていること。** 当たっていなければ以後の mount は
;; (nil . :fat32) を積むだけで、「読めない」が別の理由で出てしまう
(assert-equal t (if *insd-app-device* t nil))
(assert-equal t (if (%device-handle *insd-app-device*) t nil))
;; アプリのディスクは FAT32 である(ここが nil なら焼き込みか結線が狂っている)
(assert-equal t (if (%device-fat32-uuid (%device-handle *insd-app-device*)) t nil))

(mount "/app" *insd-app-device* ':fat32)

;;; ---------------------------------------------------------------------------
;;; 1. 【陽性対照】読み込む前は未定義である
;;;
;;;    **これが無いと、次の節は「どこかで定義されていた」でも通る。**
;;;    核・要約・無検査の窓口を守る包みの 3 つが 1 つも無いことを先に固定する。
;;;
;;;    皮(inspect)をここで呼ばないのは、**万一定義されていたら実機の鍵盤を
;;;    待って段ごと固まる**からである(皮は (standard-input) を読む)。
;;;    核の名前で未定義を確かめるほうが安全で、同じことが言える。
;;; ---------------------------------------------------------------------------

(assert-error (inspect-run 1 2 3))
(assert-error (inspect-brief 1))
(assert-error (%inspect-class-name 1))

;;; ---------------------------------------------------------------------------
;;; 2. 【完了条件】/app から読み込める
;;;
;;;    `.lisp`(拡張子 4 文字)が置けるのは FAT32 だからである。
;;;    旧方式(FAT16)は 8.3 名しか扱えない。
;;; ---------------------------------------------------------------------------

(defglobal *insd-load-result* (load "/app/inspect.lisp"))
(assert-equal t *insd-load-result*)

;; 読み込んだ結果として、つまみ 2 つと核と要約が在る
(assert-equal 12 (dynamic *inspect-max-items*))
(assert-equal 32 (dynamic *inspect-brief-chars*))
(assert-equal "<FIXNUM> 42" (inspect-brief 42))
(assert-equal '(inspect-eof 0 0)
              (inspect-run 42 (create-string-input-stream "")
                           (create-string-output-stream)))

;; 無検査の 3 窓口を守る包みも在る(詳しい対照は inspect_test.lisp §1)
(assert-equal nil (%inspect-class-name 42))
(assert-equal nil (%inspect-instance-class 42))
(assert-equal nil (%inspect-instance-slots 42))

;;; ---------------------------------------------------------------------------
;;; 3. 【完了条件】皮が (standard-input) / (standard-output) を通して動く
;;;
;;;    皮(inspect)は**その 2 つをそれぞれ 1 回だけ**呼んで核へ渡す。
;;;    ここではその 2 つを文字列のストリームへ差し替えて、キー入力なしで動かす
;;;    (test/lisp/pager_app_disk_test.lisp §3 と同じ手)。
;;;
;;;    **番号でたどって中に入り、q で戻る**ところまでを 1 本で見る。
;;; ---------------------------------------------------------------------------

(defglobal *insd-nl* (create-string 1 #\newline))
(defglobal *insd-out* (create-string-output-stream))

(defglobal *insd-res*
  (dynamic-let ((*inspect-max-items* 3))
    (with-standard-input
        (create-string-input-stream (string-append "1" *insd-nl* "q" *insd-nl*))
      (with-standard-output *insd-out*
        (inspect (list 10 (list 20 30)))))))

(defglobal *insd-text* (get-output-stream-string *insd-out*))

;; 1 段入って q でやめた。**返り値は記号 1 つと fixnum 2 つだけ**で、
;; 見ていた値を返していない(対話の輪は結果を必ず印字する。repl.c:67/:70)
(assert-equal '(inspect-quit 1 1) *insd-res*)
;; 2 段ぶんの表示が、この順で出ている
(assert-equal (string-append
               "== <CONS> len=2" *insd-nl*
               " 0: <FIXNUM> 10" *insd-nl*
               " 1: <CONS> len=2" *insd-nl*
               "inspect> "
               "== <CONS> len=2" *insd-nl*
               " 0: <FIXNUM> 20" *insd-nl*
               " 1: <FIXNUM> 30" *insd-nl*
               " (up available)" *insd-nl*
               "inspect> ")
              *insd-text*)

;;; ---------------------------------------------------------------------------
;;; 4. 【完了条件】/app から読んだ版でも循環した構造で止まらない
;;;
;;;    #214 の陽性対照(自分を含むリスト)を、**実機の円盤から読んだ定義で**
;;;    もう一度見る。ホスト共有から読んだ版の対照は inspect_test.lisp §5。
;;;
;;;    **この節が返ってきたということは止まらなかったということである。**
;;; ---------------------------------------------------------------------------

(defglobal *insd-self* (list 1 nil))
(defglobal *insd-self-wired* (set-car (cdr *insd-self*) *insd-self*))

(assert-equal "<CONS> len=2" (inspect-brief *insd-self*))
(assert-equal '(inspect-quit 3 3)
              (dynamic-let ((*inspect-max-items* 2))
                (inspect-run *insd-self*
                             (create-string-input-stream
                              (string-append "1" *insd-nl* "1" *insd-nl*
                                             "1" *insd-nl* "q" *insd-nl*))
                             (create-string-output-stream))))
