;; FAT32-M9: Primary IDE HDD(BOOT_FAT32_IMG)がos_block_device_probe_allから
;; ide-primary-masterとして検出できていること、およびPrimary追加によって
;; 既存のblk0=ide-secondary-master(fat16_test.lisp/fat32_test.lispが
;; (%device-handle 'blk0)で前提にしている登録順)が壊れていないことを確認する。

;; (%fat32-primary-boot-find-by-name name alist) : *devices*と同形式のalistから
;; :nameがnameと一致する最初のinfoプリストを返す(無ければnil)。
(defun %fat32-primary-boot-find-by-name (name alist)
  (if (null alist)
      nil
      (if (equal (%plist-get (cdr (car alist)) ':name "") name)
          (cdr (car alist))
          (%fat32-primary-boot-find-by-name name (cdr alist)))))

;; ide-secondary-masterが引き続きblk0であること(既存テストの前提の回帰確認)。
(assert-equal "ide-secondary-master"
              (%plist-get (cdr (assoc 'blk0 (dynamic *devices*))) ':name ""))

;; ide-primary-masterがPrimaryチャネルのプローブにより*devices*へ登録されていること。
(let ((primary (%fat32-primary-boot-find-by-name "ide-primary-master" (dynamic *devices*))))
  (progn
    (assert-equal t (if primary t nil))
    (assert-equal t (if (%plist-get primary ':handle nil) t nil))
    (assert-equal t (> (%plist-get primary ':total-sectors 0) 0))))
