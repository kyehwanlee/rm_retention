// Build:
//   sudo apt install libcjson-dev
//   gcc -D_XOPEN_SOURCE=700 -O2 -Wall -Wextra retention_cleaner.c -o retention_cleaner -lcjson

#define _GNU_SOURCE
#define _XOPEN_SOURCE 700
#include <ftw.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <getopt.h>
#include <cjson/cJSON.h>

/*
  Assumptions:
   - config.json format:
     {
       "retention": {
         "default": 30,
         "1001": 60,
         "1017": 120
       }
     }

   - directory layout to evaluate:
     /.../<DEVICE>/<YYYY>/<MM>/<DD>/<HH>/<mm>
     last 5 tokens are YYYY MM DD HH mm
     device is token at index n-6
*/
#define MAX_PATH_LEN 256
#define MAX_TOKEN_LEN 16

static bool gDry_run = false;

// input: p_time, return: epoch time_t value
typedef struct tagPTIME 
{ 
  int year, month, day, hour, minute, second; 
} PTIME;

time_t ptime_to_epoch(const PTIME *pt)
{
  struct tm t = {0};

  t.tm_year = pt->year - 1900;  // 1900년 기준
  t.tm_mon  = pt->month - 1;    // 0~11
  t.tm_mday = pt->day;
  t.tm_hour = pt->hour;
  t.tm_min  = pt->minute;
  t.tm_sec  = pt->second;

  return timegm(&t);  // UTC time epoch return
}

#define NUM_COMPANY_MAX 256
#define LEN_COMPANY_ID 8
typedef struct tagRETN_CONFIG {
  int default_days;
  struct {
	char company_id[LEN_COMPANY_ID];
	int retention_days;
  } company[NUM_COMPANY_MAX];
  int count;

} RETN_CONFIG;



static RETN_CONFIG gRet_config;

static bool load_json_config(const char *path, RETN_CONFIG *pConfig)
{
  // initial values
  pConfig->count =0;
  pConfig->default_days = 30;

  // Open the JSON file for reading
  FILE *fp = fopen(path, "r");
  if (fp == NULL) {
    printf("Error: Unable to open the file.\n");
    return false;
  }

  // get the file size
  if (fseek(fp, 0, SEEK_END) != 0) {
    perror("Error seeking file"); // Prints "Error seeking file: [error description]"
    fprintf(stderr, "Error seeking file: %s\n", strerror(errno));
    fclose(fp);
    return false;
  }

  long fileSize = ftell(fp);
  if (fileSize == -1L) {
    perror("Error getting file position");
    fprintf(stderr, "Error getting file position: %s\n", strerror(errno));
    fclose(fp);
    return false;
  }

  if (fseek(fp, 0, SEEK_SET) != 0) {
    perror("Error seeking file"); // Prints "Error seeking file: [error description]"
    fprintf(stderr, "Error seeking file: %s\n", strerror(errno));
    fclose(fp);
    return false;
  }

  // Read the entire file into a buffer
  char *buffer = (char *) malloc(fileSize + 1);
  if (buffer == NULL) {
    fclose(fp);
    return false;
  }
  size_t num_read = fread(buffer, 1, (size_t)fileSize, fp);
  if (num_read != (size_t)fileSize) {
    fclose(fp);
    free(buffer);
    return false;
  }
  buffer[fileSize] = '\0'; // Null-terminate the string

  // Close the file
  fclose(fp);


  // Parse the JSON data
  cJSON *obj_json= cJSON_Parse(buffer);

  // Check if parsing was successful
  //
  if (obj_json == NULL) {
    const char *error_ptr = cJSON_GetErrorPtr();
    if (error_ptr != NULL) {
      fprintf(stderr, "Error JSON parsing, before: %s\n", error_ptr);
    }
    cJSON_Delete(obj_json);
    free(buffer);
    return false;
  }

  // Process the JSON data
  cJSON *retention = cJSON_GetObjectItem(obj_json, "retention");
  if (!retention ) { 
    cJSON_Delete(obj_json); 
    free(buffer);
    return false; 
  }


  for (cJSON *iter = retention->child; iter != NULL; iter = iter->next) {
    if (!iter->string || !cJSON_IsNumber(iter))
      continue;

    // default days parsing
    if (strcmp(iter->string, "default")==0) {
      pConfig->default_days = iter->valueint;
      continue;
    }

    if (pConfig->count >= NUM_COMPANY_MAX ) {
      fprintf(stderr, " Warning: reached to the max number of company: %d %s\n",
          NUM_COMPANY_MAX, iter->string);
      continue;
    }

    // company id: days parsing
    strncpy (pConfig->company[pConfig->count].company_id, iter->string, LEN_COMPANY_ID-1);
    pConfig->company[pConfig->count].company_id[LEN_COMPANY_ID - 1] = '\0';
    pConfig->company[pConfig->count].retention_days = iter->valueint;
    pConfig->count++;
  }

  // Keep reference to retention object: increase its refcount by duplicating pointer in new object
  //pConfig->json_Comp_Days = cJSON_Duplicate(retention, 1); // The '1' indicates a deep copy

  // Clean up
  cJSON_Delete(obj_json);
  free(buffer);
  return true;
}



