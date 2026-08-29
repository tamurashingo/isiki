;;;; FAT16ファイルシステムの読み込みAPI。
;;;; ide.lispと同じ理由でAOTトランスパイル対象外の、通常のload形式(インタプリタ
;;;; 実行専用)ファイル。src/lisp/ide.lisp(*ide-device*/read-sector)のロードを
;;;; 前提とする。REPLから(load "src/lisp/ide.lisp")→(load "src/lisp/fat16.lisp")
;;;; の順に明示的に読み込む。
;;;;
;;;; FAT16-M1: BPB(Boot Sector)パースのみを実装する(documents/fs.md)。

;; (%fat16-u16 bytes offset) : bytes(fixnum(0-255)のリスト、read-sectorの戻り値)の
;; offsetから始まるLE 16bit値を1個のfixnumに合成する。
(defun %fat16-u16 (bytes offset)
  (logior (elt bytes offset) (ash (elt bytes (+ offset 1)) 8)))

;; (%fat16-u32 bytes offset) : bytesのoffsetから始まるLE 32bit値を1個のfixnumに
;; 合成する。logior/ashが2引数限定のため%fat16-u16を2回に分けて合成する。
(defun %fat16-u32 (bytes offset)
  (logior (%fat16-u16 bytes offset) (ash (%fat16-u16 bytes (+ offset 2)) 16)))

;; BPB(BIOS Parameter Block、ブートセクタの先頭に置かれる)のうち、FAT16の
;; レイアウト計算に必要な最小限のフィールドのみを保持する。
(defclass bpb ()
  ((bytes-per-sector :initarg :bytes-per-sector :initform nil)
   (sectors-per-cluster :initarg :sectors-per-cluster :initform nil)
   (reserved-sectors :initarg :reserved-sectors :initform nil)
   (num-fats :initarg :num-fats :initform nil)
   (root-entry-count :initarg :root-entry-count :initform nil)
   (total-sectors :initarg :total-sectors :initform nil)
   (sectors-per-fat :initarg :sectors-per-fat :initform nil)))

