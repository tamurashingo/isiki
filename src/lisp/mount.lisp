;;;; マウントテーブル(*mounts*)のLisp側API。パスをマウントパスへ割り当てた
;;;; ブロックデバイス+ファイルシステム種別へ解決するための登録機能を提供する。
;;;;
;;;; device.lisp/ide.lisp/partition.lisp等と同様、M15(#29)でAOTトランスパイル
;;;; 対象に移動済み。ビルド時にsrc/c/lisp_compiled.cへ
;;;; 変換されカーネルバイナリへ直接リンクされ、ブート時に
;;;; os_register_aot_init_functions()/os_run_aot_toplevel_forms()経由で既に
;;;; global_environmentへ登録・初期化済みのため、REPLからの明示的なloadは不要
;;;; (このコメントは移行前の記述が古いまま残っていたもので、[ファイルI/O]#52
;;;; (M9)の調査で発見・修正した)。
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
