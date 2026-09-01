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

;;; --- device.lispのFAT32 UUID検出(describe用) ---
;;
;; FAT32フォーマット済みディスクのため、blk0のUUIDはXXXX-XXXX形式で検出できる
;; (device_test.lispのFAT16版アサーションと同型)。逆に%device-fat16-uuidは
;; このディスクに対してはFAT16署名が一致せずnilを返すはず。

(defglobal fat32-test-uuid32 (%device-fat32-uuid *fat32-test-device*))

(assert-equal t (if fat32-test-uuid32 t nil))
(assert-equal 9 (length fat32-test-uuid32))
(assert-equal 45 (char-code (string-elt fat32-test-uuid32 4))) ; '-'

(assert-equal nil (%device-fat16-uuid *fat32-test-device*))

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

;;; --- FAT32-M6a: 既存ファイルの同クラスタ数上書き ---
;;
;; WRITE1.TXT(Makefile参照、mkfs.vfat作成時に"A"を512回=ちょうど1クラスタ分
;; 書き込んだ書き込みテスト専用ファイル、sectors-per-cluster=1なので1クラスタ=
;; 512byte)に対して同じ1クラスタ以内に収まる別内容を書き込み、読み込みで一致する
;; ことを確認する。他の既存ファイルは読み込み専用のまま変更しないため、書き込みは
;; このファイルにのみ行う。

;; (%fat32-test-make-byte-list n value) : 長さnの、全要素がvalueのfixnumリストを
;; 作るテスト専用ヘルパー。再帰は使わずwhileで組み立てる(nがファイルサイズに
;; 比例して大きくなり得るため)。
(defun %fat32-test-make-byte-list (n value)
  (let ((i 0) (acc nil))
    (while (< i n)
      (setq acc (cons value acc))
      (setq i (+ i 1)))
    acc))

;; 書き込み前の内容確認(念のため)
(assert-equal 512 (length (fat32-read-file *fat32-test-device* "/WRITE1.TXT")))
(assert-equal 65 (elt (fat32-read-file *fat32-test-device* "/WRITE1.TXT") 0))
(assert-equal 65 (elt (fat32-read-file *fat32-test-device* "/WRITE1.TXT") 511))

(defglobal fat32-test-write1-new (list 87 82 73 84 69 49 45 78 69 87)) ;; "WRITE1-NEW"

