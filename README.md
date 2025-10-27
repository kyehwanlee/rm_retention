


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




# 4. Future Improvement

- The current program runs as a single process that scans directories using nftw() to obtain time values and compares them against user-defined retention periods from config.json to decide whether to delete a folder.

- **To improve scalability and responsiveness, the design can be extended to use two cooperative execution threads:**  
  - Thread 1: Recursively scans the existing directory structure with nftw() and builds a min-heap (complete binary tree) in memory.
Each heap entry stores the creation time and the retention time configured for that company/device.
The sum of these two values forms the expire_epoch, which becomes the heap’s key so that the smallest (earliest expiration) entry always remains at the top.

  - Thread 2: The check interval (e.g., every 10–30 minutes) can be made configurable through the JSON file to adapt to different workloads. This checks the top of the heap and compares its timestamp with the current system time (time(NULL)).
If the current time exceeds the expiration, the corresponding directory and files are deleted.

- **Threading vs Multi-Process:**  
Instead of using two separate processes with IPC, using two threads (pthread) is simpler and more efficient because they share the same memory address space and can directly access the heap structure.

- **Time Complexity Advantages:**
  - The key advantage lies in finding the minimum value:
in a stack, queue, or list this search takes O(n),
but in a min-heap, retrieving the minimum (top element) takes O(1).
  - Even though push/pop on a heap require O(log n) time, this is much more efficient than a full O(n) scan when handling large-scale datasets (e.g., 100,000+ entries).

- **Restart durability (optional enhancement):**  
Only a single nftw() scan at startup is necessary to rebuild the heap.
For large or frequently changing directory sets, the design can later integrate inotify to detect new directories in real time and immediately update the heap.
This approach ensures that newly created entries are not missed while reducing the overhead of repeated full scans.

- Once stabilized, this two-thread architecture can run continuously as a background daemon or systemd service, providing efficient mid-scale to large-scale retention management without the need for an external database.




# 5. Implementation Considerations

To ensure the proposed multi-threaded and heap-based architecture operates safely and efficiently, the following design considerations should be addressed during implementation:

- **Thread Synchronization:**  
Since multiple threads (heap builder, watcher, and cleaner) may concurrently access the shared heap structure, all heap operations (push/pop/peek) should be protected using pthread_mutex_t locks. 
A lightweight mutex ensures data consistency while maintaining concurrency between threads. (example below)  
  ```bash
  pthread_mutex_lock(&heap_mutex);
  heap_push(&heap, entry);
  pthread_mutex_unlock(&heap_mutex);
  ```

- **Initial Scan Load Control:**  
When performing the first nftw() traversal across a very large directory tree, CPU and disk I/O usage can spike.
To mitigate this, the scanning thread can implement a rate limiter or batch insertion into the heap (e.g., pushing 1000 entries per cycle with short sleeps).


- **Cross-platform and Restart Durability:**  
The design is fully portable to all Linux distributions supporting POSIX APIs.
On startup, only a single nftw() scan is required to rebuild the heap.
Optionally, inotify can be used to dynamically add or remove heap entries whenever directories are created or deleted, ensuring real-time synchronization without full rescans.




# 6. Installations & Usage

## (1) install requirements to install jSON package
This installs the header (/usr/include/cjson/) and the shared library (/usr/lib64/libcjson.so).

### Ubuntu / Debian	
```bash
sudo apt update
sudo apt install libcjson-dev -y
```
	
### RHEL / CentOS / Rocky / AlmaLinux
(Older systems may use yum instead of dnf)
Make sure EPEL repository is enabled before installing dependencies:

```bash
# Enable EPEL repository
sudo dnf install epel-release -y
sudo dnf update -y

# Then install cJSON development package
sudo dnf install cjson-devel -y
```
  


## (2) compile and build
###	Either use Makefile 
	
	  $ make

###	or gcc compilation command
	
	  $ gcc -O2 -Wall -o rm_retention rm_retention.c -I/usr/include/cjson -lcjson



## (3) usage	


	Usage: ./rm_retention -c config.json -r ROOT [--dry-run] [--fd N]

	  -c/--config  config.json path (required)
	  -r/--root    root directory to scan (required)
	  --dry-run    perform dry-run (default: false)
	  --fd N       nftw max open fds (default 32)

	(for example)
		./rm_retention -c config.json -r /data --dry-run --fd 32










