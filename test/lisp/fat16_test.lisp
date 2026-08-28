;; test/lisp/fat16_test.lisp
;;
;; src/lisp/fat16.lisp(FAT16-M1: BPBパース)の動作確認。
;; QEMU側はMakefileの$(FAT16_DISK_IMG)ターゲット(mkfs.vfat -F 16でフォーマットした
;; 16MBイメージ)をQEMU_DISK_IMG経由でbus=1,unit=0にアタッチしている。
;; 期待値はホスト上でxxd -g1 tmp/fat16_test.imgのセクタ0を目視確認して得たもの
;; (mkfs.fat 4.2、16MBイメージのデフォルト値)。
;;
;; 本ファイルはtest/lisp/test_framework.lisp(assert-equal)とsrc/lisp/ide.lisp
;; (*ide-device*)とsrc/lisp/fat16.lisp(fat16-read-bpb)を、本ファイルより先に
;; boot-entryスクリプトがloadしている前提で書かれている。

(defglobal fat16-test-bpb (fat16-read-bpb *ide-device*))

(assert-equal t (if fat16-test-bpb t nil))

(assert-equal 512 (slot-value fat16-test-bpb 'bytes-per-sector))
(assert-equal 4 (slot-value fat16-test-bpb 'sectors-per-cluster))
(assert-equal 4 (slot-value fat16-test-bpb 'reserved-sectors))
(assert-equal 2 (slot-value fat16-test-bpb 'num-fats))
(assert-equal 512 (slot-value fat16-test-bpb 'root-entry-count))
(assert-equal 32768 (slot-value fat16-test-bpb 'total-sectors))
(assert-equal 32 (slot-value fat16-test-bpb 'sectors-per-fat))

;;; --- FAT16-M2: ルートディレクトリエントリの列挙 ---
;;
;; $(FAT16_DISK_IMG)のルートディレクトリは、作成順にHELLO.TXT(空ファイル)→
;; TEST.LSP(18byte)→BIG.TXT(2500byte)→DELETED.TXT(最後に作成後にrm、先頭バイトが
;; 0xE5になる)→残りは0x00の空き終端、という並びになっている(Makefileのmkfs.vfat
;; 手順を参照。DELETED.TXTを最後に作る/消すのは、それより前に作るとカーネルの
;; vfatドライバが後続ファイル作成時に空いた0xE5スロットを再利用してしまうため)。
;; 削除済みエントリはスキップされ、走査は0x00終端で止まるため、戻り値は
;; HELLO.TXT/TEST.LSP/BIG.TXTの3件のみになるはず。

(assert-equal (list (list "HELLO.TXT" ':file 0) (list "TEST.LSP" ':file 18) (list "BIG.TXT" ':file 2500))
              (fat16-read-dir *ide-device* "/"))

;;; --- FAT16-M4: クラスタ→セクタ変換とファイル本体読み込み ---
;;
;; TEST.LSPは単一クラスタ(3、終端)、BIG.TXTは2クラスタ(4→5→終端)に分割されている
;; (Makefile参照、クラスタサイズは4セクタ*512byte=2048byte)。fat16-read-fileが
;; クラスタ境界を跨いだ読み込みを正しく行えることを、2500byte全体ではなく代表点
;; (先頭・1クラスタ目末尾・2クラスタ目先頭・末尾)の値で確認する(全体を
;; assert-equalで比較すると再帰の深さが大きくなるため)。
;; BIG.TXTの内容は"0123456789"の繰り返しなので、インデックスiの値は(i mod 10)+48
;; (ASCIIコード)になる。

(assert-equal (list 72 101 108 108 111) (subseq (fat16-read-file *ide-device* "/TEST.LSP") 0 5))
(assert-equal 18 (length (fat16-read-file *ide-device* "/TEST.LSP")))

(defglobal fat16-test-big (fat16-read-file *ide-device* "/BIG.TXT"))
(assert-equal 2500 (length fat16-test-big))
(assert-equal 48 (elt fat16-test-big 0))
(assert-equal 55 (elt fat16-test-big 2047))
(assert-equal 56 (elt fat16-test-big 2048))
(assert-equal 57 (elt fat16-test-big 2499))

(assert-equal nil (fat16-read-file *ide-device* "/HELLO.TXT"))

;;; --- FAT16-M3: FATテーブルのクラスタチェイン追跡 ---
;;
;; MakefileのFAT16_DISK_IMGルールが、mkfs.vfat後にホスト側でFATテーブル(1本目)へ
;; 直接dd/printfし、クラスタ10→11→14→終端(0xFFFF)という非連続なチェインを合成
;; している(クラスタ3はTEST.LSPが使用中のため避けている)。

(defglobal fat16-test-bpb-m3 (fat16-read-bpb *ide-device*))

(assert-equal 11 (fat16-fat-entry *ide-device* fat16-test-bpb-m3 10))
(assert-equal 14 (fat16-fat-entry *ide-device* fat16-test-bpb-m3 11))
(assert-equal #xFFFF (fat16-fat-entry *ide-device* fat16-test-bpb-m3 14))

(assert-equal (list 10 11 14) (fat16-cluster-chain *ide-device* fat16-test-bpb-m3 10))
