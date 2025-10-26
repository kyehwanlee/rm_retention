#include <stdio.h>
#include <unistd.h> /* getopt */
#include <stdlib.h> /* EXIT_{numbers} */
#include <limits.h> /* LINE_MAX */
#include <string.h> /* strcpy ... */

#include "mkbrdata.h"

/*
 * version ID data & global variable
 */
static const char svn_id[]=" $mkBr$";
int gDebug= TRUE;

/*
 * Function Declaration 
 */
void xml_init(FILE*);
void xml_terminate(FILE *fp_xml);
int parse_for_brite(char *data, LPMRT_FORM);
void print_usage(int argc, char** argv);
void xml_bgp_tag(FILE*, char*, LPMRT_FORM);
void xml_data_tag(FILE* fp_xml, char* id, LPMRT_FORM data);
void substring(char* str, char delim[]);
LINK_OBJ* link_obj_new(void);



#define _TEST_ 1

int main(int argc, char **argv)
{

	int opt_level=0;
	char *mrt_file = argv[1];
	char xml_file[128];
	int ret=1;
	int c,i;
	FILE *fp_mrt, *fp_xml;
	char buffer[LINE_MAX];

	
	/* Linked list init */
	//initNode();

#define _LINK_OBJ_TEST_ 1
#if _LINK_OBJ_TEST_ 
	LINK_OBJ *temp  = link_obj_new();
	DEBUG(" temp: %p \n", temp);
	MRT_FORM mrt;
	mrt.local_pref = 111;
	(*temp->lpfn_InsertNode)(temp, (void*)&mrt);
	DEBUG(" &mrt: %p \n", &mrt);
	(*temp->lpfn_PrintNode)(temp);
#endif
	return 1;


#if _TEST___
	for(i=0; i<argc; i++)
		printf("argc: argv = %d: %s\n", i+1, argv[i]);
#endif

	while ((c = getopt(argc, argv, "c:l:sej:V")) > 0) { 
		printf("--------------\n");
		switch (c) {    
			case 'c':       
				mrt_file = optarg;   
				printf("optarg:%s", optarg);
				break;        
			case 'l':       
				opt_level = 1;
				break;        
			case 's':       
				//use_syslog = opt_syslog = 1;
				break;
			case 'e':
				//use_stderr = opt_stderr = 1;
				break;
			case 'j':
#if 0
				if (!configure_integer(&rc, &jitter, optarg))
					goto done;
#endif
				//opt_jitter = 1;      
				break;
			case 'V':
				puts(svn_id);
				ret = 0;
				//goto done;
			default:
				fprintf(stderr, "Usage: %s [-t nsecs] [-n] name\n",
						argv[0]);
				exit(EXIT_FAILURE);
				break;
		}
	}

#if _TEST_
	printf("\n\nargc:%d \n", argc);
	for(i=0; i<argc; i++)
		printf("argc: argv = %d: %s\n", i+1, argv[i]);
	for(i=0; i<optind; i++)
		printf("%d: optind=%d argv[optind]:%s \n", i, optind, argv[optind]);

	printf("LINE_MAX: %d \n", LINE_MAX);
#endif


	if (optind > argc) {
		fprintf(stderr, "Expected argument after options\n");
		exit(EXIT_FAILURE);
	}

	/* mrt file load */
	if (mrt_file <= 0) {
		printf("\n\nThere is no mrt file input \n");
		print_usage(argc, argv);
		exit(EXIT_FAILURE);
	}
	if((fp_mrt = fopen(mrt_file, "rt")) == NULL ) {
		printf("\n\nCouldn't load mrt file: %s \n", mrt_file);
		print_usage(argc, argv);
		exit(EXIT_FAILURE);
	}
	
	sprintf(xml_file, "%s.xml", mrt_file);
	if((fp_xml = fopen(xml_file,"wt"))==NULL) {
		printf("\n\nCouldn't write xml file: %s \n");
		exit(EXIT_FAILURE);
	}

	/* initializing */
	int breaker=0;
	LPMRT_FORM lpt_mrt_data=malloc(sizeof(MRT_FORM));
	char id_prefix[] = "dat_";
	int id_count=0;
	char dat_id[10];

	xml_init(fp_xml);
	xml_bgp_tag(fp_xml, mrt_file, lpt_mrt_data);

	DEBUG(" DEBUG testing\n");


	/* implementation of parsing and making xml */

	while( fgets(buffer, LINE_MAX, fp_mrt) != NULL)
	//fgets(buffer,128, fp_mrt);
	{

		sprintf(dat_id, "%s%d", id_prefix, id_count++);
		parse_for_brite(buffer, lpt_mrt_data);
		xml_data_tag(fp_xml, dat_id, lpt_mrt_data);

		breaker++;

		if (breaker==10)
			break;
	}



	xml_terminate(fp_xml);

	fclose(fp_mrt);
	fclose(fp_xml);

	return 0;
} // end of main



