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

;; (%fat16-parse-sector-entries bytes entry-offset entries-remaining) : 1セクタ
;; 分のディレクトリエントリ(entries-remaining個、通常16)を先頭から順にパースし、
;; (entries . stopped)を返す。entriesは有効なdir-entryのリスト(削除済みは
;; 除外済み)、stoppedは0x00終端に到達済みならt(この場合、呼び出し元は次の
;; セクタへ進んではならない)。
(defun %fat16-parse-sector-entries (bytes entry-offset entries-remaining)
  (if (<= entries-remaining 0)
      (cons nil nil)
      (let ((parsed (%fat16-dir-entry-at bytes entry-offset)))
        (if (eq parsed 'end)
            (cons nil t)
            (let ((rest (%fat16-parse-sector-entries bytes (+ entry-offset 32) (- entries-remaining 1))))
              (cons (if (null parsed) (car rest) (cons parsed (car rest)))
                    (cdr rest)))))))

;; (%fat16-scan-root-dir device lba sectors-remaining) : lbaからsectors-remaining
;; セクタ分のルートディレクトリを読み、有効なdir-entryのリストを返す。0x00終端に
;; 到達した時点で残りのセクタは読まずに走査を終える。read-sectorが失敗した場合は
;; それまでに集めたエントリは捨ててnilを返す(部分結果を返さない、既存のIDE層と
;; 同じ「失敗時nil」の慣習)。
(defun %fat16-scan-root-dir (device lba sectors-remaining)
  (if (<= sectors-remaining 0)
      nil
      (let ((bytes (read-sector device lba)))
        (if (null bytes)
            nil
            (let ((result (%fat16-parse-sector-entries bytes 0 16)))
              (if (cdr result)
                  (car result)
                  (append (car result)
                          (%fat16-scan-root-dir device (+ lba 1) (- sectors-remaining 1)))))))))

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

;; (fat16-read-dir device path) : ルートディレクトリ("/")のエントリ一覧を
;; ("NAME" :file/:dir size)の形のリストで返す。pathは現状"/"固定でよく
;; (サブディレクトリ対応はFAT16-M7、documents/fs.md)、引数として受け取るのみで
;; 未使用。BPBが読めない場合はnil。
(defun fat16-read-dir (device path)
  (let ((bpb (fat16-read-bpb device)))
    (if (null bpb)
        nil
        (%fat16-dir-entries-to-display-list
          (%fat16-scan-root-dir device
                                 (fat16-root-dir-lba bpb)
                                 (%fat16-root-dir-sector-count bpb))))))

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
(defun fat16-cluster-chain (device bpb start-cluster)
  (let ((entry (fat16-fat-entry device bpb start-cluster)))
    (if (null entry)
        (list start-cluster)
        (if (>= entry #xFFF8)
            (list start-cluster)
            (cons start-cluster (fat16-cluster-chain device bpb entry))))))

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

;; (%fat16-path-to-name path) : "/NAME.EXT"形式のpathから先頭の"/"を除いた名前を
;; 取り出す。サブディレクトリ対応はFAT16-M7、現状は"/"直下の単一階層のみ想定。
(defun %fat16-path-to-name (path)
  (subseq path 1 (length path)))

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
;; 順に並べた1本のリストへ展開する。appendの第一引数は常に1クラスタ分
;; (sectors-per-cluster個、小さい)なので、クラスタ数に関わらずスタックが
;; 深くならない。
(defun %fat16-clusters-to-lbas (bpb clusters)
  (if (null clusters)
      nil
      (append (%fat16-lba-range (fat16-cluster-to-lba bpb (car clusters))
                                 (slot-value bpb 'sectors-per-cluster))
              (%fat16-clusters-to-lbas bpb (cdr clusters)))))

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

;; (fat16-read-file device path) : path("/NAME.EXT"形式、単一階層のみ)のファイル
;; 本体をfixnum(0-255)のリストとして返す。read-sector/write-sectorと同じ
;; バイト列表現(全体方針: FAT16層のバイト列操作はfixnumリストのまま行う、文字列化
;; しない)。エントリが見つからない場合・クラスタ読み込みに失敗した場合はnil。
;; 0byteファイル(size=0)はクラスタを辿らずそのままnil(=空リスト)を返す。
(defun fat16-read-file (device path)
  (let ((bpb (fat16-read-bpb device)))
    (if (null bpb)
        nil
        (let ((entry (%fat16-find-dir-entry
                       (%fat16-scan-root-dir device
                                              (fat16-root-dir-lba bpb)
                                              (%fat16-root-dir-sector-count bpb))
                       (%fat16-path-to-name path))))
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
                          (subseq bytes 0 size))))))))))