void print_usage (char* usage)
{
  fprintf(stderr,
      "Usage: %s -c config.json -r ROOT [--dry-run] [--fd N]\n"
      "  -c/--config  config.json path (required)\n"
      "  -r/--root    root directory to scan (required)\n"
      "  --dry-run    perform dry-run (default: false)\n"
      "  --fd N       nftw max open fds (default 32)\n", usage);
}


int get_json_retention_days (const char* cid)
{
  int retVal_days = gRet_config.default_days; //default days
    
  for (int i = 0; i < gRet_config.count; i++) {
    if (strcmp(gRet_config.company[i].company_id, cid) == 0)
      return gRet_config.company[i].retention_days;
  }

  return retVal_days; 

}


int parse_path_info (const char *path, PTIME *ptime_out, char* company_out, size_t size)
{
  if (!path || !ptime_out)
    return -1;

  char path_buffer[MAX_PATH_LEN];
  strncpy(path_buffer, path, sizeof(path_buffer));
  path_buffer[MAX_PATH_LEN-1] = 0;

  char *token[MAX_TOKEN_LEN] = {0};
  char *saveptr=NULL;
  const char delimiters[] = "/";
  int n=0;
  char *device=NULL;
  char *company_id = NULL;

  char *t = strtok_r(path_buffer, delimiters, &saveptr);

  // token n: 1=/data, 2=company, 3=device, 4=year, 5=month, 6=day, 7=hour, 8=minute
  while(t != NULL) {
    if (n == MAX_TOKEN_LEN)
      break;
    token[n++] = t;
    t = strtok_r (NULL, delimiters, &saveptr);
  }

  if (n < 6)  // at least day info needed
    return -1;
  
  PTIME *p= ptime_out; 
  company_id = token[1];
  device     = token[2];
  p->year    = atoi(token[3]);
  p->month   = atoi(token[4]);
  p->day     = atoi(token[5]);

  p->hour    = (n >= 7) ? atoi(token[6]): 0;
  p->minute  = (n >= 8) ? atoi(token[7]): 0;
  p->second  = 0;

  if (company_out && size >0) {
    strncpy(company_out, company_id, size); 
    company_out[size-1] = '\0';
  }

  return 0;
}