void print_usage(int argc, char** argv)
{

	fprintf(stderr, "Usage: %s [-t nsecs] [-n] name\n", argv[0]);

}


int parse_for_brite(char *data, LPMRT_FORM lpt_mrt)
{
    if(!data) { printf("File reading error \n"); return -1;}

	LPMRT_FORM pt_mrt_data = lpt_mrt;
	char lnstr[LINE_MAX], delim[] = "|", delim2[]=" "; 
    char* record;
	char* str;
	char* saveprt1, saveprt2;
	int len=0;

	strcpy(lnstr,data);

	record = strtok_r(lnstr, delim, &saveprt1);	// 1.type
	strcpy(pt_mrt_data->type, record);

	record = strtok_r(NULL, delim, &saveprt1);	// 2. time
	strcpy(pt_mrt_data->timestamp, record);

	record = strtok_r(NULL, delim, &saveprt1);	// 3. STATUS
	strcpy(pt_mrt_data->status, record);

	record = strtok_r(NULL, delim, &saveprt1);	// 4. peer
	strcpy(pt_mrt_data->ip_peer, record);

	record = strtok_r(NULL, delim, &saveprt1);	// 5. peerAS
	strcpy(pt_mrt_data->peerAS, record);

	record = strtok_r(NULL, delim, &saveprt1);	// 6. prefix
	strcpy(pt_mrt_data->prefix, record);

	record = strtok_r(NULL, delim, &saveprt1);	// 7. AS_PATH
	pt_mrt_data->as_path= record;
	len = strlen(record)+1;
	str = malloc(sizeof(len));
	strcpy(str, record);
	str[len]='\0';
	substring(str, delim2);

	record = strtok_r(NULL, delim, &saveprt1);	// 8. origin
	strcpy(pt_mrt_data->origin, record);

	record = strtok_r(NULL, delim, &saveprt1);	// 9. next_hop
	strcpy(pt_mrt_data->ip_nexthop, record);

	record = strtok_r(NULL, delim, &saveprt1);	// 10. local_pref
	pt_mrt_data->local_pref = (unsigned short)atoi(record);

	record = strtok_r(NULL, delim, &saveprt1);	// 11. multi_exit_disc
	pt_mrt_data->multi_exit_disc= (unsigned short)atoi(record);

	record = strtok_r(NULL, delim, &saveprt1);	// 12. community
	pt_mrt_data->community  = record;


#if __DEBUG__
	printf("%s \t", pt_mrt_data->type);
	printf("%s \t", pt_mrt_data->timestamp);
	printf("%s \t", pt_mrt_data->status);
	printf("%-16s \t", pt_mrt_data->ip_peer);
	printf("%s \t", pt_mrt_data->peerAS);
	printf("%s \t", pt_mrt_data->prefix);
	printf("%s \t", pt_mrt_data->as_path);
	printf("%s \t", pt_mrt_data->origin);
	printf("%s \t", pt_mrt_data->ip_nexthop);
	printf("%d \t", pt_mrt_data->local_pref);
	printf("%d \t", pt_mrt_data->multi_exit_disc);
	printf("%s \t", pt_mrt_data->community);

	printf("\n");
#endif

	return 0;
}

#if _TESTS_
#if 0//_Example_
"http://linux.die.net/man/3/strtok_r"

