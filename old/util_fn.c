
#include<stdio.h>
#include<stdlib.h>
#include<sys/socket.h>
#include<netinet/in.h>
#include<arpa/inet.h>
#include<string.h>
#include <stdarg.h> /* va_list va_start va_end */

#include "util_fn.h"

extern void debug_print(const char* name, const char* func, const int line, char *fmt, ...)
__attribute__((format(printf, 4,5)));

void debug_print(const char* name, const char* func, const int line, char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[%s():%i]\t",func, line);
    vfprintf(stderr, fmt, ap);
    va_end(ap);

}

unsigned long ip_cal(void)
{
    unsigned char ip_frag[4]={0};
    unsigned long ip_address;
    struct in_addr sin_addr;

    ip_address= (ip_frag[0] <<24) | (ip_frag[1]<<16) | (ip_frag[2]<<8) | ip_frag[3] ;
    sin_addr.s_addr=ip_address;

}

// ~/ForCES/NIST_ID_LOC/ControlElement/RoutingProc/utils.h" 
// /users/kyehwanl/ForCES/NIST_ID_LOC/MappingServer/MappingServer.c
// ~/ForCES/NIST_ID_LOC/ControlElement/RoutingProc/utils.h      
// make-csv.c


// addr: ip address, addr_data: extracted ipaddr
int addrtoul(char *addr, unsigned char* addr_data)
{

    const char *cp;
    unsigned char* ap;
    unsigned long data= 0;
    int i;

    ap = (char*)&data;

    for (cp=addr, i=0; *cp; cp++) {
        if (*cp <= '9' && *cp >= '0') {
            ap[i] = 10*ap[i] + (*cp-'0');
            continue;
        }
        if (*cp == '.' && ++i <= 3)
            continue;
        return -1;
    }
    addr_data[0] = ap[0];
    addr_data[1] = ap[1];
    addr_data[2] = ap[2];
    addr_data[3] = ap[3];

#if __DEBUG__
//  printf("%x %d %d %d %d \n", data, ap[0], ap[1], ap[2], ap[3]);
    printf("%x %d %d %d %d \n", data, addr_data[3], addr_data[2], addr_data[1], addr_data[0]);
#endif
    return 0;
}



#if _REFERENCE_

int CNetCal::Init()
{
    FILE * pInitFile;

    pInitFile = fopen("init.txt", "rt");
    if(!pInitFile) { printf("File error, exit\n"); return -1;}

    char lnstr[MAX_LINE],  delims[] = " \n"; //char skstr[MAX_LEN];
    char* record;

    //int argc=0; char** m_lplpArgv;
    m_imemSize = sizeof(char) * 20 * 8;

    m_lplpArgv = (char**)   malloc(m_imemSize);
    memset(m_lplpArgv, 0x00, m_imemSize);

    char** lplpOrig_Argv= m_lplpArgv;  // rewind

    while( feof( pInitFile ) == 0 )
    {
        m_iArgc=0; m_lplpArgv = lplpOrig_Argv;
        if( fgets( lnstr, MAX_LINE, pInitFile ) && strcmp( lnstr, "") )
        {
            record = strtok( lnstr, delims );
            m_lplpArgv[m_iArgc++]  = record;

            while( record && strcmp( record, "" ) ){
                record = strtok( NULL, delims );

                if(record != 0) {
                    //if(record[0] >= 0x41)
                        m_lplpArgv[m_iArgc++]= record;
                    //else 
                        //city[row][(iCnt-1)/2].iDist = atoi(record);
                }
            } // end - while
        }// end - if


        //
        // implementation
        //
        parser();
    }

    fclose(pInitFile);

    return 0;
}

int CNetCal::parser()
{
    int argc = m_iArgc;
    char** argv= m_lplpArgv;

    CNode *lpNode;
    CLink *lpLink;

    while(argc>0) {

        if (*argv == NULL) break;

        // 'set'
        if(strcmp(*argv, "set") == 0 || strcmp(*argv, "SET") == 0) {
            printf(" set \n");
        }

        // 'node'
        else if (strcmp(*argv, "node") == 0 || strcmp(*argv, "NODE") == 0) {

            lpNode = new CNode;
            NEXT_ARG();

            if (*argv == NULL) { printf("error\n"); break;}

            if( **argv >= 0x30 && **argv <= 0x39)    // number
                printf("number: %d \n", atoi(*argv) );
            else if( **argv >= 0x40) {
                strcpy(lpNode->name, *argv);

            }


            InsertNode(lpNode);
        }

        // 'link'
        else if (strcmp(*argv, "link") == 0 || strcmp(*argv, "LINK") == 0) {

            char* strNode1; char* strNode2;
            lpLink = new CLink;

            NEXT_ARG();
            strNode1 = *argv;
            INVENTORY_LINK *lptLink1 = FindNode(strNode1);

            NEXT_ARG();
            strNode2 = *argv;
            INVENTORY_LINK *lptLink2 = FindNode(strNode2);

            lptLink1->NeighborLink.push_back(strNode2);
            lptLink2->NeighborLink.push_back(strNode1);

#if _TEST_
            int i=0;
            printf("link1 - current node: %s \n", lptLink1->Node->name);
            for(i=0; i< lptLink1->NeighborLink.size(); i++)
                printf("neighbor node: %s \n", lptLink1->NeighborLink[i].c_str());

            printf("link2 - current node: %s \n", lptLink2->Node->name);
            for(i=0; i< lptLink2->NeighborLink.size(); i++)
                printf("neighbor node: %s \n", lptLink2->NeighborLink[i].c_str());
#if 0
            vector<string>::iterator iter;

            iter =(vector<string>::iterator)lptLink1->NeighborLink.begin(); 
            for(iter; iter != (vector<string>::iterator)lptLink1->NeighborLink.end(); iter++)
            {
                //printf("%s\n", (lptLink1->NeighborLink[iter]));
                //cout<< lptLink1->NeighborLink[iter][0] << endl;
            }
#endif
#endif

        }

        else
        {

        }

        // variable
        if (**argv == '$') {
            strcpy(GetCurrentNode()->var, *argv);
            printf("\n -- print variable : %s \n\n", GetCurrentNode()->var);
        }

        argc--; argv++;
    }


    return 0;
}



#endif //_REFERENCE_




