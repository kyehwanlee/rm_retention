


# 1. What this program does

This program removes directories and files according to the retention period defined in the config.json file.
If the creation time (derived from the directory path) is older than the specified retention period, that directory and all its subordinate files are deleted.



# 2. Process Analysis

The load_json_config() function first reads the user’s configuration file — which defines the company ID and retention period — using the cJSON library.

The ftw() (File Tree Walk) function is used to recursively visit and process all directories and subdirectories under the target path.
It automatically calls a user-defined callback function for each entry in the tree to handle the required file I/O operations.

In the callback function cb_delete_entry(), the program compares the date extracted from the directory name with the retention period specified in the configuration.
If the directory’s age exceeds the retention period, that directory — along with all its subdirectories and contained files — is removed.



# 3. Why ftw() was chosen

This approach was chosen because the ftw() function perfectly fits the assignment’s requirement:
to traverse and selectively remove directories and files based on a retention policy, without manually implementing recursive directory traversal logic.

The ftw() function greatly simplifies complex traversal processes — handling nested folders, symbolic links, and different file types —
while eliminating the need for explicit recursion or stack management.
It also provides useful metadata such as file type (FTW_F, FTW_D, FTW_DP, etc.) and traversal depth through the struct FTW argument,
enabling fine-grained control over deletion rules, retention logic, and dry-run behaviors.


# 4. How to use

## (1) install requirements
	In case of Ubuntu system, to install jSON package,
	
	  $ sudo apt install libcjson-dev


## (2) compile and build
###	Either use Makefile 
	
	  $ make

###	or gcc compilation command
	
	  $ gcc -O2 -Wall -o rm_retention rm_retention.c -I/usr/include/cjson -lcjson

	










