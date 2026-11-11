;; test/lisp/pager_app_disk_test.lisp
;;
;; [#215] **ページャをアプリ用の円盤(/app)から読み込んで、実機で動かす。**
;;
;; 完了条件のうち「`/app` から読み込んで動くことを実機の段で確かめる」がここ。
;; 核の振る舞い(5 つの対照)は test/lisp/pager_test.lisp にある。
;;
;; **この順でなければ意味を持たない。** 段(test/lisp/qemu_boot_app_pager.lisp)は
;; このファイルを**先に**読む。pager_test.lisp がホスト共有から
;; apps/pager.lisp を読んでしまった後では、「/app から読んだから定義された」と
;; 言えなくなる(定義が先にあっても通ってしまう)。
;; だから下の「読み込む前は未定義である」という陽性対照(規則 8)が要る。
;;
;; 置き場所の約束は apps/hello.lisp と同じで、原本は apps/ にあり
;; ビルドがアプリ専用の FAT32 ディスク(Makefile の APP_DISK_IMG)へ焼き込む。
;; **自動の割り当ては 1 つも無い**ので、読む前に mount を呼ぶ前段が必ず要る
;; (test/lisp/load_disk_test.lisp と同じ)。

;;; ---------------------------------------------------------------------------
;;; 0. 前段: アプリ専用ディスクを名前で引いて割り当てる
;;;
;;;    **装置の名前(blk0/blk1/...)は探査順の連番**なので、付けるディスクの構成が
;;;    変わると同じディスクの名前が動く。構成に依らず同じディスクを指すため、
;;;    チャネルの名前で引く(%device-named、src/lisp/device.lisp)。
;;; ---------------------------------------------------------------------------

(defglobal *pgd-app-device* (%device-named "ide-primary-slave"))

;; **陽性対照: 名前引きが当たっていること。** 当たっていなければ以後の mount は
;; (nil . :fat32) を積むだけで、「読めない」が別の理由で出てしまう
(assert-equal t (if *pgd-app-device* t nil))
(assert-equal t (if (%device-handle *pgd-app-device*) t nil))
;; アプリのディスクは FAT32 である(ここが nil なら焼き込みか結線が狂っている)
(assert-equal t (if (%device-fat32-uuid (%device-handle *pgd-app-device*)) t nil))

(mount "/app" *pgd-app-device* ':fat32)

;;; ---------------------------------------------------------------------------
;;; 1. 【陽性対照】読み込む前は未定義である
;;;
;;;    **これが無いと、次の節は「どこかで定義されていた」でも通る。**
;;;    ページャの関数が 1 つも無いことを先に固定しておく。
;;; ---------------------------------------------------------------------------

(assert-error (pager-run 1 2 3))
(assert-error (pager "/app/pager.lisp"))

;;; ---------------------------------------------------------------------------
;;; 2. 【完了条件】/app から読み込める
;;;
;;;    `.lisp`(拡張子 4 文字)が置けるのは FAT32 だからである。
;;;    旧方式(FAT16)は 8.3 名しか扱えない。
;;; ---------------------------------------------------------------------------

(defglobal *pgd-load-result* (load "/app/pager.lisp"))
(assert-equal t *pgd-load-result*)

;; 読み込んだ結果として、核と皮と頁の高さが在る
(assert-equal 20 (dynamic *pager-page-lines*))
(assert-equal '(pager-end 0 0)
              (pager-run (create-string-input-stream "")
                         (create-string-input-stream "")
                         (create-string-output-stream)))

;;; ---------------------------------------------------------------------------
;;; 3. 【完了条件】/app のファイルを実際に一面ずつ見せる
;;;
;;;    **題材はいま読み込んだ /app/pager.lisp 自身。**
;;;    資料を別に焼き込まないのは、アプリ用の円盤へ入るのが apps/*.lisp だけで
;;;    (Makefile の APP_LISP_SRC)、資料を足すとビルドの規則を広げることになるから。
;;;    自分自身なら「読み込めた経路」と「読める経路」が同じ円盤の同じファイルで
;;;    揃う(= load で読めたのに open-input-file で読めない、の食い違いも見える)。
;;;
;;;    **期待値はファイルの長さに依らない形にする。** 頁の高さ 5 で 1 面だけ出して
;;;    "q" で打ち切れば、出た行数は必ず 5 である(先頭の行だけは固定で言える)。
;;; ---------------------------------------------------------------------------

(defglobal *pgd-nl* (create-string 1 #\newline))
(defglobal *pgd-out* (create-string-output-stream))

;; 行送りの数を数える。**繰り返しは関数の中に入れる**(トップレベルに書いた
;; 繰り返しは 1 反復ごとに再帰的に評価され、スタックを壊す。
;; docs/investigation/app-terminal-io.logic.md §8-10)
(defun %pgd-count-newlines (s)
  (let ((i 0) (n 0) (len (length s)))
    (while (< i len)
      (progn
        (if (char= (elt s i) #\newline) (setq n (+ n 1)) nil)
        (setq i (+ i 1))))
    n))

;; 皮(pager)は (standard-input) / (standard-output) を**それぞれ 1 回だけ**呼ぶ。
;; ここではその 2 つを文字列のストリームへ差し替えて、キー入力なしで動かす
(defglobal *pgd-res*
  (dynamic-let ((*pager-page-lines* 5))
    (with-standard-input (create-string-input-stream (string-append "q" *pgd-nl*))
      (with-standard-output *pgd-out*
        (pager "/app/pager.lisp")))))

(defglobal *pgd-text* (get-output-stream-string *pgd-out*))

;; 1 面(5 行)出して打ち切った
(assert-equal '(pager-quit 1 5) *pgd-res*)
;; **本当に /app/pager.lisp を読んでいること。** 先頭 20 文字はこのアプリの見出し行で、
;; ファイルの長さが変わっても動かない
(assert-equal ";;;; apps/pager.lisp" (subseq *pgd-text* 0 20))
;; 続きがある印が最後に出ている(5 行 + 印)
(assert-equal "--more-- (1) "
              (subseq *pgd-text* (- (length *pgd-text*) 13) (length *pgd-text*)))
;; 5 行ぶんの行送りと、印の前の 1 つ。**行の本数で「一面ぶん」を確かめる**
(assert-equal 5 (%pgd-count-newlines *pgd-text*))

;;; ---------------------------------------------------------------------------
;;; 4. 無いファイルは通知する(ホスト共有へ落ちない)
;;;
;;;    皮が open-input-file の契約をそのまま通していることを見る。
;;;    load と同じ理由(綴り間違いがホスト共有の文面で失敗すると原因が分からない)。
;;; ---------------------------------------------------------------------------

(assert-error-class '<simple-error> (pager "/no-such-mount/x.txt"))
(assert-error-class '<simple-error> (pager "/app/no-such-file.txt"))
