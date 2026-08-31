;;;; デバイス管理(*devices*)のLisp側API。
;;;;
;;;; utility.lisp/init_aot.lispとは異なりAOTトランスパイル対象外の、通常のload形式
;;;; の(インタプリタ実行専用)ファイル。REPLから(load "src/lisp/device.lisp")で
;;;; ide.lisp等の各ドライバより先に明示的に読み込む。文字リテラル(#\X)はAOT
;;;; トランスパイラでは未対応だが、このファイルはインタプリタ実行のみなので
;;;; 使用できる(が、utility.lispの慣習に合わせformat "~C"経由での文字出力に統一する)。
;;;;
;;;; 個別ドライバ(ide.lisp等)への依存を持たない自己完結ファイルとする。FAT16の
;;;; UUID検出もfat16.lispを読み込んでいるかどうかに関わらず動作するよう、
;;;; read-sectorのみを使って直接セクタ0を解析する(このコードベースにはfboundpが
;;;; 無いため、load済みかどうかの分岐は書けない)。

;; *devices* : ((name . info-plist) ...) のalist。nameはblk0/blk1等のシンボル。
;; infoプリストの形: (:type 'blk :handle <raw-pointer> :name "ide-secondary-master"
;; :model "QEMU HARDDISK" :total-sectors 32768)
;; *classes*/*generic-methods*/*handlers*と同じ理由でdefdynamic+%%set-dynamicを
;; 使う(defglobalは呼び出し元の環境にしか書き込まれず、複数ドライバから追加登録
;; されていくレジストリには使えない)。
(defdynamic *devices* nil)

;; (%device-blk-name n) : n(fixnum)をblk0,blk1,...というシンボルにする。
(defun %device-blk-name (n)
  (string-to-symbol (string-append "BLK" (%room-digit-string n))))

;; (%device-count-of-type type alist) : alistの中で:typeがtypeとeqなエントリの数。
(defun %device-count-of-type (type alist)
  (if (null alist)
      0
      (+ (if (eq (%plist-get (cdr (car alist)) ':type nil) type) 1 0)
         (%device-count-of-type type (cdr alist)))))

;; (%device-register name info) : *devices*へ(name . info)を追加登録し、nameを返す。
(defun %device-register (name info)
  (progn
    (%%set-dynamic '*devices* (cons (cons name info) (dynamic *devices*)))
    name))

;; (%device-register-blk info) : infoにblk+番号の名前を割り振って*devices*へ登録する。
(defun %device-register-blk (info)
  (%device-register (%device-blk-name (%device-count-of-type 'blk (dynamic *devices*))) info))

;; (%device-handle name) : *devices*からnameエントリの:handleを取り出す(無ければnil)。
(defun %device-handle (name)
  (%plist-get (cdr (assoc name (dynamic *devices*))) ':handle nil))

;; (%device-output-stream) : *standard-output*が動的束縛されていればそれを使い、
;; なければ画面への新規出力ストリームを開く(utility.lisp/ide.lispと同じ慣習)。
(defun %device-output-stream ()
  (if (dynamic *standard-output*)
      (dynamic *standard-output*)
      (open-output-stream)))

;;; --- FAT16 Volume ID -> UUID ---
;;;
;;; BPB上のBS_FilSysType(offset 0x36, 8byte)が"FAT16"で始まるかを検出信号とし、
;;; 一致した場合のみVolumeID(offset 0x27, 4byte little-endian)をXXXX-XXXX形式の
;;; 大文字16進文字列にする(Windowsのvolコマンドと同じ、上位16bit-下位16bitの split)。
;;; FAT32は他のどこにも実装されていないため、ここでも対象外。

;; (%device-list-nth lst n) : lst(0起点)のn番目の要素を返す。
(defun %device-list-nth (lst n)
  (if (= n 0)
      (car lst)
      (%device-list-nth (cdr lst) (- n 1))))

;; (%device-fat16-signature-p bytes) : bytes(read-sectorの返す512要素リスト)の
;; offset 0x36(54)から"FAT16"のASCIIコード列が続いているかを調べる。
(defun %device-fat16-signature-p (bytes)
  (and (= (%device-list-nth bytes 54) 70)
       (= (%device-list-nth bytes 55) 65)
       (= (%device-list-nth bytes 56) 84)
       (= (%device-list-nth bytes 57) 49)
       (= (%device-list-nth bytes 58) 54)))

;; (%device-fat16-volume-id bytes) : offset 0x27(39)から4byte little-endianで
;; VolumeIDを読み、非負整数にする。
(defun %device-fat16-volume-id (bytes)
  (+ (%device-list-nth bytes 39)
     (* (%device-list-nth bytes 40) 256)
     (* (%device-list-nth bytes 41) 65536)
     (* (%device-list-nth bytes 42) 16777216)))

;; (%device-hex-nibble-code n) : 0-15の値をASCII文字コード(fixnum)にする
;; (0-9->'0'-'9', 10-15->'A'-'F')。ide.lisp %hex-nibble-codeと同じ変換。
(defun %device-hex-nibble-code (n)
  (if (< n 10)
      (+ n 48)
      (+ (- n 10) 65)))

;; (%device-hex-write-digits out value ndigits) : valueの下位ndigits桁を
;; 大文字16進数としてoutへ書き出す(桁数に足りない上位は0埋め)。
(defun %device-hex-write-digits (out value ndigits)
  (if (= ndigits 0)
      nil
      (progn
        (%device-hex-write-digits out (div value 16) (- ndigits 1))
        (format out "~C" (%device-hex-nibble-code (mod value 16))))))

;; (%device-fat16-uuid-string volume-id) : volume-id(32bit非負整数)を
;; "XXXX-XXXX"形式の大文字16進文字列にする。
(defun %device-fat16-uuid-string (volume-id)
  (let ((out (create-string-output-stream)))
    (progn
      (%device-hex-write-digits out (div volume-id 65536) 4)
      (format out "-")
      (%device-hex-write-digits out (mod volume-id 65536) 4)
      (get-output-stream-string out))))

;; (%device-fat16-uuid handle) : handleのセクタ0を読み、FAT16と判定できた場合は
;; UUID文字列を、そうでなければnilを返す。
(defun %device-fat16-uuid (handle)
  (let ((bytes (read-sector handle 0)))
    (if (and bytes (%device-fat16-signature-p bytes))
        (%device-fat16-uuid-string (%device-fat16-volume-id bytes))
        nil)))

;;; --- describe ---

;; (%device-describe-blk out info) : ブロックデバイスの詳細情報(name/model/
;; total-sectors、FAT16なら追加でuuid)をoutへ書き出す。
(defun %device-describe-blk (out info)
  (progn
    (format out "  type: block~%")
    (format out "  name: ~A~%" (%plist-get info ':name ""))
    (format out "  model: ~A~%" (%plist-get info ':model ""))
    (format out "  total-sectors: ~D~%" (%plist-get info ':total-sectors 0))
    (let ((uuid (%device-fat16-uuid (%plist-get info ':handle nil))))
      (if uuid
          (format out "  fat16-uuid: ~A~%" uuid)
          nil))))

;; (%device-describe-info out name info) : :typeで分岐してdescribeの本体を出力する。
(defun %device-describe-info (out name info)
  (progn
    (format out "~A:~%" name)
    (cond
      ((eq (%plist-get info ':type nil) 'blk) (%device-describe-blk out info))
      (t (format out "  unknown device type~%")))))

;; (describe name) : *devices*からnameエントリを探し、詳細情報を%device-output-stream
;; へ表示する。見つかった場合はinfoプリストを、見つからない場合はnilを返す。
(defun describe (name)
  (let ((entry (assoc name (dynamic *devices*))))
    (if (null entry)
        (progn
          (format (%device-output-stream) "~A: no such device~%" name)
          nil)
        (progn
          (%device-describe-info (%device-output-stream) name (cdr entry))
          (cdr entry)))))