// Callback function to be called by nftw for each file/directory
//
static int cb_delete_entry(const char *fpath, const struct stat *sb,
    int typeflag, struct FTW *ftwbuf)
{
  // Depth: 
  // 0=/data, 1=company, 2=device, 3=year, 4=month, 5=day, 6=hour, 7=minute

  // skip under day levels
  if (ftwbuf->level < 5) 
    return 0;

  PTIME pt = (PTIME){0}; 
  char company_id[LEN_COMPANY_ID] = {0};

  if (parse_path_info(fpath, &pt, company_id, (size_t)sizeof(company_id)) != 0) 
    return 0;

  // deletion criteria is the day, so hour.min should be zero'ed 
  pt.hour   = 0;
  pt.minute = 0;
  pt.second = 0;

  time_t device_time = ptime_to_epoch(&pt);
  time_t now=time(NULL);
  double diff_days = difftime(now, device_time )/(60*60*24);


  // obtain device's retention days from json
  int retention_days = get_json_retention_days (company_id);

  // true, if device current working days bigger than retention days
  bool bRemove = (diff_days >= retention_days); 

  switch (typeflag) {
    // delete files inside directory
    case FTW_F:
    case FTW_SL:

      // remove files, symbols etc
      if (bRemove == true) {
        if (gDry_run) // dry-run check
          printf("[DRY-RUN] Deleted file: %s\n", fpath);
        else {
          if (remove(fpath)==0)
            printf("Deleted file: %s\n",fpath);
          else
            perror(fpath);
        }
      }
      break;

      // if directory (post-order travel)
    case FTW_DP: 
      // direcotry : post-order, empty order: minute(7)->hour(6)->day(5) 
      if (bRemove && (ftwbuf->level >= 5 && ftwbuf->level <= 7)) {

        // remove directory itself
        if (gDry_run) // dry-run check
          printf("[DRY-RUN] Delete directory: %s\n", fpath);

        else {
          if (remove(fpath)==0)
            printf("Delete directory: %s\n",fpath);
          else
            perror(fpath);
        }
      }
      break;

    default:
      break;
  }

  return 0;
}



int main(int argc, char **argv) {
  int opt_level=0;
  int c,i;
  const char *config_path = NULL;
  const char *root_path = NULL;
  int fd_value= 32;  //default

  typedef struct option longoption_t;
  typedef enum param {
    O_DRYRUN=1, O_FD
  } param_t;

  static longoption_t longoptions[] = {
    { "config",   required_argument, NULL, 'c'},
    { "root",     required_argument, NULL, 'r'},
    { "dry-run",  no_argument,       NULL, O_DRYRUN},
    { "fd",       required_argument, NULL, O_FD    },
    { NULL, 0, NULL, 0 }
  };

  while ((c = getopt_long(argc, argv, "c:r:V", longoptions , NULL)) != -1) 
  { 
    switch (c) {    
      case 'c':       
        config_path = optarg;   
        printf("config_path :%s\n", config_path );
        break;        
      case 'r':       
        root_path = optarg;
        printf("root_path:%s\n", root_path);
        break;        
      case O_DRYRUN:       
        gDry_run = true;
        printf("dry-run:%d\n", gDry_run);
        break;
      case O_FD:       
        fd_value = atoi(optarg);
        printf("fd :%d\n", fd_value);
        break;
      case 'V':
        //use_stderr = opt_stderr = 1;
        break;
      default:
        print_usage(argv[0]);
        exit(EXIT_FAILURE);
        break;
    }
  }


  if (!config_path || !root_path) { 
    print_usage(argv[0]); 
    return EXIT_FAILURE; 
  }

  printf("Config path: %s\nRoot path: %s\nDry-Run: %s\nFD size: %d\n",
      config_path, root_path, gDry_run ?"true":"false", fd_value);


  if (!load_json_config(config_path, &gRet_config))
    return EXIT_FAILURE;


  // FTW_PHYS: Do not follow symbolic links || FTW_DEPTH: ensures children are processed before parent 
  int flags = FTW_PHYS | FTW_DEPTH; 

  if (nftw(root_path, cb_delete_entry, fd_value, flags) == -1) {
    perror("nftw");
    return EXIT_FAILURE;
  }


  return 0;
}

