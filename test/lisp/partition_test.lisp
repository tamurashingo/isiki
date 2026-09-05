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

;;; --- 5. パーティションスライス(blk0s1、FAT32)経由でもLFN(ロングファイル名、
;;; FAT32-M10)が有効であること ---
;;
;; fat32.lispのfat32-create-file/fat32-read-file/fat32-create-directory/
;; fat32-read-dirは、生ディスクハンドルと同じくパーティションハンドル
;; (%ide-partition-handle-p、read-sector/write-sectorがLBAへオフセットを加算する)
;; に対しても差異なく動くはずだが、これまでのテストはblk0上(パーティション
;; テーブル無しの生ディスク)でのみLFNを確認していた。パーティション2(blk0s1)は
;; TEST.LSPのみを持つ空のFAT32なので、ここで8.3に収まらない長い名前の
;; ファイル・ディレクトリを新規作成し、往復一致することを実証する
;; (既存のTEST.LSPは短名のためLFNの検証にはならない)。

;; 8.3に収まらない長いファイル名での新規作成→読み込みの往復一致。
(defglobal *partition-test-lfn-content* (list 83 76 73 67 69 45 76 70 78)) ;; "SLICE-LFN"

(assert-equal t (if (fat32-create-file *partition-test-p2* "/Partition_Slice_LFN_File.txt" *partition-test-lfn-content*) t nil))
(assert-equal *partition-test-lfn-content* (fat32-read-file *partition-test-p2* "/Partition_Slice_LFN_File.txt"))
;; 大文字小文字を変えたパスでも表示名経由で同一エントリを読める
;; (LFN表示名の大文字小文字非依存マッチの回帰確認)。
(assert-equal *partition-test-lfn-content* (fat32-read-file *partition-test-p2* "/partition_slice_lfn_file.txt"))
(assert-equal (list (list "TEST.LSP" ':file 36) (list "Partition_Slice_LFN_File.txt" ':file 9))
              (fat32-read-dir *partition-test-p2* "/"))

;; 8.3に収まらない長いディレクトリ名での新規作成→内部へのファイル作成の往復一致。
(assert-equal t (if (fat32-create-directory *partition-test-p2* "/Partition_Slice_LFN_Dir") t nil))
(assert-equal (list (list "." ':dir 0) (list ".." ':dir 0))
              (fat32-read-dir *partition-test-p2* "/Partition_Slice_LFN_Dir"))
(assert-equal t (if (fat32-create-file *partition-test-p2* "/Partition_Slice_LFN_Dir/INSIDE.TXT" *partition-test-lfn-content*) t nil))
(assert-equal *partition-test-lfn-content* (fat32-read-file *partition-test-p2* "/Partition_Slice_LFN_Dir/INSIDE.TXT"))

;;; --- 6. mount経由のopen-input-streamがfat32-read-fileと同じ内容を読めること ---
;;
;; (mount "/" 'blk0s1 ':fat32)でパーティション2(blk0s1)をルートへマウントし、
;; open-input-streamへ渡す絶対パスがマウント解決→fat32-read-fileへ橋渡しされる
;; ことを確認する(9P経由ではなくFAT32ドライバ経由で読めていることの実証)。

(mount "/" 'blk0s1 ':fat32)

;; streamからEOFまで1byteずつ読み、fixnumのリストにする(read-byteの戻り値は
;; fixnumまたはEOFでnil)。
(defun %partition-test-read-all-bytes (stream)
  (let ((b (read-byte stream)))
    (if (null b)
        nil
        (cons b (%partition-test-read-all-bytes stream)))))

(defglobal *partition-test-mount-stream* (open-input-stream "/Partition_Slice_LFN_File.txt"))
(assert-equal *partition-test-lfn-content* (%partition-test-read-all-bytes *partition-test-mount-stream*))
(close *partition-test-mount-stream*)
