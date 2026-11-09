;; test/lisp/load_disk_test.lisp
;;
;; [M2-T1/T2/T3 / #193 #194 #195] **ディスクに置いた `.lisp` を load できること、
;; および load のパスの契約。**
;;
;;   先頭が '/' でない -> ホスト共有(9P)。**マウント表は引かない**(従来どおり)
;;   先頭が '/'        -> マウント表で解決する
;;   解決できない      -> **通知する。ホスト共有へ落とさない**
;;
;; **心覚え: 相対 = 母艦(開発時)、絶対 = 機械の中。**
;;
;; 置き場所(決定 D2b。#195): アプリは**アプリ専用の FAT32 ディスク**
;; (Makefile の APP_DISK_IMG、Primary の slave に固定)の根に置く。
;; ホスト側の原本は apps/ で、ビルドがイメージへ焼き込む。
;; **自動の割り当ては 1 つも無い**ので、読む前に mount を呼ぶ前段が必ず要る。

;;; ---------------------------------------------------------------------------
;;; 0. 前段: アプリ専用ディスクを名前で引いて割り当てる
;;;
;;;    **装置の名前(blk0/blk1/...)は探査順の連番**なので、付けるディスクの
;;;    構成が変わると同じディスクの名前が動く。構成に依らず同じディスクを
;;;    指すため、チャネルの名前で引く(%device-named、src/lisp/device.lisp)。
;;; ---------------------------------------------------------------------------

(defglobal *ld-app-device* (%device-named "ide-primary-slave"))

;; **陽性対照: 名前引きが当たっていること。** 当たっていなければ以後の
;; mount は (nil . :fat32) を積むだけで、「読めない」が別の理由で出てしまう
(assert-equal t (if *ld-app-device* t nil))
(assert-equal t (if (%device-handle *ld-app-device*) t nil))
;; アプリのディスクは FAT32 である(ここが nil なら焼き込みか結線が狂っている)
(assert-equal t (if (%device-fat32-uuid (%device-handle *ld-app-device*)) t nil))
;; **データ用ディスク(blk0)とは別物であること。** 同じなら「アプリ専用の
;; ディスクを足した」ことを何も確かめていない
(assert-equal nil (eq *ld-app-device* 'blk0))

(mount "/app" *ld-app-device* ':fat32)

;;; ---------------------------------------------------------------------------
;;; 1. 【完了条件】絶対パスの `.lisp` が load できる
;;;
;;;    **`.lisp`(拡張子 4 文字)が置けるのは FAT32 だから**である。
;;;    旧方式(FAT16)は 8.3 名しか扱えないので *.LSP に限られる。
;;; ---------------------------------------------------------------------------

;; **読まれたことの見分け方。** アプリは (defglobal *isiki-hello-mark* 'from-hello-lisp)
;; を持つので、**読む前に別の値へ書き換えておけば**、戻り値だけでなく
;; 「本当にもう一度読まれた」ことが分かる(数える形にするとアプリ側の defglobal が
;; 読み込みごとに初期値へ戻してしまい、何も分からなくなる)。
(defglobal *isiki-hello-mark* 'never-loaded)

;; [陰性対照] まずホスト共有から読む。**先頭に区切りが無い名前は
;; 従来どおりホスト共有へ行く**(マウント表を張った後でも変わらない)
(assert-equal t (load "apps/hello.lisp"))
(assert-equal 'from-hello-lisp *isiki-hello-mark*)
(assert-equal 42 (isiki-hello))

;; ディスクから読む。**同じ中身・別の経路。**
(setq *isiki-hello-mark* 'stale-before-disk-load)
(assert-equal t (load "/app/hello.lisp"))
(assert-equal 'from-hello-lisp *isiki-hello-mark*)
(assert-equal 42 (isiki-hello))

;; 割り当ては接頭辞でしかない。**同じディスクを別の名前にも割り当てれば、
;; そちらの絶対パスでも読める**(= 経路はマウント表で決まっていて、
;; "/app" のような特定の綴りが埋め込まれているのではない)
(mount "/mnt" *ld-app-device* ':fat32)
(setq *isiki-hello-mark* 'stale-before-mnt-load)
(assert-equal t (load "/mnt/hello.lisp"))
(assert-equal 'from-hello-lisp *isiki-hello-mark*)

;;; ---------------------------------------------------------------------------
;;; 2. 【完了条件】解決できない絶対パスは通知する(ホスト共有へ落ちない)
;;;
;;;    **落とすと、割り当て名の綴り間違いがホスト共有の文面で失敗して
;;;    原因が分からなくなる**(docs/investigation/m2-load-from-disk.logic.md §9-5)。
;;;    文面を丸ごと固定して、**どちらの理由で落ちたのかを残す。**
;;; ---------------------------------------------------------------------------

(defun ld-catch (thunk)
  (block ld
    (with-handler (lambda (c) (return-from ld c)) (funcall thunk))))

(assert-error-class '<simple-error> (load "/no-such-mount/x.lisp"))
(defglobal *ld-c1* (ld-catch (lambda () (load "/no-such-mount/x.lisp"))))
(assert-equal "load: /no-such-mount/x.lisp: no such mount"
              (car (simple-error-format-arguments *ld-c1*)))

;; **綴り間違いも同じ理由で落ちる。** "/app2" は "/app" に一致しない(境界の検査)
(defglobal *ld-c2* (ld-catch (lambda () (load "/app2/hello.lisp"))))
(assert-equal "load: /app2/hello.lisp: no such mount"
              (car (simple-error-format-arguments *ld-c2*)))

;;; ---------------------------------------------------------------------------
;;; 3. 割り当ては解決できたが、ディスクにそのファイルが無い
;;;
;;;    **理由が 2 の場合と違う**ことを文面で固定する。
;;;    FAT の運転手は「ファイルが無い」と「空のファイル」を区別できない
;;;    (既知の制約)ので、どちらもこの文面になる。
;;; ---------------------------------------------------------------------------

(defglobal *ld-c3* (ld-catch (lambda () (load "/app/no-such-app.lisp"))))
(assert-equal "load: /app/no-such-app.lisp: cannot read file"
              (car (simple-error-format-arguments *ld-c3*)))

;;; ---------------------------------------------------------------------------
;;; 4. 作業ディレクトリは効かない(決定。#195)
;;;
;;;    ls / cd / cat は効かせているが、load に効かせると
;;;    「相対 = ホスト共有」の契約と正面からぶつかる。
;;;    **効いていないことを試験で固定する**(将来その契約を変えるときに、
;;;    ここが落ちて「決めたことを変えている」と気づけるように)。
;;; ---------------------------------------------------------------------------

(cd "/app")
(assert-equal "/app" (pwd))
;; cd の後でも、先頭に区切りが無い名前はホスト共有のルートから見た相対指定のまま
(setq *isiki-hello-mark* 'stale-before-cwd-load)
(assert-equal t (load "apps/hello.lisp"))
(assert-equal 'from-hello-lisp *isiki-hello-mark*)
;; 作業ディレクトリ相対("hello.lisp" が /app/hello.lisp になる)には**ならない**
(assert-error-class '<simple-error> (load "hello.lisp"))
;; 後始末。**(cd "/") では戻せない**("/" は割り当てていないので cd は失敗する)。
;; この段はこのファイルで終わりだが、作業ディレクトリを残さない
(%%set-dynamic '*cwd* "/")
(assert-equal "/" (dynamic *cwd*))
