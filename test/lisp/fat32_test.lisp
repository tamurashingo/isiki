;; test/lisp/fat32_test.lisp
;;
;; src/lisp/fat32.lisp(FAT32-M1〜M4: BPBパース/ディレクトリ列挙/クラスタチェイン/
;; ファイル読み込み)の動作確認。
;; QEMU側はMakefileの$(FAT32_DISK_IMG)ターゲット(mkfs.vfat -F 32でフォーマットした
;; 40MBイメージ)をQEMU_DISK_IMG経由でbus=1,unit=0にアタッチしている。
;; 期待値はホスト上でxxd -g1 tmp/fat32_test.imgのブートセクタを目視確認して得たもの
;; (mkfs.fat 4.2、40MBイメージのデフォルト値)。
;;
;; 本ファイルはtest/lisp/test_framework.lisp(assert-equal)とsrc/lisp/device.lisp+
;; src/lisp/ide.lisp(*devices*)とsrc/lisp/fat32.lisp(fat32-read-bpb)を、本ファイルより
;; 先にboot-entryスクリプトがloadしている前提で書かれている。

;; *devices*からblk0(Secondary IDEチャネルのデバイス)のハンドルを取り出す。
(defglobal *fat32-test-device* (%device-handle 'blk0))

(defglobal fat32-test-bpb (fat32-read-bpb *fat32-test-device*))

(assert-equal t (if fat32-test-bpb t nil))

(assert-equal 512 (slot-value fat32-test-bpb 'bytes-per-sector))
(assert-equal 1 (slot-value fat32-test-bpb 'sectors-per-cluster))
(assert-equal 32 (slot-value fat32-test-bpb 'reserved-sectors))
(assert-equal 2 (slot-value fat32-test-bpb 'num-fats))
(assert-equal 81920 (slot-value fat32-test-bpb 'total-sectors))
(assert-equal 630 (slot-value fat32-test-bpb 'fat-size-32))
(assert-equal 2 (slot-value fat32-test-bpb 'root-cluster))

;;; --- FAT32-M2: ルート/サブディレクトリ統一エントリ列挙 ---
;;
;; $(FAT32_DISK_IMG)のルートディレクトリは、作成順にHELLO.TXT(空ファイル)→
;; TEST.LSP(18byte)→BIG.TXT(1000byte)→WRITE1.TXT(512byte、FAT32-M6書き込み
;; テスト専用)→SUBDIR(NESTED.TXT/DEEPER/DEEP.TXTを持つ)→DELETED.TXT(最後に
;; 作成後にrm、先頭バイトが0xE5になる)という並び(Makefile参照)。削除済み
;; エントリはスキップされるため、戻り値はHELLO.TXT/TEST.LSP/BIG.TXT/WRITE1.TXT/
;; SUBDIRの5件のみになるはず。

(assert-equal (list (list "HELLO.TXT" ':file 0) (list "TEST.LSP" ':file 18) (list "BIG.TXT" ':file 1000) (list "WRITE1.TXT" ':file 512)
                     (list "SUBDIR" ':dir 0))
              (fat32-read-dir *fat32-test-device* "/"))

;; サブディレクトリも同じ抽象(クラスタチェイン)で解決できることの確認
;; (多階層パス、FAT16ではM7aまで対応が遅れたがFAT32は最初から対応)。

(assert-equal (list (list "." ':dir 0) (list ".." ':dir 0) (list "NESTED.TXT" ':file 19) (list "DEEPER" ':dir 0))
              (fat32-read-dir *fat32-test-device* "/SUBDIR"))
(assert-equal (list (list "." ':dir 0) (list ".." ':dir 0) (list "DEEP.TXT" ':file 0))
              (fat32-read-dir *fat32-test-device* "/SUBDIR/DEEPER"))

;;; --- FAT32-M3: FATテーブル(32bit)とクラスタチェイン追跡 ---
;;
;; Makefileでクラスタ40→41→44→終端という非連続なチェインを合成している
;; (実際に使用中のクラスタとは重複しない、xxdで目視確認済み)。

(assert-equal (list 40 41 44) (fat32-cluster-chain *fat32-test-device* fat32-test-bpb 40))

;;; --- FAT32-M4: クラスタ→セクタ変換とファイル本体読み込み ---
;;
;; BIG.TXTは1000byte(sectors-per-cluster=1=512byte/clusterなので2クラスタに
;; 分割される)。fat32-read-fileがクラスタ境界を跨いだ読み込みを正しく行えることを、
;; 全体ではなく代表点(先頭・1クラスタ目末尾・2クラスタ目先頭)の値で確認する
;; (fat16.lispのBIG.TXTテストと同じ理由、再帰の深さを増やさないため)。BIG.TXTの
;; 内容は"0123456789"の繰り返しなので、インデックスiの値は(i mod 10)+48
;; (ASCIIコード)になる。

(assert-equal (list 72 101 108 108 111) (subseq (fat32-read-file *fat32-test-device* "/TEST.LSP") 0 5))
(assert-equal 18 (length (fat32-read-file *fat32-test-device* "/TEST.LSP")))

(defglobal fat32-test-big (fat32-read-file *fat32-test-device* "/BIG.TXT"))
(assert-equal 1000 (length fat32-test-big))
(assert-equal 48 (elt fat32-test-big 0))
(assert-equal 49 (elt fat32-test-big 511))
(assert-equal 50 (elt fat32-test-big 512))

(assert-equal (list 110 101 115 116 101) (subseq (fat32-read-file *fat32-test-device* "/SUBDIR/NESTED.TXT") 0 5))
(assert-equal 19 (length (fat32-read-file *fat32-test-device* "/SUBDIR/NESTED.TXT")))
