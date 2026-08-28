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
