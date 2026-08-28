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
;; TEST.LSP(18byte)→DELETED.TXT(作成後にrm、先頭バイトが0xE5になる)→
;; 残りは0x00の空き終端、という並びになっている(Makefileのmkfs.vfat手順を参照)。
;; 削除済みエントリはスキップされ、走査は0x00終端で止まるため、戻り値は
;; HELLO.TXTとTEST.LSPの2件のみになるはず。

(assert-equal (list (list "HELLO.TXT" ':file 0) (list "TEST.LSP" ':file 18))
              (fat16-read-dir *ide-device* "/"))
