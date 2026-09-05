;;;; マウントテーブル(*mounts*)のLisp側API。パスをマウントパスへ割り当てた
;;;; ブロックデバイス+ファイルシステム種別へ解決するための登録機能を提供する。
;;;;
;;;; device.lisp/ide.lisp/partition.lisp等と同様、AOTトランスパイル対象外の、
;;;; 通常のload形式(インタプリタ実行専用)ファイル。REPLから
;;;; (load "src/lisp/device.lisp")→(load "src/lisp/ide.lisp")→
;;;; (load "src/lisp/partition.lisp")の後に(load "src/lisp/mount.lisp")で
;;;; 明示的に読み込む(fat16.lisp/fat32.lispより前後どちらでもよい)。
;;;;
;;;; ホスト9P経由のファイルアクセス("/9p"配下)はここには登録しない。C側
;;;; (src/c/mount.c)が*mounts*に依存せず常に組み込みで解決するため、mount.lispが
;;;; 未loadのままでも"/9p/..."パスは動作する。

;; *mounts* : ((path . (device . fs-type)) ...) のalist。pathは"/"や"/mnt"のような
;; マウント先の絶対パス、deviceはblk0s0等の*devices*キー(symbol)、fs-typeは
;; ':fat32/':fat16等のキーワードシンボル。*devices*と同じ理由でdefdynamic+
;; %%set-dynamicを使う(defglobalは呼び出し元の環境にしか書き込まれないため)。
(defdynamic *mounts* nil)

;; (mount path device fs-type) : *mounts*の先頭へ(path . (device . fs-type))を
;; 追加登録し、pathを返す。同じpathを再度mountした場合は新しいエントリが先頭に
;; 積まれ、以後の解決ではそちらが優先される(*devices*の%device-registerと同じく
;; 重複排除はしない)。
(defun mount (path device fs-type)
  (progn
    (%%set-dynamic '*mounts* (cons (cons path (cons device fs-type)) (dynamic *mounts*)))
    path))
