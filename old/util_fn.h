#ifndef __UTIL_FN_H__
#define __UTIL_FN_H__

#include <linux/types.h>


#define NEXT_ARG() do { argv++; if (--argc <= 0) incomplete_command(); } while(0)
#define NEXT_ARG_OK() (argc - 1 > 0) 
#define PREV_ARG() do { argv--; argc++; } while(0)

typedef struct
{       
	__u8 family;
	__u8 bytelen;
	__s16 bitlen;
	__u32 flags;
	__u32 data[4];
} inet_prefix; 



#endif //__UTIL_FN_H__
