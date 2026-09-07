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
;; Long_File_Name.txt/This_Is_A_Very_Long_File_Name.txt/SUBDIRの7件のみになるはず。

(assert-equal (list (list "HELLO.TXT" ':file 0) (list "TEST.LSP" ':file 18) (list "BIG.TXT" ':file 1000) (list "WRITE1.TXT" ':file 512)
                     (list "Long_File_Name.txt" ':file 17) (list "This_Is_A_Very_Long_File_Name.txt" ':file 1)
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

(assert-equal #(72 101 108 108 111) (subseq (fat32-read-file *fat32-test-device* "/TEST.LSP") 0 5))

;;; --- FAT32-M10: VFAT Long File Name (LFN) ---

(assert-equal #(108 111 110 103) (subseq (fat32-read-file *fat32-test-device* "/Long_File_Name.txt") 0 4))
(assert-equal #(108 111 110 103) (subseq (fat32-read-file *fat32-test-device* "/long_file_name.txt") 0 4))
(assert-equal 1 (length (fat32-read-file *fat32-test-device* "/This_Is_A_Very_Long_File_Name.txt")))
(assert-equal 18 (length (fat32-read-file *fat32-test-device* "/TEST.LSP")))

(defglobal fat32-test-big (fat32-read-file *fat32-test-device* "/BIG.TXT"))
(assert-equal 1000 (length fat32-test-big))
(assert-equal 48 (elt fat32-test-big 0))
(assert-equal 49 (elt fat32-test-big 511))
(assert-equal 50 (elt fat32-test-big 512))

(assert-equal #(110 101 115 116 101) (subseq (fat32-read-file *fat32-test-device* "/SUBDIR/NESTED.TXT") 0 5))
(assert-equal 19 (length (fat32-read-file *fat32-test-device* "/SUBDIR/NESTED.TXT")))

;;; --- [ファイルI/O]#47(M4): read-into!の境界値テスト ---
;;
;; fat16_test.lispと同じ方針: fat32-test-big(BIG.TXT、1000byte、1クラスタ=
;; 512byte)を正解データとして使い、read-into!が任意のオフセット・長さで
;; 同じ内容を部分的に読めることを確認する。

(defun %fat32-test-resolve-node (device path)
  (let ((bpb (fat32-read-bpb device)))
    (let ((resolved (%fat32-resolve-file device bpb path)))
      (%fat32-find-dir-entry (%fat32-scan-dir-entries device (car resolved)) (cdr resolved)))))

(defglobal fat32-test-big-node (%fat32-test-resolve-node *fat32-test-device* "/BIG.TXT"))

;; 先頭10byte
(let ((buf (create-vector 10 0)))
  (assert-equal 10 (read-into! fat32-test-big-node buf 0 0 10))
  (assert-equal (subseq fat32-test-big 0 10) buf))

;; クラスタ境界(512byte)をまたぐ範囲
(let ((buf (create-vector 10 0)))
  (assert-equal 10 (read-into! fat32-test-big-node buf 0 507 10))
  (assert-equal (subseq fat32-test-big 507 517) buf))

;; ファイルサイズ(1000)ちょうどから読もうとするとEOFで0byte
(let ((buf (create-vector 5 0)))
  (assert-equal 0 (read-into! fat32-test-big-node buf 0 1000 5)))

;; 末尾ちょうど: 995から10byte要求しても実際に読めるのは5byte(EOF)
(let ((buf (create-vector 10 99)))
  (assert-equal 5 (read-into! fat32-test-big-node buf 0 995 10))
  (assert-equal (subseq fat32-test-big 995 1000) (subseq buf 0 5)))

;; 前進シーク後の後退シーク(last-cluster-index/last-cluster-numberキャッシュの
;; 後退分岐の確認)
(let ((buf (create-vector 5 0)))
  (assert-equal 5 (read-into! fat32-test-big-node buf 0 512 5))
  (assert-equal (subseq fat32-test-big 512 517) buf)
  (assert-equal 5 (read-into! fat32-test-big-node buf 0 0 5))
  (assert-equal (subseq fat32-test-big 0 5) buf))

;;; --- FAT32-M6a: 既存ファイルの同クラスタ数上書き ---
;;
;; WRITE1.TXT(Makefile参照、mkfs.vfat作成時に"A"を512回=ちょうど1クラスタ分
;; 書き込んだ書き込みテスト専用ファイル、sectors-per-cluster=1なので1クラスタ=
;; 512byte)に対して同じ1クラスタ以内に収まる別内容を書き込み、読み込みで一致する
;; ことを確認する。他の既存ファイルは読み込み専用のまま変更しないため、書き込みは
;; このファイルにのみ行う。

;; (%fat32-test-make-byte-list n value) : 長さnの、全要素がvalueのfixnumの
;; general-vectorを作るテスト専用ヘルパー。[ファイルI/O]#46(M3)でfat32-write-file/
;; fat32-create-fileの契約がconsリストからvectorへ変更されたのに合わせ、
;; create-vectorを使うよう変更した(関数名は既存の呼び出し箇所を変えずに済むよう
;; そのまま残す)。
(defun %fat32-test-make-byte-list (n value)
  (create-vector n value))

;; 書き込み前の内容確認(念のため)
(assert-equal 512 (length (fat32-read-file *fat32-test-device* "/WRITE1.TXT")))
(assert-equal 65 (elt (fat32-read-file *fat32-test-device* "/WRITE1.TXT") 0))
(assert-equal 65 (elt (fat32-read-file *fat32-test-device* "/WRITE1.TXT") 511))

(defglobal fat32-test-write1-new #(87 82 73 84 69 49 45 78 69 87)) ;; "WRITE1-NEW"

(assert-equal t (if (fat32-write-file *fat32-test-device* "/WRITE1.TXT" fat32-test-write1-new) t nil))
(assert-equal fat32-test-write1-new (fat32-read-file *fat32-test-device* "/WRITE1.TXT"))
(assert-equal (list (list "HELLO.TXT" ':file 0) (list "TEST.LSP" ':file 18) (list "BIG.TXT" ':file 1000) (list "WRITE1.TXT" ':file 10)
                     (list "Long_File_Name.txt" ':file 17) (list "This_Is_A_Very_Long_File_Name.txt" ':file 1)
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
(assert-equal (list (list "HELLO.TXT" ':file 0) (list "TEST.LSP" ':file 18) (list "BIG.TXT" ':file 1000)                      (list "WRITE1.TXT" ':file 513)
                     (list "Long_File_Name.txt" ':file 17) (list "This_Is_A_Very_Long_File_Name.txt" ':file 1)
                     (list "SUBDIR" ':dir 0))
              (fat32-read-dir *fat32-test-device* "/"))

;; クラスタ数が減る書き込み(縮小)は対象外としてnilを返す(現在2クラスタ確保済みの
;; WRITE1.TXTへ、1クラスタで収まる10byteを書こうとする)。直前の(失敗した)呼び出し
;; でデータ/ディレクトリエントリが変更されていないことも確認する。
(assert-equal nil (fat32-write-file *fat32-test-device* "/WRITE1.TXT" fat32-test-write1-new))
(assert-equal fat32-test-write1-2clusters (fat32-read-file *fat32-test-device* "/WRITE1.TXT"))
(assert-equal (list (list "HELLO.TXT" ':file 0) (list "TEST.LSP" ':file 18) (list "BIG.TXT" ':file 1000)                      (list "WRITE1.TXT" ':file 513)
                     (list "Long_File_Name.txt" ':file 17) (list "This_Is_A_Very_Long_File_Name.txt" ':file 1)
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
                     (list "Long_File_Name.txt" ':file 17) (list "This_Is_A_Very_Long_File_Name.txt" ':file 1)
                     (list "SUBDIR" ':dir 0)
                     (list "NEW1.TXT" ':file 0) (list "NEW2.TXT" ':file 100))
              (fat32-read-dir *fat32-test-device* "/"))

;; 同名エントリが既に存在する場合はnil(上書きはfat32-write-fileの役割)。既存の
;; TEST.LSPの内容/一覧が変更されていないことも確認する。
(assert-equal nil (fat32-create-file *fat32-test-device* "/TEST.LSP" fat32-test-new2))
(assert-equal 18 (length (fat32-read-file *fat32-test-device* "/TEST.LSP")))
(assert-equal (list (list "HELLO.TXT" ':file 0) (list "TEST.LSP" ':file 18) (list "BIG.TXT" ':file 1000) (list "WRITE1.TXT" ':file 513)
                     (list "Long_File_Name.txt" ':file 17) (list "This_Is_A_Very_Long_File_Name.txt" ':file 1)
                     (list "SUBDIR" ':dir 0)
                     (list "NEW1.TXT" ':file 0) (list "NEW2.TXT" ':file 100))
              (fat32-read-dir *fat32-test-device* "/"))

;; LFN対応後は8.3に収まらない名前でも作成できる。
(defglobal fat32-test-lfn-create (%fat32-test-make-byte-list 20 76)) ;; 全要素76('L')

(assert-equal t (if (fat32-create-file *fat32-test-device* "/A.B.C" fat32-test-lfn-create) t nil))
(assert-equal fat32-test-lfn-create (fat32-read-file *fat32-test-device* "/A.B.C"))
(assert-equal t (if (fat32-create-file *fat32-test-device* "/TOOLONGNAME.TXT" fat32-test-lfn-create) t nil))
(assert-equal fat32-test-lfn-create (fat32-read-file *fat32-test-device* "/TOOLONGNAME.TXT"))
(assert-equal (list (list "HELLO.TXT" ':file 0) (list "TEST.LSP" ':file 18) (list "BIG.TXT" ':file 1000) (list "WRITE1.TXT" ':file 513)
                     (list "Long_File_Name.txt" ':file 17) (list "This_Is_A_Very_Long_File_Name.txt" ':file 1)
                     (list "SUBDIR" ':dir 0)
                     (list "NEW1.TXT" ':file 0) (list "NEW2.TXT" ':file 100)
                     (list "A.B.C" ':file 20) (list "TOOLONGNAME.TXT" ':file 20))
              (fat32-read-dir *fat32-test-device* "/"))

;; サブディレクトリ内への書き込み・新規作成(多階層パス解決の回帰確認)
(defglobal fat32-test-nested-new #(78 69 83 84 69 68 45 78 69 87)) ;; "NESTED-NEW"

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
                     (list "Long_File_Name.txt" ':file 17) (list "This_Is_A_Very_Long_File_Name.txt" ':file 1)
                     (list "SUBDIR" ':dir 0)
                     (list "NEW1.TXT" ':file 0) (list "NEW2.TXT" ':file 100)
                     (list "A.B.C" ':file 20) (list "TOOLONGNAME.TXT" ':file 20)
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

;; LFN名でのディレクトリ作成
(assert-equal t (if (fat32-create-directory *fat32-test-device* "/Long_Directory_Name") t nil))
(assert-equal (list (list "." ':dir 0) (list ".." ':dir 0))
              (fat32-read-dir *fat32-test-device* "/Long_Directory_Name"))
(assert-equal t (if (fat32-create-directory *fat32-test-device* "/MULTI.PART.NAME") t nil))
(assert-equal (list (list "." ':dir 0) (list ".." ':dir 0))
              (fat32-read-dir *fat32-test-device* "/MULTI.PART.NAME"))

;; 存在しない親ディレクトリの下へのmkdirもnil
(assert-equal nil (fat32-create-directory *fat32-test-device* "/NOSUCHDIR/CHILD"))

;;; --- [ファイルI/O]#48(M5): write-from!の境界値テスト ---
;;
;; 他のテストの状態に影響しないよう専用の新規ファイルを使う。FAT32の
;; このディスクはsectors-per-cluster=1なのでクラスタサイズ=セクタサイズ=512byte
;; (fat32_test.lisp冒頭のBPBアサーション参照)。
;;
;; #47(read-into!)と同じ原因(*generic-methods*がinit.lispのdefdynamicで
;; 上書き消去され、総称関数が「no applicable method」で無反応スキップになる
;; 問題、詳細はfat16_test.lispのコメント参照)により、以前は実際のディスク
;; 書き込み内容を検証できていなかった。原因を修正したので、通常通り書き込み後の
;; 内容をfat32-read-fileで読み直して検証する。

;; 1クラスタに収まる小さいファイルの一部を書き換える
(assert-equal t (if (fat32-create-file *fat32-test-device* "/M5SMALL.TXT" (create-vector 10 65)) t nil)) ;; 全byte'A'
(defglobal fat32-test-small-node (%fat32-test-resolve-node *fat32-test-device* "/M5SMALL.TXT"))
(assert-equal t (write-from! fat32-test-small-node (create-vector 3 66) 0 4 3)) ;; オフセット4から3byte'B'
(defglobal fat32-test-small-after (fat32-read-file *fat32-test-device* "/M5SMALL.TXT"))
(assert-equal 10 (length fat32-test-small-after))
(assert-equal #(65 65 65 65 66 66 66 65 65 65) fat32-test-small-after)
(assert-equal 10 (slot-value fat32-test-small-node 'size))

;; ファイル末尾を越える範囲への書き込み(ファイルサイズの拡張、既存クラスタ内)
(assert-equal t (if (fat32-create-file *fat32-test-device* "/M5EXTEND.TXT" (create-vector 10 65)) t nil))
(defglobal fat32-test-extend-node (%fat32-test-resolve-node *fat32-test-device* "/M5EXTEND.TXT"))
(assert-equal t (write-from! fat32-test-extend-node (create-vector 5 67) 0 8 5)) ;; オフセット8から5byte'C'→サイズ13に拡張
(defglobal fat32-test-extend-after (fat32-read-file *fat32-test-device* "/M5EXTEND.TXT"))
(assert-equal 13 (length fat32-test-extend-after))
(assert-equal #(65 65 65 65 65 65 65 65 67 67 67 67 67) fat32-test-extend-after)
(assert-equal 13 (slot-value fat32-test-extend-node 'size))

;; 複数クラスタにまたがる書き込み(sectors-per-cluster=1なのでクラスタ境界=512byte)
(assert-equal t (if (fat32-create-file *fat32-test-device* "/M5MULTI.TXT" (create-vector 512 65)) t nil))
(defglobal fat32-test-multi-node (%fat32-test-resolve-node *fat32-test-device* "/M5MULTI.TXT"))
(assert-equal t (write-from! fat32-test-multi-node (create-vector 10 68) 0 507 10)) ;; オフセット507から10byte'D'(507-516、クラスタ境界512をまたぐ)
(defglobal fat32-test-multi-after (fat32-read-file *fat32-test-device* "/M5MULTI.TXT"))
(assert-equal 517 (length fat32-test-multi-after)) ;; 507+10=517へ拡張
(assert-equal 65 (elt fat32-test-multi-after 506))
(assert-equal 68 (elt fat32-test-multi-after 507))
(assert-equal 68 (elt fat32-test-multi-after 511))
(assert-equal 68 (elt fat32-test-multi-after 512))
(assert-equal 68 (elt fat32-test-multi-after 516))

;; 新規クラスタ確保を伴う追記(既存チェイン長を超えるオフセットへの書き込み)
(assert-equal t (if (fat32-create-file *fat32-test-device* "/M5APPEND.TXT" (create-vector 512 65)) t nil)) ;; ちょうど1クラスタ
(defglobal fat32-test-append-node (%fat32-test-resolve-node *fat32-test-device* "/M5APPEND.TXT"))
(assert-equal t (write-from! fat32-test-append-node (create-vector 5 69) 0 512 5)) ;; 2クラスタ目に新規書き込み
(defglobal fat32-test-append-after (fat32-read-file *fat32-test-device* "/M5APPEND.TXT"))
(assert-equal 517 (length fat32-test-append-after))
(assert-equal 65 (elt fat32-test-append-after 511))
(assert-equal 69 (elt fat32-test-append-after 512))
(assert-equal 69 (elt fat32-test-append-after 516))

;;; --- [ファイルI/O]#49(M6): OPEN-OUTPUT-FILE/OPEN-IO-FILEのストリーミングI/O(FAT32) ---
;; fat16_test.lispのBIGWRITEテストと同じ検証をFAT32側でも行う(M6コミット時に
;; FAT16側でしか検証していなかったのを補う)。sectors-per-cluster=1(512byte
;; クラスタ)のため、10000byteの書き込みだけでも約20クラスタの新規確保を伴い、
;; self_handleのGC再配置漏れ(#49で発見・修正済み)のようなクラスタ拡張時の
;; バグを十分に踏める規模になる。
(mount "/mnt" 'blk0 ':fat32)

(defun %%fat32-test-write-n-chars (stream ch n)
  (let ((i 0))
    (while (< i n)
      (progn
        (write-char ch stream)
        (setq i (+ i 1))))))

(defglobal fat32-test-bigwrite-len 10000)
(defglobal fat32-test-bigwrite-stream (open-output-file "/mnt/BIGWR.TXT"))
(assert-equal t (if fat32-test-bigwrite-stream t nil))
(%%fat32-test-write-n-chars fat32-test-bigwrite-stream #\A fat32-test-bigwrite-len)
(close fat32-test-bigwrite-stream)

(defglobal fat32-test-bigwrite-after (fat32-read-file *fat32-test-device* "/BIGWR.TXT"))
(assert-equal t (if fat32-test-bigwrite-after t nil))
(assert-equal fat32-test-bigwrite-len (length fat32-test-bigwrite-after))
(assert-equal 65 (elt fat32-test-bigwrite-after 0))
(assert-equal 65 (elt fat32-test-bigwrite-after 4999))
(assert-equal 65 (elt fat32-test-bigwrite-after 9999))

;; OPEN-IO-FILE: 既存ファイルに対してtruncateしない(FAT16側と同じ確認)。
(defglobal fat32-test-bigio-stream (open-io-file "/mnt/BIGWR.TXT"))
(assert-equal t (if fat32-test-bigio-stream t nil))
(defglobal fat32-test-bigio-first-char (read-char fat32-test-bigio-stream))
(assert-equal #\A fat32-test-bigio-first-char)
(set-file-position fat32-test-bigio-stream 5000)
(write-char #\Z fat32-test-bigio-stream)
(close fat32-test-bigio-stream)

(defglobal fat32-test-bigio-after (fat32-read-file *fat32-test-device* "/BIGWR.TXT"))
(assert-equal fat32-test-bigwrite-len (length fat32-test-bigio-after))
(assert-equal 65 (elt fat32-test-bigio-after 4999))
(assert-equal 90 (elt fat32-test-bigio-after 5000))
(assert-equal 65 (elt fat32-test-bigio-after 5001))

;;; --- [ファイルI/O]#50(M7): file-length高速パス + read-file-into-vector/write-vector-to-file(FAT32) ---

(defglobal fat32-test-filesize-t0 (get-internal-real-time))
(defglobal fat32-test-filesize-result (fat32-file-size *fat32-test-device* "/BIGWR.TXT"))
(defglobal fat32-test-filesize-t1 (get-internal-real-time))
(assert-equal 10000 fat32-test-filesize-result)

(defglobal fat32-test-readfile-t0 (get-internal-real-time))
(defglobal fat32-test-readfile-result (fat32-read-file *fat32-test-device* "/BIGWR.TXT"))
(defglobal fat32-test-readfile-t1 (get-internal-real-time))
(assert-equal 10000 (length fat32-test-readfile-result))

(assert-equal t (<= (- fat32-test-filesize-t1 fat32-test-filesize-t0)
                     (- fat32-test-readfile-t1 fat32-test-readfile-t0)))

(assert-equal 10000 (file-length "/mnt/BIGWR.TXT"))

(defglobal fat32-test-rfitv-vec (create-vector 300 0))
(defglobal fat32-test-rfitv-fill-i 0)
(while (< fat32-test-rfitv-fill-i 300)
  (progn
    (set-elt (mod fat32-test-rfitv-fill-i 256) fat32-test-rfitv-vec fat32-test-rfitv-fill-i)
    (setq fat32-test-rfitv-fill-i (+ fat32-test-rfitv-fill-i 1))))
(assert-equal t (if (write-vector-to-file "/mnt/RFITV.BIN" fat32-test-rfitv-vec) t nil))
(defglobal fat32-test-rfitv-readback (read-file-into-vector "/mnt/RFITV.BIN"))
(assert-equal t (if fat32-test-rfitv-readback t nil))
(assert-equal fat32-test-rfitv-vec fat32-test-rfitv-readback)
(assert-equal nil (read-file-into-vector "/mnt/NO-SUCH-FILE.BIN"))
