;;;; FAT32ファイルシステムの読み込みAPI。
;;;; ide.lispと同じ理由でAOTトランスパイル対象外の、通常のload形式(インタプリタ
;;;; 実行専用)ファイル。各関数はdeviceハンドル(*devices*/(%device-handle 'blk0)
;;;; 等で得られるblock_device_t*)を引数に取り、read-sector(src/lisp/ide.lisp)経由
;;;; で読み込む。REPLから(load "src/lisp/device.lisp")→(load "src/lisp/ide.lisp")→
;;;; (load "src/lisp/fat32.lisp")の順に明示的に読み込む。src/lisp/fat16.lispとは
;;;; コードを共有しない独立ファイル(ide.lisp/device.lispと同じ「個別ドライバは
;;;; 自己完結ファイル」という方針)。
;;;;
;;;; FAT32-M1: BPB(拡張含む)パースのみを実装する。

;; (%fat32-u16 bytes offset) : bytes(fixnum(0-255)のリスト、read-sectorの戻り値)の
;; offsetから始まるLE 16bit値を1個のfixnumに合成する。
(defun %fat32-u16 (bytes offset)
  (logior (elt bytes offset) (ash (elt bytes (+ offset 1)) 8)))

;; (%fat32-u32 bytes offset) : bytesのoffsetから始まるLE 32bit値を1個のfixnumに
;; 合成する。logior/ashが2引数限定のため%fat32-u16を2回に分けて合成する。
(defun %fat32-u32 (bytes offset)
  (logior (%fat32-u16 bytes offset) (ash (%fat32-u16 bytes (+ offset 2)) 16)))

;; BPB(BIOS Parameter Block)のうち、FAT32のレイアウト計算に必要な最小限の
;; フィールドのみを保持する。FAT16のbpbクラスとの違い: root-entry-count/
;; sectors-per-fatは常に0になるため持たず、代わりに拡張BPBのfat-size-32/
;; root-clusterを持つ。
(defclass bpb32 ()
  ((bytes-per-sector :initarg :bytes-per-sector :initform nil)
   (sectors-per-cluster :initarg :sectors-per-cluster :initform nil)
   (reserved-sectors :initarg :reserved-sectors :initform nil)
   (num-fats :initarg :num-fats :initform nil)
   (total-sectors :initarg :total-sectors :initform nil)
   (fat-size-32 :initarg :fat-size-32 :initform nil)
   (root-cluster :initarg :root-cluster :initform nil)))

;; (fat32-read-bpb device) : deviceのセクタ0(ブートセクタ)を読み、BPBの各フィールドを
;; 取り出してbpb32インスタンスを返す。read-sectorが失敗した場合はnil。
;; オフセットは標準的なFAT32 BPB(共通部+拡張部)のレイアウト通り:
;;   0x0B(11) bytes-per-sector (u16)
;;   0x0D(13) sectors-per-cluster (u8)
;;   0x0E(14) reserved-sectors (u16)
;;   0x10(16) num-fats (u8)
;;   0x13(19) total-sectors-16 (u16、FAT32では常に0なので0x20(32)を使う)
;;   0x20(32) total-sectors-32 (u32)
;;   0x24(36) fat-size-32 (u32、FAT16のsectors-per-fatに相当する実値)
;;   0x2C(44) root-cluster (u32、ルートディレクトリの開始クラスタ番号)
(defun fat32-read-bpb (device)
  (let ((bytes (read-sector device 0)))
    (if (null bytes)
        nil
        (let ((total-sectors-16 (%fat32-u16 bytes 19)))
          (make-instance 'bpb32
            ':bytes-per-sector (%fat32-u16 bytes 11)
            ':sectors-per-cluster (elt bytes 13)
            ':reserved-sectors (%fat32-u16 bytes 14)
            ':num-fats (elt bytes 16)
            ':total-sectors (if (= total-sectors-16 0)
                                 (%fat32-u32 bytes 32)
                                 total-sectors-16)
            ':fat-size-32 (%fat32-u32 bytes 36)
            ':root-cluster (%fat32-u32 bytes 44))))))