;; (fat16-read-bpb device) : deviceのセクタ0(ブートセクタ)を読み、BPBの各フィールドを
;; 取り出してbpbインスタンスを返す。read-sectorが失敗した場合はnil。
;; オフセットは標準的なFAT16 BPBのレイアウト通り:
;;   0x0B(11) bytes-per-sector (u16)
;;   0x0D(13) sectors-per-cluster (u8)
;;   0x0E(14) reserved-sectors (u16)
;;   0x10(16) num-fats (u8)
;;   0x11(17) root-entry-count (u16)
;;   0x13(19) total-sectors-16 (u16、0なら0x20(32)のtotal-sectors-32を使う)
;;   0x16(22) sectors-per-fat (u16)
;;   0x20(32) total-sectors-32 (u32)
(defun fat16-read-bpb (device)
  (let ((bytes (read-sector device 0)))
    (if (null bytes)
        nil
        (let ((total-sectors-16 (%fat16-u16 bytes 19)))
          (make-instance 'bpb
            ':bytes-per-sector (%fat16-u16 bytes 11)
            ':sectors-per-cluster (elt bytes 13)
            ':reserved-sectors (%fat16-u16 bytes 14)
            ':num-fats (elt bytes 16)
            ':root-entry-count (%fat16-u16 bytes 17)
            ':total-sectors (if (= total-sectors-16 0)
                                 (%fat16-u32 bytes 32)
                                 total-sectors-16)
            ':sectors-per-fat (%fat16-u16 bytes 22))))))

;;; --- FAT16-M2: ルートディレクトリエントリの列挙 ---

;; (fat16-root-dir-lba bpb) : ルートディレクトリの開始LBA。予約セクタの直後に
;; num-fats個のFATテーブルが並ぶため、その合計を予約セクタ数に足したものになる。
(defun fat16-root-dir-lba (bpb)
  (+ (slot-value bpb 'reserved-sectors)
     (* (slot-value bpb 'num-fats) (slot-value bpb 'sectors-per-fat))))

;; (%fat16-root-dir-sector-count bpb) : ルートディレクトリが占有するセクタ数。
;; FAT16の仕様上root-entry-count*32は必ずbytes-per-sectorの倍数になる
;; (ルートディレクトリはセクタ境界に揃えて確保される)。
(defun %fat16-root-dir-sector-count (bpb)
  (div (* (slot-value bpb 'root-entry-count) 32) (slot-value bpb 'bytes-per-sector)))

;; (defclass dir-entry ...) : ディレクトリエントリ1件のパース結果。nameは
;; %fat16-bytes-to-stringで組み立てた表示用文字列(8.3名、拡張子が空なら"."無し)。
(defclass dir-entry ()
  ((name :initarg :name :initform nil)
   (attr :initarg :attr :initform nil)
   (size :initarg :size :initform nil)
   (start-cluster :initarg :start-cluster :initform nil)))

;; (%fat16-drop-leading-spaces bytes) : bytes先頭の連続するASCIIスペース(32)を
;; 取り除いた残りを返す(string-trimが存在しないための自前ヘルパー)。
(defun %fat16-drop-leading-spaces (bytes)
  (if (and (not (null bytes)) (= (car bytes) 32))
      (%fat16-drop-leading-spaces (cdr bytes))
      bytes))

;; (%fat16-rtrim-spaces bytes) : bytes末尾の連続するASCIIスペースを取り除いた
;; 残りを返す(reverseして先頭を落とし、reverseで戻す)。
(defun %fat16-rtrim-spaces (bytes)
  (reverse (%fat16-drop-leading-spaces (reverse bytes))))

;; (%fat16-bytes-to-string bytes) : ASCIIコードのfixnumリストbytesから、対応する
;; 文字を1文字ずつ持つLisp文字列を組み立てる(FAT16-M0(1)で確認したcode-char+
;; create-string+set-eltの手順)。
(defun %fat16-bytes-to-string (bytes)
  (let ((s (create-string (length bytes))))
    (progn
      (for ((b bytes (cdr b))
            (i 0 (+ i 1)))
          ((null b) nil)
        (set-elt (code-char (car b)) s i))
      s)))

;; (%fat16-dir-entry-name bytes offset) : offsetにある32byteエントリの8+3byte名
;; フィールドから表示用文字列("HELLO.TXT"、拡張子が空なら"HELLO"のように"."無し)
;; を組み立てる。
(defun %fat16-dir-entry-name (bytes offset)
  (let ((name-bytes (%fat16-rtrim-spaces (%ide-take (%ide-drop bytes offset) 8)))
        (ext-bytes (%fat16-rtrim-spaces (%ide-take (%ide-drop bytes (+ offset 8)) 3))))
    (if (null ext-bytes)
        (%fat16-bytes-to-string name-bytes)
        (string-append (%fat16-bytes-to-string name-bytes) "." (%fat16-bytes-to-string ext-bytes)))))

;; (%fat16-dir-entry-at bytes offset) : bytes(1セクタ512byte分)のoffsetにある
;; 32byteディレクトリエントリをパースする。先頭バイトが0x00ならシンボル'endを
;; (ルートディレクトリの走査終了、以降は未使用領域)、0xE5(削除済み)ならnilを、
;; それ以外はdir-entryインスタンスを返す。
(defun %fat16-dir-entry-at (bytes offset)
  (let ((first-byte (elt bytes offset)))
    (if (= first-byte 0)
        'end
        (if (= first-byte #xE5)
            nil
            (make-instance 'dir-entry
              ':name (%fat16-dir-entry-name bytes offset)
              ':attr (elt bytes (+ offset 11))
              ':start-cluster (%fat16-u16 bytes (+ offset 26))
              ':size (%fat16-u32 bytes (+ offset 28)))))))

;; セクタ単位のディレクトリエントリ走査本体(旧%fat16-parse-sector-entries/
;; %fat16-scan-root-dir)はFAT16-M7aで%fat16-scan-dir-entries(このファイル末尾、
;; サブディレクトリ対応の節)へ統合された。ルート専用の(lba, 残りセクタ数)引数を
;; 「ディレクトリを構成するLBAのリスト」に一般化し、サブディレクトリ(クラスタ
;; チェインでセクタ数が不定)でも同じ実装で安全に走査できるようにするため。

;; (%fat16-dir-entry-kind attr) : attrバイトのディレクトリ属性ビット(0x10)を見て
;; :fileまたは:dirを返す。
(defun %fat16-dir-entry-kind (attr)
  (if (= (logand attr #x10) 0) ':file ':dir))

;; (%fat16-dir-entries-to-display-list entries) : dir-entryのリストを
;; ("NAME" :file/:dir size)の3要素リストのリストに変換する。
(defun %fat16-dir-entries-to-display-list (entries)
  (if (null entries)
      nil
      (cons (list (slot-value (car entries) 'name)
                  (%fat16-dir-entry-kind (slot-value (car entries) 'attr))
                  (slot-value (car entries) 'size))
            (%fat16-dir-entries-to-display-list (cdr entries)))))

;; (fat16-read-dir device path) : path("/"、または"/DOCS"のような多階層パス、
;; FAT16-M7a)が指すディレクトリのエントリ一覧を("NAME" :file/:dir size)の形の
;; リストで返す。パス解決の実体は%fat16-resolve-dir-lbas(このファイル末尾、
;; FAT16-M7a節)。BPBが読めない・パスが解決できない場合はnil。
(defun fat16-read-dir (device path)
  (let ((bpb (fat16-read-bpb device)))
    (if (null bpb)
        nil
        (let ((lbas (%fat16-resolve-dir-lbas device bpb (%fat16-split-path path))))
          (if (null lbas)
              nil
              (%fat16-dir-entries-to-display-list (%fat16-scan-dir-entries device lbas)))))))

;;; --- FAT16-M3: FATテーブルのクラスタチェイン追跡 ---

;; 直前に読んだFATセクタの(lba . bytes)キャッシュ。init.lispの*classes*と同じ理由
;; (defglobal+setqはsetqが関数呼び出し内のenvironmentにしか書き込めず、呼び出し元に
;; 見えない)でdefdynamic+%%set-dynamicを使う。隣接クラスタへの連続アクセスでの
;; 重複読み込みを避けるための、1セクタ分のみの軽量キャッシュ(過剰最適化はしない)。
(defdynamic *fat16-fat-cache-lba* nil)
(defdynamic *fat16-fat-cache-bytes* nil)

;; (%fat16-fat-sector-bytes device lba) : FATテーブル中のlbaセクタの内容(512byte)
;; を返す。直前に読んだセクタと同じlbaならキャッシュを再利用する。
(defun %fat16-fat-sector-bytes (device lba)
  (if (and (dynamic *fat16-fat-cache-lba*) (= (dynamic *fat16-fat-cache-lba*) lba))
      (dynamic *fat16-fat-cache-bytes*)
      (let ((bytes (read-sector device lba)))
        (progn
          (%%set-dynamic '*fat16-fat-cache-lba* lba)
          (%%set-dynamic '*fat16-fat-cache-bytes* bytes)
          bytes))))

;; (fat16-fat-entry device bpb cluster-no) : FATテーブル中のcluster-noに対応する
;; 16bit値を返す。FATテーブル(1本目)はreserved-sectors番目のセクタから始まり、
;; cluster-no*2byte目の位置にLE u16として格納されている。read-sectorが失敗した
;; 場合はnil。
(defun fat16-fat-entry (device bpb cluster-no)
  (let ((byte-offset (* cluster-no 2)))
    (let ((sector-offset (div byte-offset (slot-value bpb 'bytes-per-sector)))
          (offset-in-sector (mod byte-offset (slot-value bpb 'bytes-per-sector))))
      (let ((bytes (%fat16-fat-sector-bytes device (+ (slot-value bpb 'reserved-sectors) sector-offset))))
        (if (null bytes)
            nil
            (%fat16-u16 bytes offset-in-sector))))))

;; (fat16-cluster-chain device bpb start-cluster) : start-clusterから始まるクラスタ
;; チェインを、FATエントリを辿って訪問順のリストで返す(例: (3 4 7))。エントリ値が
;; 0xFFF8以上(0xFFFFの正規終端マーカーを含む、documents/fs.mdの簡略化した扱いに
;; 従いbad-cluster等も終端として扱う)なら終端、それ未満は次クラスタへの参照として
;; 辿る。fat16-fat-entryがnilを返した(読み込み失敗)場合もそこで打ち切る。
;; クラスタ数に比例して深くなるためLisp再帰ではなくwhileで書く(%fat16-find-free-cluster
;; と同じ理由、documents/fs.md参照)。上限は%fat16-total-cluster-count+2(クラスタ番号は
;; 2始まり)とし、万一FATが破損して自己参照/循環したチェインになっていても無限ループ
;; せず打ち切る(eval.cにはTCOもスタックガードも無いため、再帰実装だと破損チェインが
;; そのままハング/メモリ破損につながり得る)。
(defun fat16-cluster-chain (device bpb start-cluster)
  (let ((cluster start-cluster) (count 0)
        (limit (+ (%fat16-total-cluster-count bpb) 2))
        (rev-chain nil) (done nil))
    (while (and (not done) (< count limit))
      (setq rev-chain (cons cluster rev-chain))
      (setq count (+ count 1))
      (let ((entry (fat16-fat-entry device bpb cluster)))
        (if (or (null entry) (>= entry #xFFF8))
            (setq done t)
            (setq cluster entry))))
    (%fat16-reverse-iter rev-chain)))

;;; --- FAT16-M4: クラスタ→セクタ変換とファイル本体読み込み ---

;; (%fat16-data-start-lba bpb) : データ領域の開始LBA。ルートディレクトリの直後。
(defun %fat16-data-start-lba (bpb)
  (+ (fat16-root-dir-lba bpb) (%fat16-root-dir-sector-count bpb)))

;; (fat16-cluster-to-lba bpb cluster-no) : cluster-noに対応するデータ領域内の
;; LBAを返す。FAT16の仕様上クラスタ番号2がデータ領域の先頭に対応する
;; (0/1は予約、通常のデータクラスタは2始まり)。
(defun fat16-cluster-to-lba (bpb cluster-no)
  (+ (%fat16-data-start-lba bpb)
     (* (- cluster-no 2) (slot-value bpb 'sectors-per-cluster))))

;; (%fat16-find-dir-entry entries name) : dir-entryのリストentriesからnameと
;; (拡張子まで含めた)8.3名が一致するものを探す。見つからなければnil。
(defun %fat16-find-dir-entry (entries name)
  (if (null entries)
      nil
      (if (string= (slot-value (car entries) 'name) name)
          (car entries)
          (%fat16-find-dir-entry (cdr entries) name))))

;; (%fat16-lba-range start-lba count) : start-lbaからcount個の連続したLBA番号を
;; 並べたリストを返す(バイト列ではなくLBA番号自体の小さいリスト。1クラスタ分=
;; sectors-per-cluster個程度なので、これをappendしてもスタックを圧迫しない)。
(defun %fat16-lba-range (start-lba count)
  (if (<= count 0)
      nil
      (cons start-lba (%fat16-lba-range (+ start-lba 1) (- count 1)))))

;; (%fat16-clusters-to-lbas bpb clusters) : clusters(クラスタ番号のリスト、
;; fat16-cluster-chainの戻り値)を、各クラスタを構成する全セクタのLBA番号を
;; 順に並べた1本のリストへ展開する。clustersの要素数に比例して深くなるため、
;; 外側はwhileで辿る(1クラスタ分のLBA展開自体は%fat16-lba-range、既存のまま
;; 小さい再帰で安全)。
(defun %fat16-clusters-to-lbas (bpb clusters)
  (let ((remaining clusters) (rev-lbas nil))
    (while remaining
      (let ((lba-range (%fat16-lba-range (fat16-cluster-to-lba bpb (car remaining))
                                          (slot-value bpb 'sectors-per-cluster))))
        (while lba-range
          (setq rev-lbas (cons (car lba-range) rev-lbas))
          (setq lba-range (cdr lba-range))))
      (setq remaining (cdr remaining)))
    (%fat16-reverse-iter rev-lbas)))

;; (%fat16-reverse-iter list) : listを反転して返す。init_aot.lispのreverse/nreverse
;; はLisp関数呼び出し1回につきC呼び出しスタックを1段消費する再帰実装であり
;; (このインタプリタにはTCOが無いため、末尾再帰であってもスタックは消費される)、
;; 要素数が数千に及ぶとtriple faultする。whileマクロ(tagbody/goベース、
;; src/lisp/init.lisp)はgoでの繰り込みがeval_tagbodyの同じCスタックフレーム内の
;; whileループで処理されるため、繰り返し回数に関わらずCスタックが一定に留まる。
;; %fat16-read-lba-listから使うため、ここではreverse/nreverseを使わずこの
;; while版で反転する。
(defun %fat16-reverse-iter (list)
  (let ((remaining list) (acc nil))
    (while remaining
      (setq acc (cons (car remaining) acc))
      (setq remaining (cdr remaining)))
    acc))

;; (%fat16-read-lba-list device lbas) : lbasの順にセクタを読み、連結したbyteリスト
;; を返す。read-sectorが失敗した場合はそれまでに読んだ分は捨ててnil(既存のIDE層と
;; 同じ「失敗時nil」の慣習)。
;;
;; 以前はappendの第一引数を1セクタ分(512byte)に留める再帰実装だったが、
;; セクタ数(=defun呼び出しの再帰深さ)がファイルサイズに比例して増えると、
;; append自体を直さずともこのインタプリタにTCOが無いこと自体が原因で
;; triple faultした(BIG.TXTの2クラスタ=8セクタで再現、TEST.LSPの1クラスタ=
;; 4セクタでは再現しなかった)。このためセクタ・バイト単位の繰り込みを
;; while(tagbody/goベース、Cスタックを消費しない)へ置き換え、再帰を
;; 一切使わずにファイル全体を読む。
(defun %fat16-read-lba-list (device lbas)
  (let ((remaining-lbas lbas) (rev-bytes nil) (ok t))
    (while (and ok remaining-lbas)
      (let ((sector-bytes (read-sector device (car remaining-lbas))))
        (if (null sector-bytes)
            (setq ok nil)
            (progn
              (let ((remaining-bytes sector-bytes))
                (while remaining-bytes
                  (setq rev-bytes (cons (car remaining-bytes) rev-bytes))
                  (setq remaining-bytes (cdr remaining-bytes))))
              (setq remaining-lbas (cdr remaining-lbas))))))
    (if ok
        (%fat16-reverse-iter rev-bytes)
        nil)))

;; (fat16-read-file device path) : path("/NAME.EXT"、または"/DOCS/NAME.EXT"のような
;; 多階層パス、FAT16-M7a)のファイル本体をfixnum(0-255)のリストとして返す。
;; read-sector/write-sectorと同じバイト列表現(全体方針: FAT16層のバイト列操作は
;; fixnumリストのまま行う、文字列化しない)。パスが解決できない・エントリが
;; 見つからない場合・クラスタ読み込みに失敗した場合はnil。0byteファイル(size=0)
;; はクラスタを辿らずそのままnil(=空リスト)を返す。パス解決の実体は
;; %fat16-resolve-file(このファイル末尾、FAT16-M7a節)。
(defun fat16-read-file (device path)
  (let ((bpb (fat16-read-bpb device)))
    (if (null bpb)
        nil
        (let ((resolved (%fat16-resolve-file device bpb path)))
          (if (null resolved)
              nil
              (let ((entry (%fat16-find-dir-entry
                             (%fat16-scan-dir-entries device (car resolved))
                             (cdr resolved))))
                (if (null entry)
                    nil
                    (let ((size (slot-value entry 'size)))
                      (if (= size 0)
                          nil
                          (let ((bytes (%fat16-read-lba-list device
                                         (%fat16-clusters-to-lbas bpb
                                           (fat16-cluster-chain device bpb (slot-value entry 'start-cluster))))))
                            (if (null bytes)
                                nil
                                (subseq bytes 0 size))))))))))))

;;; --- FAT16-M6a: 既存ファイルの同クラスタ数上書き ---

;; (%fat16-u16-to-bytes n) : fixnum(0-65535)をLEの2byteリストに分解する
;; (%fat16-u16の逆変換)。
(defun %fat16-u16-to-bytes (n)
  (list (logand n #xFF) (logand (ash n -8) #xFF)))

;; (%fat16-u32-to-bytes n) : fixnumをLEの4byteリストに分解する(%fat16-u32の逆変換)。
;; logior/ashが2引数限定のため%fat16-u16-to-bytesを2回に分けて合成する。
(defun %fat16-u32-to-bytes (n)
  (append (%fat16-u16-to-bytes (logand n #xFFFF))
          (%fat16-u16-to-bytes (logand (ash n -16) #xFFFF))))

;; (%fat16-patch-bytes! list offset value-list) : listを破壊的にパッチする。
;; cdrでoffset分whileで進めた後、value-listの各要素をset-carで先頭から順に
;; 上書きしながらcdrで進む。戻り値はlist自体(呼び出し元が保持している変数は
;; そのまま更新後の内容を指す)。offset/value-listの長さに関わらずCスタックを
;; 消費しないwhileベースの実装(このインタプリタにTCOが無いことへの対応、
;; documents/fs.md参照)。
(defun %fat16-patch-bytes! (list offset value-list)
  (let ((cell list) (n offset))
    (while (> n 0)
      (setq cell (cdr cell))
      (setq n (- n 1)))
    (let ((values value-list))
      (while values
        (set-car cell (car values))
        (setq cell (cdr cell))
        (setq values (cdr values))))
    list))

;; (%fat16-split-into-chunks bytes chunk-size) : bytesをchunk-sizeごとのリストの
;; リストに分割する。最終チャンクが足りない分は0でパディングする。
;; %fat16-read-lba-list(M4)と同じ理由で、外側・内側ともLisp再帰を使わずwhileで
;; 書く(バイト数がファイルサイズに比例して増えるため)。%fat16-reverse-iterを
;; 再利用する。
(defun %fat16-split-into-chunks (bytes chunk-size)
  (let ((remaining bytes) (chunks nil))
    (while remaining
      (let ((rev-chunk nil) (count 0))
        (while (and remaining (< count chunk-size))
          (setq rev-chunk (cons (car remaining) rev-chunk))
          (setq remaining (cdr remaining))
          (setq count (+ count 1)))
        (while (< count chunk-size)
          (setq rev-chunk (cons 0 rev-chunk))
          (setq count (+ count 1)))
        (setq chunks (cons (%fat16-reverse-iter rev-chunk) chunks))))
    (%fat16-reverse-iter chunks)))

;; (%fat16-write-lba-list device lbas chunks) : lbasとchunks(同じ長さ)を並行して
;; whileで辿り、対応するペアをwrite-sectorする。%fat16-read-lba-list(M4)の
;; 書き込み対応版。1つでも失敗したら残りは書かず打ち切ってnil、全部成功でt。
(defun %fat16-write-lba-list (device lbas chunks)
  (let ((remaining-lbas lbas) (remaining-chunks chunks) (ok t))
    (while (and ok remaining-lbas remaining-chunks)
      (if (write-sector device (car remaining-lbas) (car remaining-chunks))
          (progn
            (setq remaining-lbas (cdr remaining-lbas))
            (setq remaining-chunks (cdr remaining-chunks)))
          (setq ok nil)))
    ok))

;; (%fat16-find-entry-in-sector bytes entry-offset entries-remaining name) : 1セクタ
;; 分のエントリをentry-offsetから順に調べ、名前がnameと一致する最初のエントリの
;; offsetを返す。0x00終端に到達したらシンボル'end、削除済み(0xE5)はスキップし
;; 次のエントリへ進む。entries-remainingを使い切ってもマッチしなければnil
;; (呼び出し元は次のセクタへ進む)。%fat16-parse-sector-entries(M2)と同じ
;; セクタ内再帰構造(最大16段、既に安全性実績あり)。
(defun %fat16-find-entry-in-sector (bytes entry-offset entries-remaining name)
  (if (<= entries-remaining 0)
      nil
      (let ((first-byte (elt bytes entry-offset)))
        (if (= first-byte 0)
            'end
            (if (and (/= first-byte #xE5)
                     (string= (%fat16-dir-entry-name bytes entry-offset) name))
                entry-offset
                (%fat16-find-entry-in-sector bytes (+ entry-offset 32) (- entries-remaining 1) name))))))

;; (%fat16-find-entry-location-scan device lbas name) : lbas(ディレクトリを構成する
;; セクタLBAのリスト、ルート・サブディレクトリのいずれでもよい)を先頭からwhileで
;; 走査し、nameと一致するエントリの(lba . offset-in-sector)を返す。0x00終端に
;; 到達するか読み込みに失敗した時点で走査を止める。見つからなければnil。
;; FAT16-M7bで(lba, 残りセクタ数)引数からLBAリスト+whileへ一般化した
;; (%fat16-scan-dir-entriesと同じ外側ループの一般化理由、documents/fs.md参照)。
(defun %fat16-find-entry-location-scan (device lbas name)
  (let ((remaining-lbas lbas) (result nil) (stopped nil))
    (while (and remaining-lbas (null result) (not stopped))
      (let ((bytes (read-sector device (car remaining-lbas))))
        (if (null bytes)
            (setq stopped t)
            (let ((found (%fat16-find-entry-in-sector bytes 0 16 name)))
              (if (eq found 'end)
                  (setq stopped t)
                  (if (null found)
                      (setq remaining-lbas (cdr remaining-lbas))
                      (setq result (cons (car remaining-lbas) found))))))))
    result))

;; (%fat16-cluster-count-for-bytes byte-length cluster-bytes) : byte-length分の
;; データを格納するのに必要なクラスタ数(切り上げ)。0byteなら0。
(defun %fat16-cluster-count-for-bytes (byte-length cluster-bytes)
  (if (= byte-length 0)
      0
      (if (= (mod byte-length cluster-bytes) 0)
          (div byte-length cluster-bytes)
          (+ (div byte-length cluster-bytes) 1))))

;; (%fat16-write-file-data device bpb start-cluster bytes) : start-clusterから
;; 始まる既存クラスタチェインへbytesを書き込む(セクタ単位に分割してwrite-sector)。
;; bytesがnil(0byte)の場合は書き込むクラスタが無いためそのままt。呼び出し元
;; (fat16-write-file)が必要クラスタ数と現クラスタ数の一致を確認済みという前提。
(defun %fat16-write-file-data (device bpb start-cluster bytes)
  (if (null bytes)
      t
      (let* ((chain (fat16-cluster-chain device bpb start-cluster))
             (lbas (%fat16-clusters-to-lbas bpb chain))
             (chunks (%fat16-split-into-chunks bytes (slot-value bpb 'bytes-per-sector))))
        (%fat16-write-lba-list device lbas chunks))))

;;; --- FAT16-M6b: クラスタ追加を伴うファイル拡張 ---

;; (%fat16-total-cluster-count bpb) : データ領域が確保できるクラスタ総数。
;; %fat16-find-free-clusterの探索上限を決めるために使う。
(defun %fat16-total-cluster-count (bpb)
  (div (- (slot-value bpb 'total-sectors) (%fat16-data-start-lba bpb))
       (slot-value bpb 'sectors-per-cluster)))

;; (%fat16-find-free-cluster device bpb) : クラスタ番号2から順にFATエントリが0
;; (未使用)になる最初のクラスタ番号をwhileで探す。上限はクラスタ総数+2(クラスタ
;; 番号は2始まりのため)。見つからなければ(ディスクフル)nil。データクラスタ数は
;; 数千に達し得るため、再帰は使わずwhileで書く(documents/fs.md参照)。
(defun %fat16-find-free-cluster (device bpb)
  (let ((cluster-no 2) (limit (+ (%fat16-total-cluster-count bpb) 2)) (found nil))
    (while (and (null found) (< cluster-no limit))
      (let ((entry (fat16-fat-entry device bpb cluster-no)))
        (if (and entry (= entry 0))
            (setq found cluster-no)
            (setq cluster-no (+ cluster-no 1)))))
    found))

;; (%fat16-set-fat-entry device bpb cluster-no value) : cluster-noのFATエントリを
;; valueで上書きする。num-fats全コピーへ同じ内容を書く(documents/fs.md、書き込みは
;; ミラー全体を更新する方針)。書き込んだセクタが%fat16-fat-sector-bytesの
;; キャッシュ(*fat16-fat-cache-lba*)と一致する場合はキャッシュを無効化し、次回
;; 読み込みで再読込させる。1つのコピーでも読み込み/書き込みに失敗したら残りは
;; 処理せず中断してnil、全コピー成功でt。
(defun %fat16-set-fat-entry (device bpb cluster-no value)
  (let* ((byte-offset (* cluster-no 2))
         (sector-offset (div byte-offset (slot-value bpb 'bytes-per-sector)))
         (offset-in-sector (mod byte-offset (slot-value bpb 'bytes-per-sector)))
         (value-bytes (%fat16-u16-to-bytes value)))
    (let ((fat-index 0) (ok t))
      (while (and ok (< fat-index (slot-value bpb 'num-fats)))
        (let* ((lba (+ (slot-value bpb 'reserved-sectors)
                        (* fat-index (slot-value bpb 'sectors-per-fat))
                        sector-offset))
               (sector-bytes (read-sector device lba)))
          (if (null sector-bytes)
              (setq ok nil)
              (progn
                (%fat16-patch-bytes! sector-bytes offset-in-sector value-bytes)
                (if (write-sector device lba sector-bytes)
                    (progn
                      (if (and (dynamic *fat16-fat-cache-lba*) (= (dynamic *fat16-fat-cache-lba*) lba))
                          (%%set-dynamic '*fat16-fat-cache-lba* nil))
                      (setq fat-index (+ fat-index 1)))
                    (setq ok nil))))))
      ok)))

;; (%fat16-allocate-clusters device bpb count) : 空きクラスタをcount個確保し、
;; 発見順のクラスタ番号リストを返す。確保したクラスタは次の探索で再び「空き」と
;; 誤認されないよう、確保直後にFATエントリへ終端マーカー(#xFFFF)を仮に書き込む
;; (呼び出し元が%fat16-link-clustersで実際のチェインへ後から繋ぎ直す前提)。
;; count個確保できなかった場合(ディスクフル、または書き込み失敗)はnil。その
;; 時点までに仮確保したクラスタの解放(フリーリストへ戻す)は行わない(ディスク
;; フル時のクラスタリークは許容する簡略化、documents/fs.md参照)。
(defun %fat16-allocate-clusters (device bpb count)
  (let ((i 0) (rev-clusters nil) (ok t))
    (while (and ok (< i count))
      (let ((cluster (%fat16-find-free-cluster device bpb)))
        (if (null cluster)
            (setq ok nil)
            (if (%fat16-set-fat-entry device bpb cluster #xFFFF)
                (progn
                  (setq rev-clusters (cons cluster rev-clusters))
                  (setq i (+ i 1)))
                (setq ok nil)))))
    (if ok
        (%fat16-reverse-iter rev-clusters)
        nil)))

;; (%fat16-last-elt list) : listの最後の要素をwhileで辿って返す(既存クラスタ
;; チェインの末尾クラスタ番号を得るために使う、要素数はファイルのクラスタ数に
;; 比例するためwhileで書く)。
(defun %fat16-last-elt (list)
  (let ((cell list))
    (while (cdr cell)
      (setq cell (cdr cell)))
    (car cell)))

;; (%fat16-link-clusters device bpb clusters) : clustersを先頭から順にFATエントリで
;; チェインする(各要素→次要素)。最後の要素のFATエントリは変更しない(呼び出し元が
;; %fat16-allocate-clustersで既に終端マーカーを設定済みという前提)。1つでも失敗
;; したら中断してnil、全部成功(または要素が1個以下で変更不要)ならt。
(defun %fat16-link-clusters (device bpb clusters)
  (let ((remaining clusters) (ok t))
    (while (and ok remaining (cdr remaining))
      (if (%fat16-set-fat-entry device bpb (car remaining) (car (cdr remaining)))
          (setq remaining (cdr remaining))
          (setq ok nil)))
    ok))

;; (%fat16-extend-file device bpb start-cluster old-cluster-count new-cluster-count
;;  bytes) : start-clusterから始まる既存チェイン(old-cluster-count個、0なら
;; まだクラスタ未確保=旧サイズ0)を、new-cluster-count個になるよう新規クラスタを
;; 確保・接続し、bytes全体を書き込む。呼び出し元はnew-cluster-count >
;; old-cluster-countであることを保証する前提(縮小はFAT16-M6bの対象外、
;; documents/fs.md参照)。成功時は新しいstart-cluster(旧チェインがあった場合は
;; 変化しないstart-cluster自身、旧チェインが無かった場合は新規確保した先頭クラスタ)
;; を返す。失敗時はnil。
(defun %fat16-extend-file (device bpb start-cluster old-cluster-count new-cluster-count bytes)
  (let ((new-clusters (%fat16-allocate-clusters device bpb (- new-cluster-count old-cluster-count))))
    (if (null new-clusters)
        nil
        (if (not (%fat16-link-clusters device bpb new-clusters))
            nil
            (let* ((had-old-chain (> old-cluster-count 0))
                   (linked-to-old (if had-old-chain
                                       (%fat16-set-fat-entry device bpb
                                         (%fat16-last-elt (fat16-cluster-chain device bpb start-cluster))
                                         (car new-clusters))
                                       t))
                   (first-cluster (if had-old-chain start-cluster (car new-clusters))))
              (if (not linked-to-old)
                  nil
                  (if (%fat16-write-file-data device bpb first-cluster bytes)
                      first-cluster
                      nil)))))))

;; (%fat16-finish-directory-update device dir-lba dir-bytes dir-offset
;;  new-start-cluster new-size) : ディレクトリエントリのstart-cluster(offset+26)と
;; size(offset+28)フィールドを書き換えてセクタを書き込む。M6a(サイズのみ変化、
;; start-clusterは同じ値を渡せば実質変更なし)・M6b(start-clusterも変わり得る)の
;; 両方から共通で使う。
(defun %fat16-finish-directory-update (device dir-lba dir-bytes dir-offset new-start-cluster new-size)
  (progn
    (%fat16-patch-bytes! dir-bytes (+ dir-offset 26) (%fat16-u16-to-bytes new-start-cluster))
    (%fat16-patch-bytes! dir-bytes (+ dir-offset 28) (%fat16-u32-to-bytes new-size))
    (write-sector device dir-lba dir-bytes)))

;; (fat16-write-file device path bytes) : path("/NAME.EXT"、または"/DOCS/NAME.EXT"
;; のような多階層パス、FAT16-M7b)の既存ファイルへbytes(fixnum 0-255のリスト)を
;; 上書きする。必要クラスタ数が現在のクラスタ数と同じ場合はFAT16-M6aの経路
;; (データ→sizeフィールドのみ更新)、必要クラスタ数が増える場合はFAT16-M6bの経路
;; (新規クラスタ確保・FATチェイン延長→データ→start-cluster/sizeフィールド更新)で
;; 書き込む。必要クラスタ数が減る場合(縮小)はFAT16-M6bの対象外としてnilを返し、
;; 既存データ/ディレクトリエントリは変更しない。パスが解決できない・エントリが
;; 見つからない・各段階の書き込みに失敗した場合もnil、成功時はt。パス解決の実体は
;; %fat16-resolve-file(FAT16-M7a節)。
(defun fat16-write-file (device path bytes)
  (let* ((bpb (fat16-read-bpb device)))
    (if (null bpb)
        nil
        (let* ((resolved (%fat16-resolve-file device bpb path))
               (loc (if (null resolved)
                        nil
                        (%fat16-find-entry-location-scan device (car resolved) (cdr resolved)))))
          (if (null loc)
              nil
              (let* ((dir-lba (car loc))
                     (dir-offset (cdr loc))
                     (dir-bytes (read-sector device dir-lba)))
                (if (null dir-bytes)
                    nil
                    (let* ((start-cluster (%fat16-u16 dir-bytes (+ dir-offset 26)))
                           (cluster-bytes (* (slot-value bpb 'sectors-per-cluster) (slot-value bpb 'bytes-per-sector)))
                           (current-cluster-count (if (= start-cluster 0)
                                                        0
                                                        (length (fat16-cluster-chain device bpb start-cluster))))
                           (required-cluster-count (%fat16-cluster-count-for-bytes (length bytes) cluster-bytes)))
                      (cond
                        ((= required-cluster-count current-cluster-count)
                         (if (%fat16-write-file-data device bpb start-cluster bytes)
                             (%fat16-finish-directory-update device dir-lba dir-bytes dir-offset start-cluster (length bytes))
                             nil))
                        ((> required-cluster-count current-cluster-count)
                         (let ((new-start-cluster (%fat16-extend-file device bpb start-cluster current-cluster-count required-cluster-count bytes)))
                           (if (null new-start-cluster)
                               nil
                               (%fat16-finish-directory-update device dir-lba dir-bytes dir-offset new-start-cluster (length bytes)))))
                        (t nil))))))))))

;;; --- FAT16-M6c: 新規ファイル作成 ---

;; (%fat16-upcase-char-code code) : 小文字ASCII(97-122)のcode(char-codeの戻り値)を
;; 大文字(-32)に変換する。それ以外のコードはそのまま返す(8.3名は大文字が規約)。
(defun %fat16-upcase-char-code (code)
  (if (and (>= code 97) (<= code 122))
      (- code 32)
      code))

;; (%fat16-8.3-field-bytes s field-len) : 文字列sを大文字化してfield-len(8か3)byte
;; の固定長フィールドへ変換する。sがfield-lenを超えるとnil(ロングファイルネーム
;; 相当、documents/fs.mdの設計でスコープ外)。不足分は空白(32)でパディングする。
;; sの長さはfield-len(最大8)以下しか受け付けないため、再帰・while問わず安全な
;; 範囲だが、他のヘルパーとの一貫性のためwhileで書く。
(defun %fat16-8.3-field-bytes (s field-len)
  (let ((n (length s)))
    (if (> n field-len)
        nil
        (let ((i 0) (rev nil))
          (while (< i n)
            (setq rev (cons (%fat16-upcase-char-code (char-code (elt s i))) rev))
            (setq i (+ i 1)))
          (let ((bytes (%fat16-reverse-iter rev)) (pad (- field-len n)))
            (while (> pad 0)
              (setq bytes (append bytes (list 32)))
              (setq pad (- pad 1)))
            bytes)))))

;; (%fat16-name-to-8.3 name) : "NAME.EXT"形式のnameを、FAT16の8.3形式11byte
;; フィールド(base8byte+ext3byte、大文字・空白パディング)へ変換する。最初の"."で
;; base/extに分割し、それぞれ%fat16-8.3-field-bytesへ渡す。2個目の"."が見つかる
;; 場合(複数ドット)、またはbase>8/ext>3の場合はロングファイルネーム相当として
;; nilを返す(documents/fs.mdの設計でスコープ外と明記済み)。"."が無い場合は
;; ext=""として扱う(拡張子無しファイル)。
(defun %fat16-name-to-8.3 (name)
  (let ((dot-pos (char-index #\. name)))
    (if (null dot-pos)
        (let ((base-bytes (%fat16-8.3-field-bytes name 8))
              (ext-bytes (%fat16-8.3-field-bytes "" 3)))
          (if (and base-bytes ext-bytes) (append base-bytes ext-bytes) nil))
        (if (char-index #\. name (+ dot-pos 1))
            nil
            (let* ((base (subseq name 0 dot-pos))
                   (ext (subseq name (+ dot-pos 1) (length name)))
                   (base-bytes (%fat16-8.3-field-bytes base 8))
                   (ext-bytes (%fat16-8.3-field-bytes ext 3)))
              (if (and base-bytes ext-bytes) (append base-bytes ext-bytes) nil))))))

;; (%fat16-find-free-slot-in-sector bytes entry-offset entries-remaining) : 1セクタ
;; 分のエントリをentry-offsetから順に調べ、先頭バイトが0x00(終端)または0xE5
;; (削除済み、再利用可)の最初のエントリのoffsetを返す。見つからなければnil
;; (呼び出し元は次のセクタへ進む)。%fat16-find-entry-in-sectorと同じセクタ内
;; 再帰構造(最大16段、既に安全性実績あり)。
(defun %fat16-find-free-slot-in-sector (bytes entry-offset entries-remaining)
  (if (<= entries-remaining 0)
      nil
      (let ((first-byte (elt bytes entry-offset)))
        (if (or (= first-byte 0) (= first-byte #xE5))
            entry-offset
            (%fat16-find-free-slot-in-sector bytes (+ entry-offset 32) (- entries-remaining 1))))))

;; (%fat16-find-free-slot-scan device lbas) : lbas(ディレクトリを構成するセクタ
;; LBAのリスト、ルート・サブディレクトリのいずれでもよい)を先頭からwhileで走査し、
;; 最初の空きスロット(削除済み0xE5、または終端0x00)の(lba . offset-in-sector)を
;; 返す。見つからなければ(ディレクトリ満杯)nil。0x00スロットを再利用しても、
;; 直後のエントリは元から0x00のままなので走査の終端条件は保たれる(0x00の直前に
;; 別の0x00は存在しないため、特別な後始末は不要)。FAT16-M7bで(lba, 残りセクタ数)
;; 引数からLBAリスト+whileへ一般化した。
(defun %fat16-find-free-slot-scan (device lbas)
  (let ((remaining-lbas lbas) (result nil))
    (while (and remaining-lbas (null result))
      (let ((bytes (read-sector device (car remaining-lbas))))
        (if (null bytes)
            (setq remaining-lbas nil)
            (let ((found (%fat16-find-free-slot-in-sector bytes 0 16)))
              (if (null found)
                  (setq remaining-lbas (cdr remaining-lbas))
                  (setq result (cons (car remaining-lbas) found)))))))
    result))

;; (%fat16-build-dir-entry-bytes name-bytes attr start-cluster size) : 32byteの
;; 新規ディレクトリエントリ全体を構築する。name-bytesは%fat16-name-to-8.3の戻り値
;; (11byte)、予約フィールド(offset 12-25の14byte)は0埋め。
(defun %fat16-build-dir-entry-bytes (name-bytes attr start-cluster size)
  (append name-bytes
          (list attr 0 0 0 0 0 0 0 0 0 0 0 0 0 0)
          (%fat16-u16-to-bytes start-cluster)
          (%fat16-u32-to-bytes size)))

;; (%fat16-allocate-new-file-data device bpb required-cluster-count bytes) :
;; 新規ファイル用のクラスタを確保してbytesを書き込む。required-cluster-countが0
;; (空ファイル)ならクラスタを確保せずstart-cluster=0を返す。それ以外は
;; %fat16-extend-file(FAT16-M6b)をold-cluster-count=0として呼び、「既存チェイン無し
;; からの新規確保」を再利用する(documents/fs.mdの設計方針通り)。失敗時はnil。
(defun %fat16-allocate-new-file-data (device bpb required-cluster-count bytes)
  (if (= required-cluster-count 0)
      0
      (%fat16-extend-file device bpb 0 0 required-cluster-count bytes)))

;; (fat16-create-file device path bytes) : path("/NAME.EXT"、または
;; "/DOCS/NAME.EXT"のような多階層パス、FAT16-M7b)に新規ファイルを作成しbytes
;; (fixnum 0-255のリスト、空ならnil)を書き込む。親ディレクトリが解決できない場合、
;; 同名エントリが既に存在する場合(上書きはfat16-write-fileの役割)、8.3名変換に
;; 失敗した場合(ロングファイルネーム相当)、空きディレクトリスロットが無い場合
;; (ディレクトリ満杯)、クラスタ確保に失敗した場合(ディスクフル)はいずれもnilを
;; 返し、何も変更しない。属性は#x20(ARCHIVE、通常ファイルの標準値)固定。パス
;; 解決の実体は%fat16-resolve-file(FAT16-M7a節)。
(defun fat16-create-file (device path bytes)
  (let* ((bpb (fat16-read-bpb device)))
    (if (null bpb)
        nil
        (let ((resolved (%fat16-resolve-file device bpb path)))
          (if (null resolved)
              nil
              (let* ((lbas (car resolved))
                     (name (cdr resolved)))
                (if (%fat16-find-entry-location-scan device lbas name)
                    nil
                    (let ((name-bytes (%fat16-name-to-8.3 name)))
                      (if (null name-bytes)
                          nil
                          (let ((slot (%fat16-find-free-slot-scan device lbas)))
                            (if (null slot)
                                nil
                                (let* ((cluster-bytes (* (slot-value bpb 'sectors-per-cluster) (slot-value bpb 'bytes-per-sector)))
                                       (required-cluster-count (%fat16-cluster-count-for-bytes (length bytes) cluster-bytes))
                                       (start-cluster (%fat16-allocate-new-file-data device bpb required-cluster-count bytes)))
                                  (if (null start-cluster)
                                      nil
                                      (let* ((slot-lba (car slot))
                                             (slot-offset (cdr slot))
                                             (slot-bytes (read-sector device slot-lba))
                                             (entry-bytes (%fat16-build-dir-entry-bytes name-bytes #x20 start-cluster (length bytes))))
                                        (if (null slot-bytes)
                                            nil
                                            (progn
                                              (%fat16-patch-bytes! slot-bytes slot-offset entry-bytes)
                                              (write-sector device slot-lba slot-bytes)))))))))))))))))

;;; --- FAT16-M7a: サブディレクトリ対応(読み込み・パス解決) ---
;;
;; ルートディレクトリは固定サイズ(セクタ数がbpbから一意に決まる)だが、
;; サブディレクトリはクラスタチェインを持つ通常ファイルと同じ構造で、セクタ数が
;; ディスク使用量に比例して増え得る。旧%fat16-scan-root-dir(セクタ数について
;; Lisp再帰)は「ルートが小さい固定サイズだから安全」という前提の上に成り立って
;; おり、この前提はサブディレクトリには通用しない(documents/fs.md、
;; eval_no_tco_interpreter_stack_limit)。このため「ディレクトリ=セクタLBAの
;; 平坦なリスト」という抽象に一般化し、走査の外側ループを常にwhileで書く。

;; (%fat16-root-dir-lbas bpb) : ルートディレクトリを構成する全セクタのLBAリスト。
(defun %fat16-root-dir-lbas (bpb)
  (%fat16-lba-range (fat16-root-dir-lba bpb) (%fat16-root-dir-sector-count bpb)))

;; (%fat16-subdir-lbas device bpb start-cluster) : start-clusterから始まるサブ
;; ディレクトリを構成する全セクタのLBAリスト(クラスタチェインを辿って展開)。
(defun %fat16-subdir-lbas (device bpb start-cluster)
  (%fat16-clusters-to-lbas bpb (fat16-cluster-chain device bpb start-cluster)))

;; (%fat16-scan-dir-entries device lbas) : lbas(ディレクトリを構成するセクタLBAの
;; リスト、ルートの固定範囲でもサブディレクトリのクラスタチェイン展開でもよい)を
;; 先頭からwhileで走査し、有効なdir-entryのリストを返す。0x00終端に到達した時点で
;; 走査を止める。セクタ内(最大16エントリ)もwhileで走査し、有効エントリを
;; consで逆順に積んでから最後に1回だけ%fat16-reverse-iterで正順化する
;; (成長していく蓄積リストにappendで結合する実装は、セクタ数が多いサブ
;; ディレクトリでコストが増えるため避ける)。read-sectorが失敗した場合はそれまでに
;; 集めたエントリを捨ててnil(既存のIDE層と同じ「失敗時nil」の慣習)。
(defun %fat16-scan-dir-entries (device lbas)
  (let ((remaining-lbas lbas) (rev-entries nil) (stopped nil) (ok t))
    (while (and ok remaining-lbas (not stopped))
      (let ((bytes (read-sector device (car remaining-lbas))))
        (if (null bytes)
            (setq ok nil)
            (progn
              (let ((offset 0) (i 0))
                (while (and (not stopped) (< i 16))
                  (let ((parsed (%fat16-dir-entry-at bytes offset)))
                    (if (eq parsed 'end)
                        (setq stopped t)
                        (progn
                          (if (not (null parsed)) (setq rev-entries (cons parsed rev-entries)))
                          (setq offset (+ offset 32))
                          (setq i (+ i 1)))))))
              (setq remaining-lbas (cdr remaining-lbas))))))
    (if ok (%fat16-reverse-iter rev-entries) nil)))

;; (%fat16-split-path path) : "/DOCS/SUB/README.TXT"のようなpathを"/"区切りで
;; 非空要素のリスト("DOCS" "SUB" "README.TXT")に分解する。連続する"/"・先頭・
;; 末尾の"/"は空要素として無視される。"/"のみのパスはnil(ルート自身)。パスの
;; 深さは小さくデータサイズに比例しないため、whileで書くが再帰でも安全な範囲。
(defun %fat16-split-path (path)
  (let ((start 0) (len (length path)) (rev-parts nil))
    (while (< start len)
      (let* ((slash-pos (char-index #\/ path start))
             (end (if slash-pos slash-pos len)))
        (if (> end start)
            (setq rev-parts (cons (subseq path start end) rev-parts)))
        (setq start (+ end 1))))
    (%fat16-reverse-iter rev-parts)))

;; (%fat16-butlast list) : listから末尾の要素を除いた部分を返す。パス要素の
;; リスト(パスの階層数、小さい)に対して使うためLisp再帰でよい(ファイル/ディスク
;; サイズに比例しないためeval_no_tco_interpreter_stack_limitの対象外)。
(defun %fat16-butlast (list)
  (if (or (null list) (null (cdr list)))
      nil
      (cons (car list) (%fat16-butlast (cdr list)))))

;; (%fat16-resolve-dir device bpb components) : componentsをルートから順に
;; サブディレクトリとして辿り、(最終ディレクトリのLBAリスト . 最終ディレクトリの
;; start-cluster)を返す。componentsがnilならルート自身((ルートLBAリスト . 0))。
;; 途中のいずれかの段でエントリが見つからない・ディレクトリでない(属性0x10無し)
;; 場合、または読み込み失敗の場合はnil。ルートの「start-cluster」は本来存在しない
;; ため0を使う(サブディレクトリの".."がルートを指す場合の規約と同じ、FAT16-M7c
;; で利用)。
(defun %fat16-resolve-dir (device bpb components)
  (let ((lbas (%fat16-root-dir-lbas bpb)) (cluster 0) (remaining components) (ok t))
    (while (and ok remaining)
      (let ((entries (%fat16-scan-dir-entries device lbas)))
        (if (null entries)
            (setq ok nil)
            (let ((entry (%fat16-find-dir-entry entries (car remaining))))
              (if (or (null entry) (= (logand (slot-value entry 'attr) #x10) 0))
                  (setq ok nil)
                  (progn
                    (setq cluster (slot-value entry 'start-cluster))
                    (setq lbas (%fat16-subdir-lbas device bpb cluster))
                    (setq remaining (cdr remaining))))))))
    (if ok (cons lbas cluster) nil)))

;; (%fat16-resolve-dir-lbas device bpb components) : %fat16-resolve-dirの結果から
;; LBAリストのみを取り出す薄いラッパー(fat16-read-dir用)。
(defun %fat16-resolve-dir-lbas (device bpb components)
  (let ((resolved (%fat16-resolve-dir device bpb components)))
    (if (null resolved) nil (car resolved))))

;; (%fat16-resolve-file device bpb path) : pathを分解し、最後の要素をファイル名
;; として残し、それ以前を%fat16-resolve-dir-lbasでディレクトリとして解決する。
;; 戻り値は(親ディレクトリのLBAリスト . ファイル名)。パスが"/"のみ(ファイル名が
;; 無い)、または親ディレクトリの解決に失敗した場合はnil。fat16-read-file/
;; fat16-write-file/fat16-create-fileが共通で使う。
(defun %fat16-resolve-file (device bpb path)
  (let ((components (%fat16-split-path path)))
    (if (null components)
        nil
        (let ((lbas (%fat16-resolve-dir-lbas device bpb (%fat16-butlast components))))
          (if (null lbas)
              nil
              (cons lbas (%fat16-last-elt components)))))))

;;; --- FAT16-M7c: fat16-create-directory(mkdir)新設 ---
;;
;; 新規サブディレクトリは既存クラスタの再利用ではなく確保直後のクラスタなので、
;; ディスク上の残存データが誤って有効なディレクトリエントリと誤認されないよう、
;; "."/".."エントリ以外を明示的にゼロ埋めしてから書き込む(%fat16-write-file-data
;; のような既存クラスタ上書きの再利用はできない)。

;; (%fat16-zero-byte-list n) : 長さnの、全要素が0のfixnumリストをwhileで構築する
;; (%fat16-test-make-byte-listと同型のパターン、本番用としてfat16.lispに追加)。
;; クラスタサイズに比例して大きくなり得るためwhileで書く。
(defun %fat16-zero-byte-list (n)
  (let ((i 0) (acc nil))
    (while (< i n)
      (setq acc (cons 0 acc))
      (setq i (+ i 1)))
    acc))

;; (%fat16-dot-entry-name-bytes) : "."エントリの11byte名フィールド("."+空白10個)。
;; %fat16-name-to-8.3は"."単体の名前を想定していない(最初の"."で分割するため
;; base=""になる)ため、専用の定数リストを用意する。
(defun %fat16-dot-entry-name-bytes ()
  (list 46 32 32 32 32 32 32 32 32 32 32))

;; (%fat16-dotdot-entry-name-bytes) : ".."エントリの11byte名フィールド(".."+空白9個)。
(defun %fat16-dotdot-entry-name-bytes ()
  (list 46 46 32 32 32 32 32 32 32 32 32))

;; (%fat16-init-dir-cluster-bytes bpb own-cluster parent-cluster) : 新規ディレクトリ用
;; に確保した1クラスタ分の初期化バイト列を構築する。先頭32byteが"."エントリ
;; (start-cluster=own-cluster)、続く32byteが".."エントリ(start-cluster=
;; parent-cluster、親がルートなら%fat16-resolve-dirの規約通り0)、残りは
;; %fat16-zero-byte-listでゼロ埋め。appendの各引数はdot/dotdot(32byte)か最後の
;; 引数(ゼロ埋め、%append-listsは最後の引数をそのまま返すのでコピーコストなし)
;; のみで、クラスタサイズに関わらずCスタックを消費しない
;; (%fat16-build-dir-entry-bytesと同じ安全なappendの使い方、documents/fs.md参照)。
(defun %fat16-init-dir-cluster-bytes (bpb own-cluster parent-cluster)
  (let* ((cluster-bytes (* (slot-value bpb 'sectors-per-cluster) (slot-value bpb 'bytes-per-sector)))
         (dot-entry (%fat16-build-dir-entry-bytes (%fat16-dot-entry-name-bytes) #x10 own-cluster 0))
         (dotdot-entry (%fat16-build-dir-entry-bytes (%fat16-dotdot-entry-name-bytes) #x10 parent-cluster 0))
         (padding (%fat16-zero-byte-list (- cluster-bytes 64))))
    (append dot-entry dotdot-entry padding)))

;; (fat16-create-directory device path) : path("/NAME"、または"/DOCS/NAME"のような
;; 多階層パス)に新規サブディレクトリを作成する。親ディレクトリが解決できない場合、
;; 同名エントリが既に存在する場合、8.3名変換に失敗した場合、空きディレクトリ
;; スロットが無い場合(ディレクトリ満杯)、クラスタ確保に失敗した場合(ディスク
;; フル)、確保したクラスタへの初期化データ書き込みに失敗した場合はいずれもnilを
;; 返し、何も変更しない。属性は#x10(ディレクトリ)固定。fat16-create-file(M6c)と
;; 同じ失敗伝播パターンで、新規クラスタ確保後は%fat16-init-dir-cluster-bytesで
;; "."/".."エントリを構築してから書き込む点のみが異なる。パスが"/"自身(ルートの
;; 作成、componentsがnil)の場合もnil。
(defun fat16-create-directory (device path)
  (let* ((bpb (fat16-read-bpb device)))
    (if (null bpb)
        nil
        (let ((components (%fat16-split-path path)))
          (if (null components)
              nil
              (let* ((name (%fat16-last-elt components))
                     (resolved-parent (%fat16-resolve-dir device bpb (%fat16-butlast components))))
                (if (null resolved-parent)
                    nil
                    (let* ((parent-lbas (car resolved-parent))
                           (parent-cluster (cdr resolved-parent)))
                      (if (%fat16-find-entry-location-scan device parent-lbas name)
                          nil
                          (let ((name-bytes (%fat16-name-to-8.3 name)))
                            (if (null name-bytes)
                                nil
                                (let ((slot (%fat16-find-free-slot-scan device parent-lbas)))
                                  (if (null slot)
                                      nil
                                      (let ((new-clusters (%fat16-allocate-clusters device bpb 1)))
                                        (if (null new-clusters)
                                            nil
                                            (let* ((new-cluster (car new-clusters))
                                                   (cluster-bytes-list (%fat16-init-dir-cluster-bytes bpb new-cluster parent-cluster))
                                                   (new-lbas (%fat16-clusters-to-lbas bpb new-clusters))
                                                   (chunks (%fat16-split-into-chunks cluster-bytes-list (slot-value bpb 'bytes-per-sector))))
                                              (if (not (%fat16-write-lba-list device new-lbas chunks))
                                                  nil
                                                  (let* ((slot-lba (car slot))
                                                         (slot-offset (cdr slot))
                                                         (slot-bytes (read-sector device slot-lba))
                                                         (entry-bytes (%fat16-build-dir-entry-bytes name-bytes #x10 new-cluster 0)))
                                                    (if (null slot-bytes)
                                                        nil
                                                        (progn
                                                          (%fat16-patch-bytes! slot-bytes slot-offset entry-bytes)
                                                          (write-sector device slot-lba slot-bytes)))))))))))))))))))))
