;; test/lisp/fat16_test.lisp
;;
;; src/lisp/fat16.lisp(FAT16-M1: BPBパース)の動作確認。
;; QEMU側はMakefileの$(FAT16_DISK_IMG)ターゲット(mkfs.vfat -F 16でフォーマットした
;; 16MBイメージ)をQEMU_DISK_IMG経由でbus=1,unit=0にアタッチしている。
;; 期待値はホスト上でxxd -g1 tmp/fat16_test.imgのセクタ0を目視確認して得たもの
;; (mkfs.fat 4.2、16MBイメージのデフォルト値)。
;;
;; 本ファイルはtest/lisp/test_framework.lisp(assert-equal)とsrc/lisp/device.lisp+
;; src/lisp/ide.lisp(*devices*)とsrc/lisp/fat16.lisp(fat16-read-bpb)を、本ファイルより
;; 先にboot-entryスクリプトがloadしている前提で書かれている。

;; *devices*からblk0(Secondary IDEチャネルのデバイス)のハンドルを取り出す。
;; *test-device*のような単一デバイス専用グローバルは廃止し、*devices*経由に統一する。
(defglobal *test-device* (%device-handle 'blk0))

(defglobal fat16-test-bpb (fat16-read-bpb *test-device*))

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
;; TEST.LSP(18byte)→BIG.TXT(2500byte)→WRITE1.TXT(2048byte、FAT16-M6書き込み
;; テスト専用)→SUBDIR(FAT16-M7サブディレクトリテスト専用、中にNESTED.TXT/
;; DEEPER/DEEP.TXTを持つ)→DELETED.TXT(最後に作成後にrm、先頭バイトが0xE5になる)
;; →残りは0x00の空き終端、という並びになっている(Makefileのmkfs.vfat手順を参照。
;; DELETED.TXTを最後に作る/消すのは、それより前に作るとカーネルのvfatドライバが
;; 後続ファイル作成時に空いた0xE5スロットを再利用してしまうため)。削除済み
;; エントリはスキップされ、走査は0x00終端で止まるため、戻り値はHELLO.TXT/
;; TEST.LSP/BIG.TXT/WRITE1.TXT/SUBDIRの5件のみになるはず。

(assert-equal (list (list "HELLO.TXT" ':file 0) (list "TEST.LSP" ':file 18) (list "BIG.TXT" ':file 2500) (list "WRITE1.TXT" ':file 2048)
                     (list "SUBDIR" ':dir 0))
              (fat16-read-dir *test-device* "/"))

;;; --- FAT16-M4: クラスタ→セクタ変換とファイル本体読み込み ---
;;
;; TEST.LSPは単一クラスタ(3、終端)、BIG.TXTは2クラスタ(4→5→終端)に分割されている
;; (Makefile参照、クラスタサイズは4セクタ*512byte=2048byte)。fat16-read-fileが
;; クラスタ境界を跨いだ読み込みを正しく行えることを、2500byte全体ではなく代表点
;; (先頭・1クラスタ目末尾・2クラスタ目先頭・末尾)の値で確認する(全体を
;; assert-equalで比較すると再帰の深さが大きくなるため)。
;; BIG.TXTの内容は"0123456789"の繰り返しなので、インデックスiの値は(i mod 10)+48
;; (ASCIIコード)になる。

(assert-equal #(72 101 108 108 111) (subseq (fat16-read-file *test-device* "/TEST.LSP") 0 5))
(assert-equal 18 (length (fat16-read-file *test-device* "/TEST.LSP")))

(defglobal fat16-test-big (fat16-read-file *test-device* "/BIG.TXT"))
(assert-equal 2500 (length fat16-test-big))
(assert-equal 48 (elt fat16-test-big 0))
(assert-equal 55 (elt fat16-test-big 2047))
(assert-equal 56 (elt fat16-test-big 2048))
(assert-equal 57 (elt fat16-test-big 2499))

(assert-equal nil (fat16-read-file *test-device* "/HELLO.TXT"))

;;; --- [ファイルI/O]#47(M4): read-into!の境界値テスト ---
;;
;; fat16-test-big(直前のfat16-read-file経由で読み込み済みのBIG.TXT全内容、
;; 既にテスト済み)を正解データとして使い、read-into!が任意のオフセット・長さで
;; 同じ内容を部分的に読めることを確認する(期待値を手計算せず、既存の信頼できる
;; データから導くことで算術ミスを避ける)。
;;
;; #48で判明した根本原因(修正済み): write-from!/read-into!等、AOTファイル
;; (file-node.lisp/fat16.lisp/fat32.lisp)側のdefmethodがos_run_aot_toplevel_forms
;; 実行中に%register-methodへ登録した内容が、その後にロードされるinit.lispの
;; (defdynamic *generic-methods* nil)によって無条件に上書き消去され、実行時には
;; 総称関数の呼び出し先が1件も見つからない状態になっていた。%generic-callが
;; 「no applicable method」でerrorを呼び、ハンドラが無いためsignal-conditionが
;; トップレベルへの非局所脱出(return-from %top-level)を起こし、read-into!/
;; write-from!呼び出しを含むトップレベルform自体が(pass/failいずれのカウントも
;; 増えずに)評価途中で中断されていた。*generic-methods*/*next-methods*の
;; defdynamicをinit_aot.lisp側(%register-method定義の直前、*classes*と同じ
;; 位置)へ移動し、fs-lisp-paths側のdefmethod登録より確実に先に初期化される
;; ようにして解決した。

;; パス解決だけを行いnode(<fat16-file-node>)を取得するテスト専用ヘルパー。
;; fat16-read-fileと同じ解決ロジック(%fat16-resolve-file/%fat16-scan-dir-entries/
;; %fat16-find-dir-entry)をそのまま使う。
(defun %fat16-test-resolve-node (device path)
  (let ((bpb (fat16-read-bpb device)))
    (let ((resolved (%fat16-resolve-file device bpb path)))
      (%fat16-find-dir-entry (%fat16-scan-dir-entries device (car resolved)) (cdr resolved)))))

(defglobal fat16-test-big-node (%fat16-test-resolve-node *test-device* "/BIG.TXT"))

;; 先頭10byte
(let ((buf (create-vector 10 0)))
  (assert-equal 10 (read-into! fat16-test-big-node buf 0 0 10))
  (assert-equal (subseq fat16-test-big 0 10) buf))

;; クラスタ境界(2048byte、Makefile参照)をまたぐ範囲
(let ((buf (create-vector 10 0)))
  (assert-equal 10 (read-into! fat16-test-big-node buf 0 2043 10))
  (assert-equal (subseq fat16-test-big 2043 2053) buf))

;; ファイルサイズ(2500)ちょうどから読もうとするとEOFで0byte
(let ((buf (create-vector 5 0)))
  (assert-equal 0 (read-into! fat16-test-big-node buf 0 2500 5)))

;; ファイルサイズ+1から読もうとしても0byte
(let ((buf (create-vector 5 0)))
  (assert-equal 0 (read-into! fat16-test-big-node buf 0 2501 5)))

;; 末尾ちょうど: 2495から10byte要求しても実際に読めるのは5byte(EOF)
(let ((buf (create-vector 10 99)))
  (assert-equal 5 (read-into! fat16-test-big-node buf 0 2495 10))
  (assert-equal (subseq fat16-test-big 2495 2500) (subseq buf 0 5)))

;; buffer-offset(書き込み先の途中位置)指定
(let ((buf (create-vector 8 0)))
  (assert-equal 3 (read-into! fat16-test-big-node buf 2 100 3))
  (assert-equal (subseq fat16-test-big 100 103) (subseq buf 2 5)))

;; 前進シーク後の後退シーク(<file-node>のlast-cluster-index/last-cluster-number
;; キャッシュが後退時にstart-clusterから正しく辿り直すことを確認する)
(let ((buf (create-vector 5 0)))
  (assert-equal 5 (read-into! fat16-test-big-node buf 0 2048 5)) ;; 前進(クラスタ2)
  (assert-equal (subseq fat16-test-big 2048 2053) buf)
  (assert-equal 5 (read-into! fat16-test-big-node buf 0 0 5))    ;; 後退(クラスタ1)
  (assert-equal (subseq fat16-test-big 0 5) buf))

;;; --- FAT16-M3: FATテーブルのクラスタチェイン追跡 ---
;;
;; MakefileのFAT16_DISK_IMGルールが、mkfs.vfat後にホスト側でFATテーブル(1本目)へ
;; 直接dd/printfし、クラスタ40→41→44→終端(0xFFFF)という非連続なチェインを合成
;; している(クラスタ40/41/44は他の実ファイル/ディレクトリが使う番号帯(3〜9番台)
;; から十分離しており衝突しない。FAT16-M7aでSUBDIR以下を追加した際、元は
;; 10/11/14だったが実クラスタ使用と衝突するため40/41/44へ変更した)。

(defglobal fat16-test-bpb-m3 (fat16-read-bpb *test-device*))

(assert-equal 41 (fat16-fat-entry *test-device* fat16-test-bpb-m3 40))
(assert-equal 44 (fat16-fat-entry *test-device* fat16-test-bpb-m3 41))
(assert-equal #xFFFF (fat16-fat-entry *test-device* fat16-test-bpb-m3 44))

(assert-equal (list 40 41 44) (fat16-cluster-chain *test-device* fat16-test-bpb-m3 40))

;;; --- FAT16-M6a: 既存ファイルの同クラスタ数上書き ---
;;
;; WRITE1.TXT(Makefile参照、mkfs.vfat作成時に"A"を2048回=ちょうど1クラスタ分
;; 書き込んだ書き込みテスト専用ファイル)に対して同じ1クラスタ以内に収まる別内容を
;; 書き込み、読み込みで一致することを確認する。他の既存ファイル(TEST.LSP/BIG.TXT/
;; HELLO.TXT)は読み込み専用のまま変更しないため、書き込みはこのファイルにのみ行う。

;; (%fat16-test-make-byte-list n value) : 長さnの、全要素がvalueのfixnumの
;; general-vectorを作るテスト専用ヘルパー。[ファイルI/O]#46(M3)でfat16-write-file/
;; fat16-create-fileの契約がconsリストからvectorへ変更されたのに合わせ、
;; create-vector(第二引数で全要素を初期化できる既存プリミティブ)を使うよう変更
;; した(関数名は既存の呼び出し箇所を変えずに済むようそのまま残す)。
(defun %fat16-test-make-byte-list (n value)
  (create-vector n value))

;; 書き込み前の内容確認(念のため)
(assert-equal 2048 (length (fat16-read-file *test-device* "/WRITE1.TXT")))
(assert-equal 65 (elt (fat16-read-file *test-device* "/WRITE1.TXT") 0))
(assert-equal 65 (elt (fat16-read-file *test-device* "/WRITE1.TXT") 2047))

(defglobal fat16-test-write1-new #(87 82 73 84 69 49 45 78 69 87)) ;; "WRITE1-NEW"

(assert-equal t (if (fat16-write-file *test-device* "/WRITE1.TXT" fat16-test-write1-new) t nil))
(assert-equal fat16-test-write1-new (fat16-read-file *test-device* "/WRITE1.TXT"))
(assert-equal (list (list "HELLO.TXT" ':file 0) (list "TEST.LSP" ':file 18) (list "BIG.TXT" ':file 2500) (list "WRITE1.TXT" ':file 10)
                     (list "SUBDIR" ':dir 0))
              (fat16-read-dir *test-device* "/"))

;;; --- FAT16-M6b: クラスタ追加を伴うファイル拡張 ---
;;
;; WRITE1.TXTは直前のM6a確認時点で1クラスタ(2048byte以内)のまま。2049byteは
;; 1クラスタを超えるため2クラスタ必要になり、現在の1クラスタと不一致になる。
;; FAT16-M6b実装により、必要クラスタ数が増える場合は新規クラスタを確保して
;; 拡張書き込みが成功するようになった(M6a時点ではここはnilを期待していた)。

(defglobal fat16-test-write1-2clusters (%fat16-test-make-byte-list 2049 66))

(assert-equal t (if (fat16-write-file *test-device* "/WRITE1.TXT" fat16-test-write1-2clusters) t nil))
(assert-equal fat16-test-write1-2clusters (fat16-read-file *test-device* "/WRITE1.TXT"))
(assert-equal (list (list "HELLO.TXT" ':file 0) (list "TEST.LSP" ':file 18) (list "BIG.TXT" ':file 2500) (list "WRITE1.TXT" ':file 2049)
                     (list "SUBDIR" ':dir 0))
              (fat16-read-dir *test-device* "/"))

;; クラスタ数が減る書き込み(縮小)はFAT16-M6bの対象外としてnilを返す(現在2
;; クラスタ確保済みのWRITE1.TXTへ、1クラスタで収まる10byteを書こうとする)。
(assert-equal nil (fat16-write-file *test-device* "/WRITE1.TXT" fat16-test-write1-new))

;; 直前の(クラスタ数減少で失敗した)呼び出しでデータ/ディレクトリエントリが
;; 変更されていないことを確認する
(assert-equal fat16-test-write1-2clusters (fat16-read-file *test-device* "/WRITE1.TXT"))
(assert-equal (list (list "HELLO.TXT" ':file 0) (list "TEST.LSP" ':file 18) (list "BIG.TXT" ':file 2500) (list "WRITE1.TXT" ':file 2049)
                     (list "SUBDIR" ':dir 0))
              (fat16-read-dir *test-device* "/"))

;;; --- FAT16-M6c: 新規ファイル作成 ---
;;
;; Makefileのmkfs.vfat手順でDELETED.TXTを最後に作成後rmしているため、この時点で
;; ルートディレクトリにはWRITE1.TXT/SUBDIRの直後に0xE5(削除済み再利用可)スロットが
;; 1つ、その直後に終端(0x00)スロットが続く並びになっている(先頭のコメント参照。
;; SUBDIRはFAT16-M7aでWRITE1.TXTの後・DELETED.TXT作成/rm前に追加されたため、
;; 0xE5スロットより前の固定位置に入る)。fat16-create-fileは最初に見つかった
;; 空きスロットへ書き込むため、1つ目の新規ファイルはDELETED.TXTだったスロットを
;; 再利用し、2つ目は新しい終端スロットに入る。よって作成後の一覧はHELLO.TXT/
;; TEST.LSP/BIG.TXT/WRITE1.TXT/SUBDIRの後に、作成した順で並ぶ。

;; 空ファイルの新規作成(クラスタ確保なし、start-cluster=0/size=0)
(assert-equal t (if (fat16-create-file *test-device* "/NEW1.TXT" nil) t nil))
(assert-equal nil (fat16-read-file *test-device* "/NEW1.TXT"))

;; 1クラスタに収まる非空ファイルの新規作成
(defglobal fat16-test-new2 (%fat16-test-make-byte-list 100 67)) ;; 全要素67('C')

(assert-equal t (if (fat16-create-file *test-device* "/NEW2.TXT" fat16-test-new2) t nil))
(assert-equal fat16-test-new2 (fat16-read-file *test-device* "/NEW2.TXT"))

(assert-equal (list (list "HELLO.TXT" ':file 0) (list "TEST.LSP" ':file 18) (list "BIG.TXT" ':file 2500) (list "WRITE1.TXT" ':file 2049)
                     (list "SUBDIR" ':dir 0)
                     (list "NEW1.TXT" ':file 0) (list "NEW2.TXT" ':file 100))
              (fat16-read-dir *test-device* "/"))

;; 同名エントリが既に存在する場合はnil(上書きはfat16-write-fileの役割)。
;; 既存のTEST.LSPの内容/一覧が変更されていないことも確認する。
(assert-equal nil (fat16-create-file *test-device* "/TEST.LSP" fat16-test-new2))
(assert-equal 18 (length (fat16-read-file *test-device* "/TEST.LSP")))
(assert-equal (list (list "HELLO.TXT" ':file 0) (list "TEST.LSP" ':file 18) (list "BIG.TXT" ':file 2500) (list "WRITE1.TXT" ':file 2049)
                     (list "SUBDIR" ':dir 0)
                     (list "NEW1.TXT" ':file 0) (list "NEW2.TXT" ':file 100))
              (fat16-read-dir *test-device* "/"))

;; 8.3形式で表現できない名前(複数ドット、またはbase/extが長すぎる)はnil。
;; いずれもスロット確保より前段の名前変換で失敗するため、一覧は変化しない。
(assert-equal nil (fat16-create-file *test-device* "/A.B.C" fat16-test-new2))
(assert-equal nil (fat16-create-file *test-device* "/TOOLONGNAME.TXT" fat16-test-new2))
(assert-equal (list (list "HELLO.TXT" ':file 0) (list "TEST.LSP" ':file 18) (list "BIG.TXT" ':file 2500) (list "WRITE1.TXT" ':file 2049)
                     (list "SUBDIR" ':dir 0)
                     (list "NEW1.TXT" ':file 0) (list "NEW2.TXT" ':file 100))
              (fat16-read-dir *test-device* "/"))

;;; --- FAT16-M7a: サブディレクトリ対応(読み込み・パス解決) ---
;;
;; SUBDIR(Makefile参照、実vfatドライバのmkdirで作成、"."/".."は本物のカーネルが
;; 生成したもの)の中にNESTED.TXT(19byte)、さらにSUBDIR/DEEPER(中にDEEP.TXT、
;; 空ファイル)という2階層のネストを持つ。%fat16-resolve-dir/%fat16-resolve-fileの
;; while反復によるパス解決が、ルート専用だった旧実装と同じ結果をルート直下でも
;; サブディレクトリでも返すことを確認する。

(assert-equal (list (list "." ':dir 0) (list ".." ':dir 0) (list "NESTED.TXT" ':file 19) (list "DEEPER" ':dir 0))
              (fat16-read-dir *test-device* "/SUBDIR"))

(assert-equal (list (list "." ':dir 0) (list ".." ':dir 0) (list "DEEP.TXT" ':file 0))
              (fat16-read-dir *test-device* "/SUBDIR/DEEPER"))

(assert-equal #(110 101 115 116 101 100 32 102 105 108 101 32 99 111 110 116 101 110 116)
              (fat16-read-file *test-device* "/SUBDIR/NESTED.TXT"))

(assert-equal nil (fat16-read-file *test-device* "/SUBDIR/DEEPER/DEEP.TXT"))

;; 存在しないディレクトリ/ファイルはnil
(assert-equal nil (fat16-read-dir *test-device* "/NOSUCHDIR"))
(assert-equal nil (fat16-read-file *test-device* "/SUBDIR/NOSUCH.TXT"))

;; ファイル(ディレクトリでないエントリ)をディレクトリとして辿ろうとした場合はnil
;; (中間パス要素・最終要素のいずれの場合も属性チェックで失敗する)
(assert-equal nil (fat16-read-dir *test-device* "/TEST.LSP"))
(assert-equal nil (fat16-read-file *test-device* "/TEST.LSP/X.TXT"))

;;; --- FAT16-M7b: サブディレクトリ対応(書き込み・新規作成) ---
;;
;; fat16-write-file/fat16-create-fileが%fat16-resolve-fileで多階層パスを解決する
;; ようになったことを、/SUBDIR/NESTED.TXTへの上書きと/SUBDIR/NEW3.TXTの新規作成で
;; 確認する(ルート直下に対する既存のM6a/M6b/M6cテストは無変更のまま上でも回帰
;; 確認済み)。

;; 既存ファイル(/SUBDIR/NESTED.TXT、19byte)への同クラスタ内上書き
(defglobal fat16-test-nested-new #(78 69 83 84 69 68 45 78 69 87)) ;; "NESTED-NEW"

(assert-equal t (if (fat16-write-file *test-device* "/SUBDIR/NESTED.TXT" fat16-test-nested-new) t nil))
(assert-equal fat16-test-nested-new (fat16-read-file *test-device* "/SUBDIR/NESTED.TXT"))
(assert-equal (list (list "." ':dir 0) (list ".." ':dir 0) (list "NESTED.TXT" ':file 10) (list "DEEPER" ':dir 0))
              (fat16-read-dir *test-device* "/SUBDIR"))

;; サブディレクトリ内への新規ファイル作成(1クラスタに収まる非空ファイル)
(defglobal fat16-test-subdir-new3 (%fat16-test-make-byte-list 50 68)) ;; 全要素68('D')

(assert-equal t (if (fat16-create-file *test-device* "/SUBDIR/NEW3.TXT" fat16-test-subdir-new3) t nil))
(assert-equal fat16-test-subdir-new3 (fat16-read-file *test-device* "/SUBDIR/NEW3.TXT"))
(assert-equal (list (list "." ':dir 0) (list ".." ':dir 0) (list "NESTED.TXT" ':file 10) (list "DEEPER" ':dir 0)
                     (list "NEW3.TXT" ':file 50))
              (fat16-read-dir *test-device* "/SUBDIR"))

;; 存在しないディレクトリの下への書き込み・新規作成はいずれもnil
(assert-equal nil (fat16-write-file *test-device* "/NOSUCHDIR/X.TXT" fat16-test-subdir-new3))
(assert-equal nil (fat16-create-file *test-device* "/NOSUCHDIR/X.TXT" fat16-test-subdir-new3))

;; 2階層下(/SUBDIR/DEEPER)への新規作成も往復一致することを確認する
(assert-equal t (if (fat16-create-file *test-device* "/SUBDIR/DEEPER/NEW4.TXT" fat16-test-subdir-new3) t nil))
(assert-equal fat16-test-subdir-new3 (fat16-read-file *test-device* "/SUBDIR/DEEPER/NEW4.TXT"))

;;; --- FAT16-M7c: fat16-create-directory(mkdir)新設 ---
;;
;; fat16-create-directoryでルート直下・サブディレクトリ配下それぞれに新規
;; ディレクトリを作成し、"."/".."エントリのstart-clusterが正しいこと(ルート直下
;; なら".."は規約通り0)、作成したディレクトリの中でfat16-create-file/
;; fat16-read-fileが正常に動くこと(mkdir→create→readのライフサイクル)、
;; 同名重複・8.3非対応名の失敗ケースを確認する。

;; ルート直下への新規ディレクトリ作成
(assert-equal t (if (fat16-create-directory *test-device* "/NEWDIR") t nil))
(assert-equal (list (list "HELLO.TXT" ':file 0) (list "TEST.LSP" ':file 18) (list "BIG.TXT" ':file 2500) (list "WRITE1.TXT" ':file 2049)
                     (list "SUBDIR" ':dir 0)
                     (list "NEW1.TXT" ':file 0) (list "NEW2.TXT" ':file 100)
                     (list "NEWDIR" ':dir 0))
              (fat16-read-dir *test-device* "/"))

;; 作成したディレクトリの内側は"."/".."のみ、".."はルート直下なので規約通り
;; start-cluster=0(fat16-read-dirの一覧にはstart-clusterが出ないため、ここでは
;; 一覧の名前・種別・sizeのみ確認する。start-clusterの規約自体は%fat16-resolve-dir
;; がルートを(lbas . 0)として扱う既存の仕組みに依存している)。
(assert-equal (list (list "." ':dir 0) (list ".." ':dir 0))
              (fat16-read-dir *test-device* "/NEWDIR"))

;; 作成したディレクトリの中への新規ファイル作成→読み込みの往復一致
(assert-equal t (if (fat16-create-file *test-device* "/NEWDIR/INSIDE.TXT" fat16-test-subdir-new3) t nil))
(assert-equal fat16-test-subdir-new3 (fat16-read-file *test-device* "/NEWDIR/INSIDE.TXT"))
(assert-equal (list (list "." ':dir 0) (list ".." ':dir 0) (list "INSIDE.TXT" ':file 50))
              (fat16-read-dir *test-device* "/NEWDIR"))

;; 既存サブディレクトリ(/SUBDIR)の配下への新規ディレクトリ作成
;; (".."が親=SUBDIRのstart-clusterを指すことを確認するため、SUBDIR内に別の
;; ファイルを作ってから対象ディレクトリのfat16-read-dirで一覧を確認する)
(assert-equal t (if (fat16-create-directory *test-device* "/SUBDIR/NEWSUB") t nil))
(assert-equal (list (list "." ':dir 0) (list ".." ':dir 0))
              (fat16-read-dir *test-device* "/SUBDIR/NEWSUB"))
(assert-equal (list (list "." ':dir 0) (list ".." ':dir 0) (list "NESTED.TXT" ':file 10) (list "DEEPER" ':dir 0)
                     (list "NEW3.TXT" ':file 50)
                     (list "NEWSUB" ':dir 0))
              (fat16-read-dir *test-device* "/SUBDIR"))

;; 同名エントリが既に存在する場合はnil(親ディレクトリの一覧は変化しない)
(assert-equal nil (fat16-create-directory *test-device* "/SUBDIR"))
(assert-equal nil (fat16-create-directory *test-device* "/NEWDIR"))

;; 8.3形式で表現できない名前はnil
(assert-equal nil (fat16-create-directory *test-device* "/A.B.C"))
(assert-equal nil (fat16-create-directory *test-device* "/TOOLONGDIRNAME"))

;; 存在しない親ディレクトリの下へのmkdirもnil
(assert-equal nil (fat16-create-directory *test-device* "/NOSUCHDIR/CHILD"))

;;; --- [ファイルI/O]#48(M5): write-from!の境界値テスト ---
;;
;; 他のテストの状態(WRITE1.TXT等の既存フィクスチャ)に影響しないよう、この
;; テスト専用の新規ファイルを作って使う。クラスタサイズは2048byte(Makefile参照)。
;;
;; #47(read-into!)と同じ原因(*generic-methods*がinit.lispのdefdynamicで
;; 上書き消去され、総称関数が「no applicable method」で無反応スキップになる
;; 問題、詳細はread-into!境界値テストのコメント参照)により、以前は実際の
;; ディスク書き込み内容を検証できていなかった。原因を修正したので、通常通り
;; 書き込み後の内容をfat16-read-fileで読み直して検証する。

;; パス解決だけを行いnode(<fat16-file-node>)を取得するテスト専用ヘルパー
;; %fat16-test-resolve-node(read-into!境界値テストで定義済み)をそのまま使う。

;; 1クラスタに収まる小さいファイルの一部を書き換える(セクタ境界をまたがない範囲)
(assert-equal t (if (fat16-create-file *test-device* "/M5SMALL.TXT" (create-vector 10 65)) t nil)) ;; 全byte'A'
(defglobal fat16-test-small-node (%fat16-test-resolve-node *test-device* "/M5SMALL.TXT"))
(assert-equal t (write-from! fat16-test-small-node (create-vector 3 66) 0 4 3)) ;; オフセット4から3byte'B'
(defglobal fat16-test-small-after (fat16-read-file *test-device* "/M5SMALL.TXT"))
(assert-equal 10 (length fat16-test-small-after))
(assert-equal #(65 65 65 65 66 66 66 65 65 65) fat16-test-small-after)
(assert-equal 10 (slot-value fat16-test-small-node 'size)) ;; 既存範囲内の上書きなのでsizeは変化しない

;; 3byteファイルへの1byte書き込み(できるだけ小さい規模での確認。クラスタサイズは
;; 2048byteなので3byteファイルでも書き込みループが辿るセクタ数(1クラスタ=4セクタ)は
;; 変わらないが、期待値の検証はしやすい)
(assert-equal t (if (fat16-create-file *test-device* "/M5MICRO.TXT" (create-vector 3 65)) t nil))
(defglobal fat16-test-micro-node (%fat16-test-resolve-node *test-device* "/M5MICRO.TXT"))
(assert-equal t (write-from! fat16-test-micro-node (create-vector 1 90) 0 1 1)) ;; オフセット1に'Z'
(defglobal fat16-test-micro-after (fat16-read-file *test-device* "/M5MICRO.TXT"))
(assert-equal #(65 90 65) fat16-test-micro-after)

;; ファイル末尾を越える範囲への書き込み(ファイルサイズの拡張、既存クラスタ内)
(assert-equal t (if (fat16-create-file *test-device* "/M5EXTEND.TXT" (create-vector 10 65)) t nil))
(defglobal fat16-test-extend-node (%fat16-test-resolve-node *test-device* "/M5EXTEND.TXT"))
(assert-equal t (write-from! fat16-test-extend-node (create-vector 5 67) 0 8 5)) ;; オフセット8から5byte'C'→サイズ13に拡張
(defglobal fat16-test-extend-after (fat16-read-file *test-device* "/M5EXTEND.TXT"))
(assert-equal 13 (length fat16-test-extend-after))
(assert-equal #(65 65 65 65 65 65 65 65 67 67 67 67 67) fat16-test-extend-after)
(assert-equal 13 (slot-value fat16-test-extend-node 'size))

;; 複数クラスタにまたがる書き込み(クラスタサイズ2048byteの境界をまたぐ範囲)
(assert-equal t (if (fat16-create-file *test-device* "/M5MULTI.TXT" (create-vector 2048 65)) t nil))
(defglobal fat16-test-multi-node (%fat16-test-resolve-node *test-device* "/M5MULTI.TXT"))
(assert-equal t (write-from! fat16-test-multi-node (create-vector 10 68) 0 2043 10)) ;; オフセット2043から10byte'D'(2043-2052、クラスタ境界2048をまたぐ)
(defglobal fat16-test-multi-after (fat16-read-file *test-device* "/M5MULTI.TXT"))
(assert-equal 2053 (length fat16-test-multi-after)) ;; 2043+10=2053へ拡張
(assert-equal 65 (elt fat16-test-multi-after 2042))
(assert-equal 68 (elt fat16-test-multi-after 2043))
(assert-equal 68 (elt fat16-test-multi-after 2047))
(assert-equal 68 (elt fat16-test-multi-after 2048))
(assert-equal 68 (elt fat16-test-multi-after 2052))

;; 新規クラスタ確保を伴う追記(既存チェイン長を超えるオフセットへの書き込み)
(assert-equal t (if (fat16-create-file *test-device* "/M5APPEND.TXT" (create-vector 2048 65)) t nil)) ;; ちょうど1クラスタ
(defglobal fat16-test-append-node (%fat16-test-resolve-node *test-device* "/M5APPEND.TXT"))
(assert-equal t (write-from! fat16-test-append-node (create-vector 5 69) 0 2048 5)) ;; 2クラスタ目に新規書き込み
(defglobal fat16-test-append-after (fat16-read-file *test-device* "/M5APPEND.TXT"))
(assert-equal 2053 (length fat16-test-append-after))
(assert-equal 65 (elt fat16-test-append-after 2047))
(assert-equal 69 (elt fat16-test-append-after 2048))
(assert-equal 69 (elt fat16-test-append-after 2052))

;;; --- [ファイルI/O]#49(M6): OPEN-OUTPUT-FILE/OPEN-IO-FILEのストリーミングI/O ---
;; 旧STREAM_FAT_FILE_CAP(65536byte)を明確に超えるファイルの書き込み→クローズ→
;; 再読み込みが欠落なく完走することを確認する(#39の直接的な解消確認)。
;; マウントされたパス経由でOPEN-OUTPUT-FILE/OPEN-IO-FILEがFATストリームを
;; 使うのはこのテストが初めてなので、専用に/mntへblk0(FAT16)をmountする。
;;
;; 書き込みループは(defun ...)に包んでza.cのJITコンパイル対象にする。トップ
;; レベルのwhileフォームは(このOSの)ツリーウォーク型インタプリタで反復ごとに
;; C側の再帰呼び出しとして評価され、TCOもスタックガードも無いため(eval.cの
;; 既知の制約、fat16-cluster-chain等のコメント参照)、数万回級の反復では
;; スタックオーバーフローで隣接メモリ(os_stream_t含む)を破損しうることが
;; 調査で判明した(write-charがある時点からFAT分岐に到達しなくなり、
;; ファイルサイズが非決定的に縮む形で顕在化した)。defun本体はza.cでJIT
;; コンパイルされ実際のネイティブループになるため、この制約を受けない
;; (za_test_stress.lispがisiki-za-test-cons-chain等をdefunとして定義し
;; N=50000で安全に反復できているのと同じ理由)。
(mount "/mnt" 'blk0 ':fat16)

(defun %%fat16-test-write-n-chars (stream ch n)
  (let ((i 0))
    (while (< i n)
      (write-char ch stream)
      (setq i (+ i 1)))))

(defglobal fat16-test-bigwrite-len 70000) ;; 65536byteの旧上限を明確に超える
(defglobal fat16-test-bigwrite-stream (open-output-file "/mnt/BIGWR.TXT"))
(assert-equal t (if fat16-test-bigwrite-stream t nil))
(%%fat16-test-write-n-chars fat16-test-bigwrite-stream #\A fat16-test-bigwrite-len) ;; 全byte'A'
(close fat16-test-bigwrite-stream)

(defglobal fat16-test-bigwrite-after (fat16-read-file *test-device* "/BIGWR.TXT"))
(assert-equal t (if fat16-test-bigwrite-after t nil))
(assert-equal fat16-test-bigwrite-len (length fat16-test-bigwrite-after))
(assert-equal 65 (elt fat16-test-bigwrite-after 0))
(assert-equal 65 (elt fat16-test-bigwrite-after 34999))
(assert-equal 65 (elt fat16-test-bigwrite-after 69999))

;; OPEN-IO-FILE: 既存ファイルに対してtruncateしない(#49で解消した既知の非対称性、
;; 「常に空バッファから始まり書き込み前のreadが常にEOFになる」の確認)。
;; 開いた直後にread-charで既存内容('A')が読めることを確認してから、
;; オフセット50000へseekして1byteだけ上書きし、ファイル全体は壊れず
;; サイズも変わらないことを確認する。
(defglobal fat16-test-bigio-stream (open-io-file "/mnt/BIGWR.TXT"))
(assert-equal t (if fat16-test-bigio-stream t nil))
(defglobal fat16-test-bigio-first-char (read-char fat16-test-bigio-stream))
(assert-equal #\A fat16-test-bigio-first-char) ;; truncateされていれば即EOF(nil)のはず
(set-file-position fat16-test-bigio-stream 50000)
(write-char #\Z fat16-test-bigio-stream)
(close fat16-test-bigio-stream)

(defglobal fat16-test-bigio-after (fat16-read-file *test-device* "/BIGWR.TXT"))
(assert-equal fat16-test-bigwrite-len (length fat16-test-bigio-after)) ;; サイズは変化しない
(assert-equal 65 (elt fat16-test-bigio-after 0))
(assert-equal 65 (elt fat16-test-bigio-after 49999))
(assert-equal 90 (elt fat16-test-bigio-after 50000))
(assert-equal 65 (elt fat16-test-bigio-after 50001))
(assert-equal 65 (elt fat16-test-bigio-after 69999))

;;; --- [ファイルI/O]#50(M7): file-length高速パス + read-file-into-vector/write-vector-to-file ---

;; fat16-file-sizeはディレクトリエントリの解決のみ(O(ディレクトリサイズ))で、
;; fat16-read-file(ファイル全体読み込み)のような性能問題(#41)を引き継がない
;; ことを、直前に作成済みのBIGWR.TXT(70000byte)に対する壁時計時間の相対比較で
;; 確認する(絶対時間の閾値だとQEMU実行環境の速度差でフレーキーになりうるため、
;; 同一環境内での相対比較にする)。#41の対象規模である1.76MBでの絶対時間の
;; 実測はM9で別途行う。
(defglobal fat16-test-filesize-t0 (get-internal-real-time))
(defglobal fat16-test-filesize-result (fat16-file-size *test-device* "/BIGWR.TXT"))
(defglobal fat16-test-filesize-t1 (get-internal-real-time))
(assert-equal 70000 fat16-test-filesize-result)

(defglobal fat16-test-readfile-t0 (get-internal-real-time))
(defglobal fat16-test-readfile-result (fat16-read-file *test-device* "/BIGWR.TXT"))
(defglobal fat16-test-readfile-t1 (get-internal-real-time))
(assert-equal 70000 (length fat16-test-readfile-result))

(assert-equal t (<= (- fat16-test-filesize-t1 fat16-test-filesize-t0)
                     (- fat16-test-readfile-t1 fat16-test-readfile-t0)))

;; FILE-LENGTH(cc_file_length, パス文字列引数)がマウント経由でもfat16-file-size
;; と同じ高速パスを通ることを確認する。
(assert-equal 70000 (file-length "/mnt/BIGWR.TXT"))
(assert-equal 2500 (file-length "/mnt/BIG.TXT"))

;; read-file-into-vector/write-vector-to-fileの往復(write→read一致)を確認する。
(defglobal fat16-test-rfitv-vec (create-vector 300 0))
(defglobal fat16-test-rfitv-fill-i 0)
(while (< fat16-test-rfitv-fill-i 300)
  (progn
    (set-elt (mod fat16-test-rfitv-fill-i 256) fat16-test-rfitv-vec fat16-test-rfitv-fill-i)
    (setq fat16-test-rfitv-fill-i (+ fat16-test-rfitv-fill-i 1))))
(assert-equal t (if (write-vector-to-file "/mnt/RFITV.BIN" fat16-test-rfitv-vec) t nil))
(defglobal fat16-test-rfitv-readback (read-file-into-vector "/mnt/RFITV.BIN"))
(assert-equal t (if fat16-test-rfitv-readback t nil))
(assert-equal fat16-test-rfitv-vec fat16-test-rfitv-readback)
(assert-equal nil (read-file-into-vector "/mnt/NO-SUCH-FILE.BIN"))

