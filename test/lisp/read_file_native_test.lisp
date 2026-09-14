;; test/lisp/read_file_native_test.lisp
;;
;; [性能測定] documents/performance-measurement.md「read-file-into-vector-native」
;; 参照。READ-FILE-INTO-VECTOR-NATIVE(mount.c、Lisp呼び出し規約を経由しない
;; 素のC実装、FAT16のルート直下のファイルのみ対応)の正当性を確認する。
;; 既存のread-file-into-vector(AOT版)と完全に同じ結果を返すことを、
;; 0byte・クラスタサイズ未満・クラスタサイズちょうど・複数クラスタにまたがる
;; サイズの各パターンで確認する。FAT16_DISK_IMG(Makefile)の固定フィクスチャ
;; (HELLO.TXT=0byte、TEST.LSP=19byte、WRITE1.TXT=2048byte=ちょうど1クラスタ、
;; BIG.TXT=2500byte=複数クラスタ)をそのまま使う。

(mount "/mnt" 'blk0 ':fat16)

;;; --- 0byteファイル: read-file-into-vectorと同じくnil(既知の制約、揃える) ---
(assert-equal nil (read-file-into-vector "/mnt/HELLO.TXT"))
(assert-equal nil (read-file-into-vector-native "/mnt/HELLO.TXT"))

;;; --- クラスタサイズ未満(19byte) ---
(defglobal rfn-test-expected-small (read-file-into-vector "/mnt/TEST.LSP"))
(defglobal rfn-test-native-small (read-file-into-vector-native "/mnt/TEST.LSP"))
(assert-equal t (if rfn-test-expected-small t nil))
(assert-equal t (if rfn-test-native-small t nil))
(assert-equal (length rfn-test-expected-small) (length rfn-test-native-small))
(assert-equal t (equal rfn-test-expected-small rfn-test-native-small))

;;; --- ちょうど1クラスタ(2048byte、WRITE1.TXT) ---
(defglobal rfn-test-expected-cluster (read-file-into-vector "/mnt/WRITE1.TXT"))
(defglobal rfn-test-native-cluster (read-file-into-vector-native "/mnt/WRITE1.TXT"))
(assert-equal 2048 (length rfn-test-expected-cluster))
(assert-equal 2048 (length rfn-test-native-cluster))
(assert-equal t (equal rfn-test-expected-cluster rfn-test-native-cluster))

;;; --- 複数クラスタにまたがる(2500byte、BIG.TXT) ---
(defglobal rfn-test-expected-multi (read-file-into-vector "/mnt/BIG.TXT"))
(defglobal rfn-test-native-multi (read-file-into-vector-native "/mnt/BIG.TXT"))
(assert-equal 2500 (length rfn-test-expected-multi))
(assert-equal 2500 (length rfn-test-native-multi))
(assert-equal t (equal rfn-test-expected-multi rfn-test-native-multi))

;;; --- 存在しないパス: nil ---
(assert-equal nil (read-file-into-vector-native "/mnt/NOSUCH.TXT"))

;;; --- サブディレクトリ配下: 本実装のスコープ外(ルート直下のみ対応)なのでnil ---
;; (read-file-into-vectorは正規実装なのでNESTED.TXTを問題なく読める。
;; read-file-into-vector-nativeはサブディレクトリ非対応のスコープ限定を
;; 意図通りnilで返すことを確認する)
(assert-equal t (if (read-file-into-vector "/mnt/SUBDIR/NESTED.TXT") t nil))
(assert-equal nil (read-file-into-vector-native "/mnt/SUBDIR/NESTED.TXT"))
