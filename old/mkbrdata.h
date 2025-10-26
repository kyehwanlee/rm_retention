#ifndef __MKBRDATA_H__
#define __MKBRDATA_H__

#define TRUE    1
#define FALSE   0

#define DEBUG(fmt, args...)\
   if(gDebug) debug_print(__FILE__, __FUNCTION__, __LINE__, fmt, ## args);
#define _DEBUG_ FALSE




typedef struct tagMRT_FORM
{

	char type[16];		// 1: TYPE
	char timestamp[12];	// 2: TIME
	char status[7]; 	// 3: 'B' taBle dump | 'A' update Announcement | 'W' update Withdrawl | 'STATUS' ???
	char ip_peer[20];					// 4: PEER
	char peerAS[6]; 					// 5: peerAS
	char prefix[20];					// 6: PREFIX
	char *as_path;						// 7: AS PATH
	char origin[10];					// 8: ORIGIN
	char ip_nexthop[20];				// 9: NEXT_HOP
	unsigned short local_pref;			// 10: LOCAL_PREF ('0' if not set)
	unsigned short multi_exit_disc; 	// 11: ( '0' if not set)
	char* community;					// 12: COMMUNITY
	char status_flag[4];				// 13: STATUS/FLAG (atomic_aggregator bit? 'AG' else 'NAG')
	char ip_aggregator[20];				// 14: AGGREGATOR (AS IP)

} MRT_FORM, *LPMRT_FORM;


typedef struct tagINVENTORY_LINK
{
    struct tagINVENTORY_LINK *nextNode, *prevNode;
	void *data;

} INVENTORY_LINK, *LPINVENTORY_LINK;
//INVENTORY_LINK *pInvenHead, *pInvenTail;


typedef struct tagLINK_OBJ
{
	INVENTORY_LINK *pInvenHead, *pInvenTail;
	void (*lpfn_initNode)(struct tagLINK_OBJ *);
	void (*lpfn_InsertNode)(struct tagLINK_OBJ *, void*);
	void (*lpfn_PrintNode)(struct tagLINK_OBJ *);

}LINK_OBJ, *LPLINK_OBJ;



#endif// __MKBRDATA_H__
