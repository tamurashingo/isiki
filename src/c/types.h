#ifndef _TYPES_H_
#define _TYPES_H_

typedef unsigned char UINT8;
typedef unsigned short CHAR16;
typedef unsigned short UINT16;
typedef unsigned int UINT32;
typedef unsigned long long UINT64;
typedef unsigned long long UINTN;


/** Lispが管理するアドレス */
typedef UINT64 lisp_addr_t;
/** LispObjectのアドレス(要untag) */
typedef UINT64 lisp_val_t;


#endif /* _TYPES_H_ */