(assert-equal t (if (fat32-write-file *fat32-test-device* "/WRITE1.TXT" fat32-test-write1-new) t nil))
(assert-equal fat32-test-write1-new (fat32-read-file *fat32-test-device* "/WRITE1.TXT"))
(assert-equal (list (list "HELLO.TXT" ':file 0) (list "TEST.LSP" ':file 18) (list "BIG.TXT" ':file 1000) (list "WRITE1.TXT" ':file 10)
                     (list "SUBDIR" ':dir 0))
              (fat32-read-dir *fat32-test-device* "/"))

;;; --- FAT32-M6b: クラスタ追加を伴うファイル拡張 ---
;;
;; WRITE1.TXTは直前のM6a確認時点で1クラスタ(512byte以内)のまま。513byteは1
;; クラスタを超えるため2クラスタ必要になり、現在の1クラスタと不一致になる。
;; 必要クラスタ数が増える場合は新規クラスタを確保して拡張書き込みが成功する。

(defglobal fat32-test-write1-2clusters (%fat32-test-make-byte-list 513 66))

(assert-equal t (if (fat32-write-file *fat32-test-device* "/WRITE1.TXT" fat32-test-write1-2clusters) t nil))
(assert-equal fat32-test-write1-2clusters (fat32-read-file *fat32-test-device* "/WRITE1.TXT"))
(assert-equal (list (list "HELLO.TXT" ':file 0) (list "TEST.LSP" ':file 18) (list "BIG.TXT" ':file 1000) (list "WRITE1.TXT" ':file 513)
                     (list "SUBDIR" ':dir 0))
              (fat32-read-dir *fat32-test-device* "/"))

;; クラスタ数が減る書き込み(縮小)は対象外としてnilを返す(現在2クラスタ確保済みの
;; WRITE1.TXTへ、1クラスタで収まる10byteを書こうとする)。直前の(失敗した)呼び出し
;; でデータ/ディレクトリエントリが変更されていないことも確認する。
(assert-equal nil (fat32-write-file *fat32-test-device* "/WRITE1.TXT" fat32-test-write1-new))
(assert-equal fat32-test-write1-2clusters (fat32-read-file *fat32-test-device* "/WRITE1.TXT"))
(assert-equal (list (list "HELLO.TXT" ':file 0) (list "TEST.LSP" ':file 18) (list "BIG.TXT" ':file 1000) (list "WRITE1.TXT" ':file 513)
                     (list "SUBDIR" ':dir 0))
              (fat32-read-dir *fat32-test-device* "/"))

;;; --- FAT32-M6c: 新規ファイル作成 ---
;;
;; Makefileのmkfs.vfat手順でDELETED.TXTを最後に作成後rmしているため、この時点で
;; ルートディレクトリにはWRITE1.TXT/SUBDIRの直後に0xE5(削除済み再利用可)スロットが
;; 1つ、その直後に終端(0x00)スロットが続く並びになっている。fat32-create-fileは
;; 最初に見つかった空きスロットへ書き込むため、1つ目の新規ファイルはDELETED.TXT
;; だったスロットを再利用し、2つ目は新しい終端スロットに入る。

;; 空ファイルの新規作成(クラスタ確保なし、start-cluster=0/size=0)
(assert-equal t (if (fat32-create-file *fat32-test-device* "/NEW1.TXT" nil) t nil))
(assert-equal nil (fat32-read-file *fat32-test-device* "/NEW1.TXT"))

;; 1クラスタに収まる非空ファイルの新規作成
(defglobal fat32-test-new2 (%fat32-test-make-byte-list 100 67)) ;; 全要素67('C')

(assert-equal t (if (fat32-create-file *fat32-test-device* "/NEW2.TXT" fat32-test-new2) t nil))
(assert-equal fat32-test-new2 (fat32-read-file *fat32-test-device* "/NEW2.TXT"))

(assert-equal (list (list "HELLO.TXT" ':file 0) (list "TEST.LSP" ':file 18) (list "BIG.TXT" ':file 1000) (list "WRITE1.TXT" ':file 513)
                     (list "SUBDIR" ':dir 0)
                     (list "NEW1.TXT" ':file 0) (list "NEW2.TXT" ':file 100))
              (fat32-read-dir *fat32-test-device* "/"))

;; 同名エントリが既に存在する場合はnil(上書きはfat32-write-fileの役割)。既存の
;; TEST.LSPの内容/一覧が変更されていないことも確認する。
(assert-equal nil (fat32-create-file *fat32-test-device* "/TEST.LSP" fat32-test-new2))
(assert-equal 18 (length (fat32-read-file *fat32-test-device* "/TEST.LSP")))
(assert-equal (list (list "HELLO.TXT" ':file 0) (list "TEST.LSP" ':file 18) (list "BIG.TXT" ':file 1000) (list "WRITE1.TXT" ':file 513)
                     (list "SUBDIR" ':dir 0)
                     (list "NEW1.TXT" ':file 0) (list "NEW2.TXT" ':file 100))
              (fat32-read-dir *fat32-test-device* "/"))

;; 8.3形式で表現できない名前(複数ドット、またはbase/extが長すぎる)はnil。いずれも
;; スロット確保より前段の名前変換で失敗するため、一覧は変化しない。
(assert-equal nil (fat32-create-file *fat32-test-device* "/A.B.C" fat32-test-new2))
(assert-equal nil (fat32-create-file *fat32-test-device* "/TOOLONGNAME.TXT" fat32-test-new2))
(assert-equal (list (list "HELLO.TXT" ':file 0) (list "TEST.LSP" ':file 18) (list "BIG.TXT" ':file 1000) (list "WRITE1.TXT" ':file 513)
                     (list "SUBDIR" ':dir 0)
                     (list "NEW1.TXT" ':file 0) (list "NEW2.TXT" ':file 100))
              (fat32-read-dir *fat32-test-device* "/"))

;; サブディレクトリ内への書き込み・新規作成(多階層パス解決の回帰確認)
(defglobal fat32-test-nested-new (list 78 69 83 84 69 68 45 78 69 87)) ;; "NESTED-NEW"

(assert-equal t (if (fat32-write-file *fat32-test-device* "/SUBDIR/NESTED.TXT" fat32-test-nested-new) t nil))
(assert-equal fat32-test-nested-new (fat32-read-file *fat32-test-device* "/SUBDIR/NESTED.TXT"))
(assert-equal (list (list "." ':dir 0) (list ".." ':dir 0) (list "NESTED.TXT" ':file 10) (list "DEEPER" ':dir 0))
              (fat32-read-dir *fat32-test-device* "/SUBDIR"))

(defglobal fat32-test-subdir-new3 (%fat32-test-make-byte-list 50 68)) ;; 全要素68('D')

(assert-equal t (if (fat32-create-file *fat32-test-device* "/SUBDIR/NEW3.TXT" fat32-test-subdir-new3) t nil))
(assert-equal fat32-test-subdir-new3 (fat32-read-file *fat32-test-device* "/SUBDIR/NEW3.TXT"))
(assert-equal (list (list "." ':dir 0) (list ".." ':dir 0) (list "NESTED.TXT" ':file 10) (list "DEEPER" ':dir 0)
                     (list "NEW3.TXT" ':file 50))
              (fat32-read-dir *fat32-test-device* "/SUBDIR"))

;; 存在しないディレクトリの下への書き込み・新規作成はいずれもnil
(assert-equal nil (fat32-write-file *fat32-test-device* "/NOSUCHDIR/X.TXT" fat32-test-subdir-new3))
(assert-equal nil (fat32-create-file *fat32-test-device* "/NOSUCHDIR/X.TXT" fat32-test-subdir-new3))

;; 2階層下(/SUBDIR/DEEPER)への新規作成も往復一致することを確認する
(assert-equal t (if (fat32-create-file *fat32-test-device* "/SUBDIR/DEEPER/NEW4.TXT" fat32-test-subdir-new3) t nil))
(assert-equal fat32-test-subdir-new3 (fat32-read-file *fat32-test-device* "/SUBDIR/DEEPER/NEW4.TXT"))

;;; --- FAT32-M7: fat32-create-directory(mkdir) ---
;;
;; fat32-create-directoryでルート直下・サブディレクトリ配下それぞれに新規
;; ディレクトリを作成し、作成したディレクトリの中でfat32-create-file/
;; fat32-read-fileが正常に動くこと(mkdir→create→readのライフサイクル)、
;; 同名重複・8.3非対応名・存在しない親ディレクトリの失敗ケースを確認する。
;; FAT16と異なり".."がルートを指す場合でも0のような特別値ではなく実際の
;; root-clusterを書き込む設計だが、fat32-read-dirの一覧にはstart-clusterが
;; 出ないため(FAT16テストと同じ理由)、ここでは名前・種別・sizeのみ確認する。

;; ルート直下への新規ディレクトリ作成
(assert-equal t (if (fat32-create-directory *fat32-test-device* "/NEWDIR") t nil))
(assert-equal (list (list "HELLO.TXT" ':file 0) (list "TEST.LSP" ':file 18) (list "BIG.TXT" ':file 1000) (list "WRITE1.TXT" ':file 513)
                     (list "SUBDIR" ':dir 0)
                     (list "NEW1.TXT" ':file 0) (list "NEW2.TXT" ':file 100)
                     (list "NEWDIR" ':dir 0))
              (fat32-read-dir *fat32-test-device* "/"))

;; 作成したディレクトリの内側は"."/".."のみ
(assert-equal (list (list "." ':dir 0) (list ".." ':dir 0))
              (fat32-read-dir *fat32-test-device* "/NEWDIR"))

;; 作成したディレクトリの中への新規ファイル作成→読み込みの往復一致
(defglobal fat32-test-inside (%fat32-test-make-byte-list 30 69)) ;; 全要素69('E')

(assert-equal t (if (fat32-create-file *fat32-test-device* "/NEWDIR/INSIDE.TXT" fat32-test-inside) t nil))
(assert-equal fat32-test-inside (fat32-read-file *fat32-test-device* "/NEWDIR/INSIDE.TXT"))
(assert-equal (list (list "." ':dir 0) (list ".." ':dir 0) (list "INSIDE.TXT" ':file 30))
              (fat32-read-dir *fat32-test-device* "/NEWDIR"))

;; 既存サブディレクトリ(/SUBDIR)配下への新規ディレクトリ作成
(assert-equal t (if (fat32-create-directory *fat32-test-device* "/SUBDIR/NEWSUB") t nil))
(assert-equal (list (list "." ':dir 0) (list ".." ':dir 0))
              (fat32-read-dir *fat32-test-device* "/SUBDIR/NEWSUB"))
(assert-equal (list (list "." ':dir 0) (list ".." ':dir 0) (list "NESTED.TXT" ':file 10) (list "DEEPER" ':dir 0)
                     (list "NEW3.TXT" ':file 50)
                     (list "NEWSUB" ':dir 0))
              (fat32-read-dir *fat32-test-device* "/SUBDIR"))

;; 同名エントリが既に存在する場合はnil(親ディレクトリの一覧は変化しない)
(assert-equal nil (fat32-create-directory *fat32-test-device* "/SUBDIR"))
(assert-equal nil (fat32-create-directory *fat32-test-device* "/NEWDIR"))

;; 8.3形式で表現できない名前はnil
(assert-equal nil (fat32-create-directory *fat32-test-device* "/A.B.C"))
(assert-equal nil (fat32-create-directory *fat32-test-device* "/TOOLONGDIRNAME"))

;; 存在しない親ディレクトリの下へのmkdirもnil
(assert-equal nil (fat32-create-directory *fat32-test-device* "/NOSUCHDIR/CHILD"))