The following program uses nested loops that employ strtok_r() to break a string into a two-level hierarchy of tokens. The first command-line argument specifies the string to be parsed. The second argument specifies the delimiter character(s) to be used to separate that string into "major" tokens. The third argument specifies the delimiter character(s) to be used to separate the "major" tokens into subtokens.
//#include <stdio.h>
//#include <stdlib.h>
//#include <string.h>
int
main(int argc, char *argv[])
{
    char *str1, *str2, *token, *subtoken;
    char *saveptr1, *saveptr2;
    int j;
    if (argc != 4) {
        fprintf(stderr, "Usage: %s string delim subdelim\n",
                argv[0]);
        exit(EXIT_FAILURE);
    }
    for (j = 1, str1 = argv[1]; ; j++, str1 = NULL) {
        token = strtok_r(str1, argv[2], &saveptr1);
        if (token == NULL)
            break;
        printf("%d: %s", j, token);
        for (str2 = token; ; str2 = NULL) {
            subtoken = strtok_r(str2, argv[3], &saveptr2);
            if (subtoken == NULL)
                break;
            printf("t --> %s", subtoken);
        }
    }
    exit(EXIT_SUCCESS);
} /* main */
An example of the output produced by this program is the following:
$ ./a.out 'a/bbb///cc;xxx:yyy:' ':;' '/'
1: a/bbb///cc
         --> a
         --> bbb
         --> cc
2: xxx
         --> xxx
3: yyy
         --> yyy
#endif


#endif

/*
void debug_print(const char* name, const char* func, const int line, char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[%s():%i]\t",func, line);
    vfprintf(stderr, fmt, ap);
    va_end(ap);

}
*/


void xml_bgp_tag(FILE* fp_xml, char* id, LPMRT_FORM data)
{

	fprintf(fp_xml, "  - <bgp id=\"%s\">\n", id);
	

}

void xml_data_tag(FILE* fp_xml, char* id, LPMRT_FORM data)
{

	/* contents */
	fprintf(fp_xml, "    - <data id=\"%s\" time=\"%s\">\n", id, data->timestamp);
	fprintf(fp_xml, "      <path>{IUT} {R} %s</path>\n", data->as_path);
	fprintf(fp_xml, "      - <announcement>\n");
	fprintf(fp_xml, "        <prefix>%s</prefix> \n", data->prefix);

	/* closing */
	fprintf(fp_xml, "      </announcement>\n");
	fprintf(fp_xml, "    </data>\n");
}


void xml_init(FILE *fp_xml)
{
	/* structure xml */
	fprintf(fp_xml, "<?xml version=\"1.0\" ?>\n");
	fprintf(fp_xml, " - <brite xmlns=\"http://www.antd.nist.gov/brite\" xsi:schemaLocation=\"http://www.antd.nist.gov/brite ../conf/brite.xsd\" xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\">\n");
	fprintf(fp_xml, "  <include file=\"rpki_tree.xml\" id=\"fil_tree\" />\n");
}


void xml_terminate(FILE *fp_xml)
{
	fprintf(fp_xml, "  </bgp>\n");
	fprintf(fp_xml, "</brite>\n");
}



void substring(char* str, char delim[])
{

	char* record;
	int count=0;

	DEBUG("delimeter = %s\n", delim);
	record = strtok(str, delim);

	while(record != NULL) {
		count++;
		DEBUG("%s\n", record);
		record = strtok(NULL, delim);
	}

}



void initNode(LPLINK_OBJ obj)
{
    //pInvenHead = (struct tagINVENTORY_LINK*)malloc(sizeof(struct tagINVENTORY_LINK));
    //pInvenHead = (INVENTORY_LINK*)malloc(sizeof(INVENTORY_LINK));
    //pInvenTail = (INVENTORY_LINK*)malloc(sizeof(INVENTORY_LINK));
	
	INVENTORY_LINK *pInvenHead = obj->pInvenHead = (INVENTORY_LINK*)malloc(sizeof(INVENTORY_LINK));
	INVENTORY_LINK *pInvenTail = obj->pInvenTail = (INVENTORY_LINK*)malloc(sizeof(INVENTORY_LINK));

    //pInvenHead = (INVENTORY_LINK*)malloc(sizeof(INVENTORY_LINK));
    //pInvenTail = (INVENTORY_LINK*)malloc(sizeof(INVENTORY_LINK));

    pInvenHead->nextNode = pInvenTail;
    pInvenHead->prevNode = pInvenHead;
    pInvenTail->nextNode = pInvenTail;
    pInvenTail->prevNode = pInvenHead;

	DEBUG("pInvenHead:%p\n", pInvenHead);
	DEBUG("pInvenTail:%p\n", pInvenTail);

}

