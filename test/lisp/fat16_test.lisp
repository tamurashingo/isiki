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
;; 直接dd/printfし、クラスタ40→41→44→終端(0xFFFF)という非連続なチェインを合成
;; している(クラスタ40/41/44は他の実ファイル/ディレクトリが使う番号帯(3〜9番台)
;; から十分離しており衝突しない。FAT16-M7aでSUBDIR以下を追加した際、元は
;; 10/11/14だったが実クラスタ使用と衝突するため40/41/44へ変更した)。

(defglobal fat16-test-bpb-m3 (fat16-read-bpb *ide-device*))

(assert-equal 41 (fat16-fat-entry *ide-device* fat16-test-bpb-m3 40))
(assert-equal 44 (fat16-fat-entry *ide-device* fat16-test-bpb-m3 41))
(assert-equal #xFFFF (fat16-fat-entry *ide-device* fat16-test-bpb-m3 44))

(assert-equal (list 40 41 44) (fat16-cluster-chain *ide-device* fat16-test-bpb-m3 40))

;;; --- FAT16-M6a: 既存ファイルの同クラスタ数上書き ---
;;
;; WRITE1.TXT(Makefile参照、mkfs.vfat作成時に"A"を2048回=ちょうど1クラスタ分
;; 書き込んだ書き込みテスト専用ファイル)に対して同じ1クラスタ以内に収まる別内容を
;; 書き込み、読み込みで一致することを確認する。他の既存ファイル(TEST.LSP/BIG.TXT/
;; HELLO.TXT)は読み込み専用のまま変更しないため、書き込みはこのファイルにのみ行う。

;; (%fat16-test-make-byte-list n value) : 長さnの、全要素がvalueのfixnumリストを
;; 作るテスト専用ヘルパー。init_aot.lispのcreate-listはこのマイルストンの起動
;; スクリプト(init.lispのみload)からは使えないため自前で用意する。再帰は使わず
;; whileで組み立てる(nがファイルサイズに比例して大きくなり得るため、
;; eval_no_tco_interpreter_stack_limitと同じ理由でLisp再帰を避ける)。
(defun %fat16-test-make-byte-list (n value)
  (let ((i 0) (acc nil))
    (while (< i n)
      (setq acc (cons value acc))
      (setq i (+ i 1)))
    acc))

;; 書き込み前の内容確認(念のため)
(assert-equal 2048 (length (fat16-read-file *ide-device* "/WRITE1.TXT")))
(assert-equal 65 (elt (fat16-read-file *ide-device* "/WRITE1.TXT") 0))
(assert-equal 65 (elt (fat16-read-file *ide-device* "/WRITE1.TXT") 2047))

(defglobal fat16-test-write1-new (list 87 82 73 84 69 49 45 78 69 87)) ;; "WRITE1-NEW"

(assert-equal t (if (fat16-write-file *ide-device* "/WRITE1.TXT" fat16-test-write1-new) t nil))
(assert-equal fat16-test-write1-new (fat16-read-file *ide-device* "/WRITE1.TXT"))
(assert-equal (list (list "HELLO.TXT" ':file 0) (list "TEST.LSP" ':file 18) (list "BIG.TXT" ':file 2500) (list "WRITE1.TXT" ':file 10)
                     (list "SUBDIR" ':dir 0))
              (fat16-read-dir *ide-device* "/"))

;;; --- FAT16-M6b: クラスタ追加を伴うファイル拡張 ---
;;
;; WRITE1.TXTは直前のM6a確認時点で1クラスタ(2048byte以内)のまま。2049byteは
;; 1クラスタを超えるため2クラスタ必要になり、現在の1クラスタと不一致になる。
;; FAT16-M6b実装により、必要クラスタ数が増える場合は新規クラスタを確保して
;; 拡張書き込みが成功するようになった(M6a時点ではここはnilを期待していた)。

(defglobal fat16-test-write1-2clusters (%fat16-test-make-byte-list 2049 66))

(assert-equal t (if (fat16-write-file *ide-device* "/WRITE1.TXT" fat16-test-write1-2clusters) t nil))
(assert-equal fat16-test-write1-2clusters (fat16-read-file *ide-device* "/WRITE1.TXT"))
(assert-equal (list (list "HELLO.TXT" ':file 0) (list "TEST.LSP" ':file 18) (list "BIG.TXT" ':file 2500) (list "WRITE1.TXT" ':file 2049)
                     (list "SUBDIR" ':dir 0))
              (fat16-read-dir *ide-device* "/"))

;; クラスタ数が減る書き込み(縮小)はFAT16-M6bの対象外としてnilを返す(現在2
;; クラスタ確保済みのWRITE1.TXTへ、1クラスタで収まる10byteを書こうとする)。
(assert-equal nil (fat16-write-file *ide-device* "/WRITE1.TXT" fat16-test-write1-new))

;; 直前の(クラスタ数減少で失敗した)呼び出しでデータ/ディレクトリエントリが
;; 変更されていないことを確認する
(assert-equal fat16-test-write1-2clusters (fat16-read-file *ide-device* "/WRITE1.TXT"))
(assert-equal (list (list "HELLO.TXT" ':file 0) (list "TEST.LSP" ':file 18) (list "BIG.TXT" ':file 2500) (list "WRITE1.TXT" ':file 2049)
                     (list "SUBDIR" ':dir 0))
              (fat16-read-dir *ide-device* "/"))

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
(assert-equal t (if (fat16-create-file *ide-device* "/NEW1.TXT" nil) t nil))
(assert-equal nil (fat16-read-file *ide-device* "/NEW1.TXT"))

;; 1クラスタに収まる非空ファイルの新規作成
(defglobal fat16-test-new2 (%fat16-test-make-byte-list 100 67)) ;; 全要素67('C')

(assert-equal t (if (fat16-create-file *ide-device* "/NEW2.TXT" fat16-test-new2) t nil))
(assert-equal fat16-test-new2 (fat16-read-file *ide-device* "/NEW2.TXT"))

(assert-equal (list (list "HELLO.TXT" ':file 0) (list "TEST.LSP" ':file 18) (list "BIG.TXT" ':file 2500) (list "WRITE1.TXT" ':file 2049)
                     (list "SUBDIR" ':dir 0)
                     (list "NEW1.TXT" ':file 0) (list "NEW2.TXT" ':file 100))
              (fat16-read-dir *ide-device* "/"))

;; 同名エントリが既に存在する場合はnil(上書きはfat16-write-fileの役割)。
;; 既存のTEST.LSPの内容/一覧が変更されていないことも確認する。
(assert-equal nil (fat16-create-file *ide-device* "/TEST.LSP" fat16-test-new2))
(assert-equal 18 (length (fat16-read-file *ide-device* "/TEST.LSP")))
(assert-equal (list (list "HELLO.TXT" ':file 0) (list "TEST.LSP" ':file 18) (list "BIG.TXT" ':file 2500) (list "WRITE1.TXT" ':file 2049)
                     (list "SUBDIR" ':dir 0)
                     (list "NEW1.TXT" ':file 0) (list "NEW2.TXT" ':file 100))
              (fat16-read-dir *ide-device* "/"))

;; 8.3形式で表現できない名前(複数ドット、またはbase/extが長すぎる)はnil。
;; いずれもスロット確保より前段の名前変換で失敗するため、一覧は変化しない。
(assert-equal nil (fat16-create-file *ide-device* "/A.B.C" fat16-test-new2))
(assert-equal nil (fat16-create-file *ide-device* "/TOOLONGNAME.TXT" fat16-test-new2))
(assert-equal (list (list "HELLO.TXT" ':file 0) (list "TEST.LSP" ':file 18) (list "BIG.TXT" ':file 2500) (list "WRITE1.TXT" ':file 2049)
                     (list "SUBDIR" ':dir 0)
                     (list "NEW1.TXT" ':file 0) (list "NEW2.TXT" ':file 100))
              (fat16-read-dir *ide-device* "/"))

;;; --- FAT16-M7a: サブディレクトリ対応(読み込み・パス解決) ---
;;
;; SUBDIR(Makefile参照、実vfatドライバのmkdirで作成、"."/".."は本物のカーネルが
;; 生成したもの)の中にNESTED.TXT(19byte)、さらにSUBDIR/DEEPER(中にDEEP.TXT、
;; 空ファイル)という2階層のネストを持つ。%fat16-resolve-dir/%fat16-resolve-fileの
;; while反復によるパス解決が、ルート専用だった旧実装と同じ結果をルート直下でも
;; サブディレクトリでも返すことを確認する。

(assert-equal (list (list "." ':dir 0) (list ".." ':dir 0) (list "NESTED.TXT" ':file 19) (list "DEEPER" ':dir 0))
              (fat16-read-dir *ide-device* "/SUBDIR"))

(assert-equal (list (list "." ':dir 0) (list ".." ':dir 0) (list "DEEP.TXT" ':file 0))
              (fat16-read-dir *ide-device* "/SUBDIR/DEEPER"))

(assert-equal (list 110 101 115 116 101 100 32 102 105 108 101 32 99 111 110 116 101 110 116)
              (fat16-read-file *ide-device* "/SUBDIR/NESTED.TXT"))

(assert-equal nil (fat16-read-file *ide-device* "/SUBDIR/DEEPER/DEEP.TXT"))

;; 存在しないディレクトリ/ファイルはnil
(assert-equal nil (fat16-read-dir *ide-device* "/NOSUCHDIR"))
(assert-equal nil (fat16-read-file *ide-device* "/SUBDIR/NOSUCH.TXT"))

;; ファイル(ディレクトリでないエントリ)をディレクトリとして辿ろうとした場合はnil
;; (中間パス要素・最終要素のいずれの場合も属性チェックで失敗する)
(assert-equal nil (fat16-read-dir *ide-device* "/TEST.LSP"))
(assert-equal nil (fat16-read-file *ide-device* "/TEST.LSP/X.TXT"))
