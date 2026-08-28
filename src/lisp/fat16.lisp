;;;; FAT16ファイルシステムの読み込みAPI。
;;;; ide.lispと同じ理由でAOTトランスパイル対象外の、通常のload形式(インタプリタ
;;;; 実行専用)ファイル。src/lisp/ide.lisp(*ide-device*/read-sector)のロードを
;;;; 前提とする。REPLから(load "src/lisp/ide.lisp")→(load "src/lisp/fat16.lisp")
;;;; の順に明示的に読み込む。
;;;;
;;;; FAT16-M1: BPB(Boot Sector)パースのみを実装する(documents/fs.md)。

;; (%fat16-u16 bytes offset) : bytes(fixnum(0-255)のリスト、read-sectorの戻り値)の
;; offsetから始まるLE 16bit値を1個のfixnumに合成する。
(defun %fat16-u16 (bytes offset)
  (logior (elt bytes offset) (ash (elt bytes (+ offset 1)) 8)))

;; (%fat16-u32 bytes offset) : bytesのoffsetから始まるLE 32bit値を1個のfixnumに
;; 合成する。logior/ashが2引数限定のため%fat16-u16を2回に分けて合成する。
(defun %fat16-u32 (bytes offset)
  (logior (%fat16-u16 bytes offset) (ash (%fat16-u16 bytes (+ offset 2)) 16)))

;; BPB(BIOS Parameter Block、ブートセクタの先頭に置かれる)のうち、FAT16の
;; レイアウト計算に必要な最小限のフィールドのみを保持する。
(defclass bpb ()
  ((bytes-per-sector :initarg :bytes-per-sector :initform nil)
   (sectors-per-cluster :initarg :sectors-per-cluster :initform nil)
   (reserved-sectors :initarg :reserved-sectors :initform nil)
   (num-fats :initarg :num-fats :initform nil)
   (root-entry-count :initarg :root-entry-count :initform nil)
   (total-sectors :initarg :total-sectors :initform nil)
   (sectors-per-fat :initarg :sectors-per-fat :initform nil)))

;; (fat16-read-bpb device) : deviceのセクタ0(ブートセクタ)を読み、BPBの各フィールドを
;; 取り出してbpbインスタンスを返す。read-sectorが失敗した場合はnil。
;; オフセットは標準的なFAT16 BPBのレイアウト通り:
;;   0x0B(11) bytes-per-sector (u16)
;;   0x0D(13) sectors-per-cluster (u8)
;;   0x0E(14) reserved-sectors (u16)
;;   0x10(16) num-fats (u8)
;;   0x11(17) root-entry-count (u16)
;;   0x13(19) total-sectors-16 (u16、0なら0x20(32)のtotal-sectors-32を使う)
;;   0x16(22) sectors-per-fat (u16)
;;   0x20(32) total-sectors-32 (u32)
(defun fat16-read-bpb (device)
  (let ((bytes (read-sector device 0)))
    (if (null bytes)
        nil
        (let ((total-sectors-16 (%fat16-u16 bytes 19)))
          (make-instance 'bpb
            ':bytes-per-sector (%fat16-u16 bytes 11)
            ':sectors-per-cluster (elt bytes 13)
            ':reserved-sectors (%fat16-u16 bytes 14)
            ':num-fats (elt bytes 16)
            ':root-entry-count (%fat16-u16 bytes 17)
            ':total-sectors (if (= total-sectors-16 0)
                                 (%fat16-u32 bytes 32)
                                 total-sectors-16)
            ':sectors-per-fat (%fat16-u16 bytes 22))))))