void InsertNode(LPLINK_OBJ obj, void* userdata)
{
	INVENTORY_LINK *pInvenHead = obj->pInvenHead;
	INVENTORY_LINK *pInvenTail = obj->pInvenTail;

    INVENTORY_LINK *newNode;
    newNode = (INVENTORY_LINK*)malloc(sizeof(INVENTORY_LINK));

    // make chainning
    newNode->nextNode = pInvenHead->nextNode;
    newNode->prevNode = pInvenHead;

    pInvenHead->nextNode->prevNode = newNode;
    pInvenHead->nextNode = newNode;

    // TO DO: fill contents
    //
	newNode->data= userdata;

	DEBUG("input data:%p \n", userdata);
	DEBUG(" local pref: %d \n\n",((LPMRT_FORM)newNode->data)->local_pref);
}

void PrintNode(LPLINK_OBJ obj)
{
	INVENTORY_LINK *pInvenHead = obj->pInvenHead;
	INVENTORY_LINK *pInvenTail = obj->pInvenTail;

    short count=1;
    INVENTORY_LINK *pTempNode;

    pTempNode= pInvenHead->nextNode;

    while(pTempNode != pInvenTail)
    {
#if __DEBUG__
        printf("CurrentNode : %p\n", pTempNode);
        printf("-------- [%d count field]-------- \n", count);
        printf(" data: %p \n", pTempNode->data);
        printf(" local pref: %d \n",((LPMRT_FORM)pTempNode->data)->local_pref);
        count++;
        pTempNode = pTempNode->nextNode;
        printf("pTempNode->next : %p\n", pTempNode->nextNode);
		printf("\n\n");
#endif
    }

}


#if 0 //_LINKED_LIST_

// if Linked list over MAX_INVEN_COUNT, delete last list
//
void DeleteNode(void)
{

    INVENTORY_LINK  *tempInven;
    tempInven = pInvenTail->prevNode;

    pInvenTail->prevNode->prevNode->nextNode = pInvenTail;
    free(tempInven);
}

// find and search wanted numbered link
INVENTORY_LINK *GetNode(short count)
{
    short i;
    INVENTORY_LINK *pObtainInven;
    pObtainInven = pInvenHead;

    if(pObtainInven->nextNode == pInvenTail)
        return NULL; // 

    for (i=0; i<count; i++)
    {
        if (pObtainInven->nextNode != pInvenTail) // && (pObtainInven->nextNode != pInvenTail))
            pObtainInven = pObtainInven->nextNode;
        else
            return NULL;
    }

    return pObtainInven;


}


void PrintNode(void)
{
    short count=1;
    INVENTORY_LINK *pTempNode;

    pTempNode= pInvenHead->nextNode;

    while(pTempNode != pInvenTail)
    {
#if __DEBUG__
        printf("CurrentNode : %p\n", pTempNode);
        printf("-------- [%d count field]-------- \n", count);
        printf(" data: %p \n\n", pTempNode->data);
        //printf(" local pref: %d \n\n",((LPMRT_FORM)pTempNode->data)->local_pref);
        count++;
        pTempNode = pTempNode->nextNode;
        printf("pTempNode->next : %p\n", pTempNode->nextNode);
#endif
    }

}

LPINVENTORY_LINK GetCurrentNode(void)
{
    INVENTORY_LINK *pObtainInven;
    pObtainInven = pInvenHead;

    if(pObtainInven->nextNode == pInvenTail)
        return NULL; // 

    if (pObtainInven->nextNode != pInvenTail) // && (pObtainInven->nextNode != pInvenTail))
        pObtainInven = pObtainInven->nextNode;
    else
        return NULL;

    return pObtainInven;

}

LPINVENTORY_LINK FindNode(char* st)
{
    INVENTORY_LINK *pIter;

    if(st == NULL) {
        printf("parameter invalid\n");
        return NULL;
    }

    if(pInvenHead->nextNode == pInvenTail)
    {
        printf(" There is no data \n");
        return NULL;
    }

    for(pIter=pInvenHead->nextNode; pIter != pInvenTail; pIter= pIter->nextNode )
    {
#if 0
        //if (strcmp(pIter->var, st) == 0) { 
        if (strcmp(pIter->Node->name, st) == 0) { 

            // to do :
            //printf("match found !!!\n"); // test
            return pIter;
        }
#endif
    }

    printf(" Not found out \n");
    return NULL;
}
#endif //_LINKED_LIST_

LINK_OBJ* link_obj_new(void)
{
	LINK_OBJ *link_obj = malloc(sizeof(LINK_OBJ));

	link_obj->lpfn_initNode = initNode;
	link_obj->lpfn_InsertNode = InsertNode;
	link_obj->lpfn_PrintNode = PrintNode;

	initNode(link_obj);

	return link_obj;
}


