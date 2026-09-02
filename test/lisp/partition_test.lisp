;; test/lisp/partition_test.lisp
;;
;; src/lisp/partition.lisp(PART-M2、GPT対応)の動作確認。
;; QEMU側はMakefileの$(GPT_MULTI_DISK_IMG)(tmp/gpt_multi_test.img)がbus=1,unit=0
;; (blk0)へアタッチされている前提。このイメージはGPTで2パーティションに分割されて
;; おり、パーティション1(16MiB、FAT16、TEST.LSPあり)・パーティション2(40MiB、FAT32、
;; TEST.LSPあり)の順にsgdiskで作成している(Makefile参照)。
;;
;; 本ファイルはtest/lisp/test_framework.lisp(assert-equal等)とsrc/lisp/device.lisp+
;; src/lisp/ide.lisp+src/lisp/partition.lispを、本ファイルより先にboot-entryスクリプト
;; がloadしている前提で書かれている。

;;; --- 1. パーティション1(blk0s0)・パーティション2(blk0s1)が*devices*へ登録されている ---

(defglobal *partition-test-p1* (%device-handle 'blk0s0))
(defglobal *partition-test-p2* (%device-handle 'blk0s1))

(assert-equal t (if *partition-test-p1* t nil))
(assert-equal t (if *partition-test-p2* t nil))

;; パーティションハンドルは生ポインタではなくconsリスト(%ide-partition-handle-p)。
(assert-equal t (%ide-partition-handle-p *partition-test-p1*))
(assert-equal t (%ide-partition-handle-p *partition-test-p2*))

;;; --- 2. total-sectorsがsgdiskで指定したサイズと一致する ---

;; 16MiB = 16777216byte / 512 = 32768セクタ
(assert-equal 32768 (%plist-get (cdr (assoc 'blk0s0 (dynamic *devices*))) ':total-sectors 0))
;; 40MiB = 41943040byte / 512 = 81920セクタ
(assert-equal 81920 (%plist-get (cdr (assoc 'blk0s1 (dynamic *devices*))) ':total-sectors 0))

;;; --- 3. パーティションハンドル経由でread-sectorがLBAオフセットを正しく加算する ---

;; パーティション1はFAT16(BS_FilSysType@0x36が"FAT16")、パーティション2はFAT32
;; (BS_FilSysType@0x52が"FAT32")として実際にmkfs.vfatでフォーマット済みのため、
;; device.lispのUUID検出関数(read-sector経由でセクタ0を見るだけ、無改修)が
;; パーティションハンドルに対しても正しく機能することを確認する。
(assert-equal t (if (%device-fat16-uuid *partition-test-p1*) t nil))
(assert-equal t (if (%device-fat32-uuid *partition-test-p2*) t nil))

;;; --- 4. 親の生ディスク(blk0)自体はFAT16/FAT32どちらでもない ---

;; blk0のLBA0はGPTのプロテクティブMBR(sgdiskが書く、type=0xEE)であり、FAT16/FAT32の
;; BPBではないため、両方nilになるはず(「ディスク全体はFAT16/FAT32でアクセスできない」
;; の実証、documents/devices.md)。
(defglobal *partition-test-whole-disk* (%device-handle 'blk0))
(assert-equal nil (%device-fat16-uuid *partition-test-whole-disk*))
(assert-equal nil (%device-fat32-uuid *partition-test-whole-disk*))
