/*
Copyright (C) 2013- The University of Notre Dame
This software is distributed under the GNU General Public License.
See the file COPYING for details.
*/

/* Monitors a set of programs for CPU time, memory and
 * disk utilization. The monitor works 'indirectly', that is, by
 * observing how the environment changed while a process was
 * running, therefore all the information reported should be
 * considered just as an estimate (this is in contrast with
 * direct methods, such as ptrace).
 *
 * Use as:
 *
 * resource_monitor -i 120000 -- some-command-line-and-options
 *
 * to monitor some-command-line at two minutes intervals (120000
 * miliseconds).
 *
 * Each monitor target resource has two functions:
 * get_RESOURCE_usage, and acc_RESOURCE_usage. For example, for memory we have
 * get_mem_usage, and acc_mem_usage. In general, all functions
 * return 0 on success, or some other integer on failure. The
 * exception are function that open files, which return NULL on
 * failure, or a file pointer on success.
 *
 * The get_RESOURCE_usage functions are called at given intervals.
 * Between each interval, the monitor does nothing. All processes
 * monitored write to the same text log file. If no file name is provided,
 * for the log, then the log file is written to a file called
 * log-monitor-PID, in which PID is the pid of the monitor.
 *
 * The acc_RESOURCE_usage(accum, other) adds the contents of
 * other, field by field, to accum.
 *
 * monitor_CATEGORY_summary writes the corresponding information
 * to the log. CATEGORY is one of process, working directory of
 * filesystem. Each field is separated by \t.
 *
 * Currently, the columns are:
 * 
 * wall:      wall time (in usecs).
 * no.proc:   number of processes
 * cpu-time:  user-mode time + kernel-mode time.
 * vmem:      current total memory size (virtual).
 * io:        read chars count using *read system calls + writen char count using *write system calls.
 * files+dir  total file + directory count of all working directories.
 * bytes      total byte count of all working directories.
 * nodes      total occupied nodes of all the filesystems used by working directories since the start of the task. 
 *
 * The log file is written to the home directory of the monitor
 * process. A flag will be added later to indicate a prefered
 * output file. Additionally, a summary log file is written at
 * the end, reporting the command run, starting and ending times,
 * and maximum, minimum, and average of the resources monitored.
 *
 * While all the logic supports the monitoring of several
 * processes by the same monitor, only one monitor can
 * be specified at the command line. This is because we plan to
 * wrap the calls to fork and clone in the monitor such that we
 * can also monitor the process children.
 *
 * Each monitored process gets a 'struct process_info', itself
 * composed of 'struct mem_info', 'struct cpu_time_info', etc. There
 * is a global variable, 'processes', that keeps a table relating pids to
 * the corresponding struct process_info.
 *
 * Likewise, there are tables that relate paths to 'struct
 * wdir_info' ('wdirs'), and device ids to 'struct
 * filesys_info' ('filesysms').
 *
 * The process tree is summarized from the struct *_info into
 * struct tree_info. For each time interval there are three
 * struct tree_info: current, maximum, and minimum. 
 *
 * Grandchildren processes are tracked via the helper library,
 * which wraps the family of fork functions.
 *
 * The monitor program handles SIGCHLD, by either retrieving the
 * last usage of the child (getrusage through waitpid) and
 * removing it from the table above described, or logging SIGSTOP
 * and SIGCONT. On SIGINT, the monitor sends the sigint signal to
 * the first processes it created, and cleans up the monitoring
 * tables.
 *
 * monitor takes the -i<miliseconds> flag, which indicates how often
 * the resources are checked. The logic is there to allow, say,
 * memory to be checked twice as often as disk, but right now all
 * the resources are checked at each interval.
 *
 */

/* BUGS:
 *
 * LOTS of code repetition that probably can be eliminated with
 * calls to function pointers and some macros.
 *
 * BSDs: kvm interface is not implemented.
 *
 * io: may report zero if process ends before we read
 * /proc/[pid]/io.
 *
 * statfs: always reports the same numbers in AFS.
 * statfs: Called in current working directory. A process might
 * be writting in a different filesystem.
 *
 * If the process writes something outside the working directory,
 * right now we are out of luck.
 *
 * For /a/b, if a and b are working directories of two different
 * processes, then b usage is logged twice.
 *
 */

#include "hash_table.h"
#include "itable.h"
#include "list.h"
#include "stringtools.h"
#include "debug.h"
#include "xxmalloc.h"
#include "copy_stream.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <limits.h>
#include <fts.h>

#include <sys/select.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/times.h>
#include <sys/stat.h>

#include <inttypes.h>
#include <sys/types.h>

#if defined(__APPLE__) || defined(__FreeBSD__)
	#include <sys/param.h>
    #include <sys/mount.h>
#else
	#include  <sys/vfs.h>
#endif

#include "rmonitor_helper_comm.h"
#include "rmonitor_piggyback.h"

#define DEFAULT_INTERVAL       1        /* in seconds */

#define ONE_MEGABYTE 1048576  /* this many bytes */
#define ONE_SECOND   1000000  /* this many usecs */    

FILE  *log_file;               /* All monitoring information of all processes is written here. */
FILE  *log_file_summary;       /* Final statistics are written to this file. */

int    monitor_queue_fd;       /* File descriptor of a datagram socket to which (great)
								  grandchildren processes report to the monitor. */

pid_t  first_process_pid;              /* pid of the process given at the command line */
pid_t  first_process_exit_status;      /* exit status of the process given at the command line */
char  *over_limit_str = NULL;          /* string to report the limits exceeded */

double secs_initial;                   /* Time at which monitoring started, in secs. */


struct itable *processes;       /* Maps the pid of a process to a unique struct process_info. */
struct hash_table *wdirs;       /* Maps paths to working directory structures. */
struct itable *filesysms;       /* Maps st_dev ids (from stat syscall) to filesystem structures. */
struct itable *wdirs_rc;        /* Counts how many process_info use a wdir_info. */
struct itable *filesys_rc;      /* Counts how many wdir_info use a filesys_info. */


char *lib_helper_name = "librmonitor_helper.so";	/* Name of the helper library that is 
													   automatically extracted */	
int lib_helper_extracted;		/* Boolean flag to indicate whether the bundled helper library 
								   was automatically extracted */ 

//time in usecs, no seconds:
struct cpu_time_info
{
	double user_time;
	double kernel_time;

	double delta_user_time;
	double delta_kernel_time;
};

struct mem_info
{
	uint64_t virtual; 
	uint64_t resident;
	uint64_t shared;
	uint64_t text;
	uint64_t data;
};

struct io_info
{
	uint64_t chars_read;
	uint64_t chars_written;

	uint64_t delta_chars_read;
	uint64_t delta_chars_written;
};

struct wdir_info
{
	double   wall_time;
	char 	*path;
	int      files;
	int      directories;
	off_t    byte_count;
	blkcnt_t block_count;

	struct filesys_info *fs;
};

struct filesys_info
{
	double          wall_time;
	int             id;
	char           *path;			// Sample path on the filesystem.
	struct statfs   disk;			// Current result of statfs call minus disk_initial.
	struct statfs   disk_initial;   // Result of the first time we call statfs.
};

struct process_info
{
	double      wall_time;
	pid_t       pid;
	const char *cmd;
	int         running;

	struct mem_info      mem;
	struct cpu_time_info cpu;
	struct io_info       io;

	struct wdir_info *wd;
};

char *resources[15] = { "wall_clock(seconds)", 
						"concurrent_processes", "cpu_time(seconds)", "virtual_memory(kB)", "resident_memory(kB)", 
						"bytes_read", "bytes_written", 
						"workdir_number_files_dirs", "workdir_footprint(MB)",
						NULL };

struct tree_info
{
	double   wall_time;
	int64_t  max_concurrent_processes;
	double   cpu_time;
	int64_t  virtual_memory; 
	int64_t  resident_memory; 
	int64_t  bytes_read;
	int64_t  bytes_written;
	int64_t  workdir_number_files_dirs;
	int64_t  workdir_footprint;

	int64_t  fs_nodes;

};

struct tree_info     tree_max;

struct tree_info     tree_limits;

/*** 
 * Utility functions (open log files, proc files, measure time)
 ***/

char *current_time(void)
{
	time_t secs = time(NULL);

	return ctime(&secs);
}

double secs_since_epoch()
{
	double secs;
	struct timeval time; 

	gettimeofday(&time, NULL);

	secs  = time.tv_sec;
	secs += ((double) time.tv_usec / ONE_SECOND);

	return secs;
}

double secs_since_launched()
{
	return (secs_since_epoch() - secs_initial);
}

double clicks_to_secs(uint64_t clicks)
{
	return ((double) clicks / sysconf(_SC_CLK_TCK));
}

void open_log_files(const char *filename)
{
	char *flog_path;
	char *flog_path_summary;

	if(filename)
		flog_path         = xxstrdup(filename);
	else
		flog_path = string_format("log-monitor-%d", getpid());

	flog_path_summary = string_format("%s-summary", flog_path); 

	if((log_file = fopen(flog_path, "w")) == NULL)
	{
		fatal("could not open log file %s : %s\n", flog_path, strerror(errno));
	}
	
	if((log_file_summary = fopen(flog_path_summary, "w")) == NULL)
	{
		fatal("could not open log file %s : %s\n", flog_path_summary, strerror(errno));
	}

	free(flog_path);
	free(flog_path_summary);
}

FILE *open_proc_file(pid_t pid, char *filename)
{
		FILE *fproc;
		char fproc_path[PATH_MAX];	
		
		sprintf(fproc_path, "/proc/%d/%s", pid, filename);

		if((fproc = fopen(fproc_path, "r")) == NULL)
		{
				debug(D_DEBUG, "could not process file %s : %s\n", fproc_path, strerror(errno));
				return NULL;
		}

		return fproc;
}

/* Parse a /proc file looking for line attribute: value */
int get_int_attribute(FILE *fstatus, char *attribute, uint64_t *value)
{
	char proc_attr_line[PATH_MAX];
	int not_found = 1;
	int n = strlen(attribute);

	if(!fstatus)
		return not_found;

	proc_attr_line[PATH_MAX - 1] = '\0';

	rewind(fstatus);
	while( fgets(proc_attr_line, PATH_MAX - 2, fstatus) )
	{
		if(strncmp(attribute, proc_attr_line, n) == 0)
		{
			//We make sure that fgets got a whole line
			if(proc_attr_line[PATH_MAX - 2] == '\n')
				proc_attr_line[PATH_MAX - 2] = '\0';
			if(strlen(proc_attr_line) == PATH_MAX - 2)
				return -1;

			sscanf(proc_attr_line, "%*s %" SCNu64, value);
			not_found = 0;
			break;
		}
	}

	return not_found;
}

void initialize_limits_tree(struct tree_info *tree, int64_t val)
{
	tree->wall_time                = val;
	tree->max_concurrent_processes = val;
	tree->cpu_time                 = val;
	tree->virtual_memory           = val;
	tree->resident_memory          = val;
	tree->bytes_read               = val;
	tree->bytes_written            = val;
	tree->workdir_number_files_dirs = val;
	tree->workdir_footprint         = val;
}


//BUG from hash table!! what if resource is set to zero?
//BUG the parsing limit functions are ugly
//BUG there is no error checking of unknown vars or malformed
//values
//BUG it is assumed that the values given are integers
/* The limits string has format "var: value[, var: value]*" */
#define parse_limit_string(vars, tr, fld) if(hash_table_lookup(vars, #fld)){\
												tr->fld = (uintptr_t) hash_table_lookup(vars, #fld);\
											    debug(D_DEBUG, "Limit %s set to %" PRId64 "\n", #fld, tr->fld);}

void parse_limits_string(char *str, struct tree_info *tree)
{
	struct hash_table *vars = hash_table_create(0,0);
	char *line              = xxstrdup(str);
	char *var, *value;

	var = strtok(line, ":");
	while(var)
	{
		var = string_trim_spaces(xxstrdup(var));
		value = strtok(NULL, ",");
		if(value)
		{
			uintptr_t v = atoi(value);
			hash_table_insert(vars, var, (uintptr_t *) v);
		}
		else
			break;

		var = strtok(NULL, ":");
	}

	parse_limit_string(vars, tree, wall_time);
	parse_limit_string(vars, tree, max_concurrent_processes);
	parse_limit_string(vars, tree, cpu_time);
	parse_limit_string(vars, tree, virtual_memory);
	parse_limit_string(vars, tree, resident_memory);
	parse_limit_string(vars, tree, bytes_read);
	parse_limit_string(vars, tree, bytes_written);
	parse_limit_string(vars, tree, workdir_number_files_dirs);
	parse_limit_string(vars, tree, workdir_footprint);

	hash_table_delete(vars);
}

/* Every line of the limits file has the format resource: value */
#define parse_limit_file(file, tr, fld) if(!get_int_attribute(file, #fld ":", (uint64_t *) &tr->fld))\
											debug(D_DEBUG, "Limit %s set to %" PRId64 "\n", #fld, tr->fld);
void parse_limits_file(char *path, struct tree_info *tree)
{
	FILE *flimits = fopen(path, "r");
	
	parse_limit_file(flimits, tree, wall_time);
	parse_limit_file(flimits, tree, max_concurrent_processes);
	parse_limit_file(flimits, tree, cpu_time);
	parse_limit_file(flimits, tree, virtual_memory);
	parse_limit_file(flimits, tree, resident_memory);
	parse_limit_file(flimits, tree, bytes_read);
	parse_limit_file(flimits, tree, bytes_written);
	parse_limit_file(flimits, tree, workdir_number_files_dirs);
	parse_limit_file(flimits, tree, workdir_footprint);

}

/***
 * Reference count for filesystems and working directories auxiliary functions.
 ***/

int itable_addto_count(struct itable *table, void *key, int value)
{
	uintptr_t count = (uintptr_t) itable_lookup(table, (uintptr_t) key);
	count += value;                              //we get 0 if lookup fails, so that's ok.

	if(count > 0)
		itable_insert(table, (uintptr_t) key, (void *) count);
	else
		itable_remove(table, (uintptr_t) key);

	return count;
}

int inc_fs_count(struct filesys_info *f)
{
	int count = itable_addto_count(filesys_rc, f, 1);

	debug(D_DEBUG, "filesystem %d reference count +1, now %d references.\n", f->id, count);

	return count;
}

int dec_fs_count(struct filesys_info *f)
{
	int count = itable_addto_count(filesys_rc, f, -1);

	debug(D_DEBUG, "filesystem %d reference count -1, now %d references.\n", f->id, count);

	if(count < 1)
	{
		debug(D_DEBUG, "filesystem %d is not monitored anymore.\n", f->id, count);
		free(f->path);
		free(f);	
	}

	return count;
}

int inc_wd_count(struct wdir_info *d)
{
	int count = itable_addto_count(wdirs_rc, d, 1);

	debug(D_DEBUG, "working directory '%s' reference count +1, now %d references.\n", d->path, count); 

	return count;
}

int dec_wd_count(struct wdir_info *d)
{
	int count = itable_addto_count(wdirs_rc, d, -1);

	debug(D_DEBUG, "working directory '%s' reference count -1, now %d references.\n", d->path, count); 

	if(count < 1)
	{
		debug(D_DEBUG, "working directory '%s' is not monitored anymore.\n", d->path);

		hash_table_remove(wdirs, d->path); 

		dec_fs_count((void *) d->fs);
		free(d->path);
		free(d);	
	}

	return count;
}

/***
 * Low level resource monitor functions.
 ***/

int get_dsk_usage(const char *path, struct statfs *disk)
{
	char cwd[PATH_MAX];

	debug(D_DEBUG, "statfs on path: %s\n", path);

	if(statfs(path, disk) > 0)
	{
		debug(D_DEBUG, "could statfs on %s : %s\n", cwd, strerror(errno));
		return 1;
	}

	return 0;
}

void acc_dsk_usage(struct statfs *acc, struct statfs *other)
{
	acc->f_bfree  += other->f_bfree;
	acc->f_bavail += other->f_bavail;
	acc->f_ffree  += other->f_ffree;
}

int get_wd_usage(struct wdir_info *d)
{
	char *argv[] = {d->path, NULL};
	FTS *hierarchy;
	FTSENT *entry;

	d->files = 0;
	d->directories = 0;
	d->byte_count = 0;
	d->block_count = 0;

	hierarchy = fts_open(argv, FTS_PHYSICAL, NULL);

	if(!hierarchy)
	{
		debug(D_DEBUG, "fts_open error: %s\n", strerror(errno));
		return 1;
	}

	while( (entry = fts_read(hierarchy)) )
	{
		switch(entry->fts_info)
		{
			case FTS_D:
				d->directories++;
				break;
			case FTS_DC:
			case FTS_DP:
				break;
			case FTS_SL:
			case FTS_DEFAULT:
				d->files++;
				break;
			case FTS_F:
				d->files++;
				d->byte_count  += entry->fts_statp->st_size;
				d->block_count += entry->fts_statp->st_blocks;
				break;
			case FTS_ERR:
				debug(D_DEBUG, "fts_read error %s: %s\n", entry->fts_name, strerror(errno));
				break;
			default:
				break;
		}
	}

	fts_close(hierarchy);

	return 0;
}

void acc_wd_usage(struct wdir_info *acc, struct wdir_info *other)
{
	acc->files       += other->files;
	acc->directories += other->directories;
	acc->byte_count  += other->byte_count;
	acc->block_count += other->block_count;
}

int get_cpu_time_usage(pid_t pid, struct cpu_time_info *cpu)
{
	/* /dev/proc/[pid]/stat */

	uint64_t kernel, user;
	
	FILE *fstat = open_proc_file(pid, "stat");
	if(!fstat)
	{
		return 1;
	}

	fscanf(fstat,
			"%*s" /* pid */ "%*s" /* cmd line */ "%*s" /* state */ "%*s" /* pid of parent */
			"%*s" /* group ID */ "%*s" /* session id */ "%*s" /* tty pid */ "%*s" /* tty group ID */
			"%*s" /* linux/sched.h flags */ "%*s %*s %*s %*s" /* faults */
			"%" SCNu64 /* user mode time (in clock ticks) */
			"%" SCNu64 /* kernel mode time (in clock ticks) */
			/* .... */,
			&kernel, &user);

	cpu->delta_user_time   = clicks_to_secs(user)   - cpu->user_time;	 
	cpu->delta_kernel_time = clicks_to_secs(kernel) - cpu->kernel_time;	

	cpu->user_time   = clicks_to_secs(user);	
	cpu->kernel_time = clicks_to_secs(kernel);	

	fclose(fstat);

	return 0;
}

void acc_cpu_time_usage(struct cpu_time_info *acc, struct cpu_time_info *other)
{
	acc->delta_user_time   += other->delta_user_time;
	acc->delta_kernel_time += other->delta_kernel_time;
}

int get_mem_usage(pid_t pid, struct mem_info *mem)
{
	// /dev/proc/[pid]/statm: 
	// total-size resident shared-pages text unused data+stack unused
	
	FILE *fmem = open_proc_file(pid, "statm");
	if(!fmem)
		return 1;

	fscanf(fmem, 
			"%" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64 " %*s %" SCNu64 " %*s",
			&mem->virtual, 
			&mem->resident, 
			&mem->shared, 
			&mem->text,
			&mem->data);

	mem->shared *= sysconf(_SC_PAGESIZE); //Multiply pages by pages size.

	fclose(fmem);

	return 0;
}

void acc_mem_usage(struct mem_info *acc, struct mem_info *other)
{
		acc->virtual  += other->virtual;
		acc->resident += other->resident;
		acc->shared   += other->shared;
		acc->data     += other->data;
}

int get_io_usage(pid_t pid, struct io_info *io)
{
	// /proc/[pid]/io: if process dies before we read the file, then info is
	// lost, as if the process did not read or write any characters.

	FILE *fio = open_proc_file(pid, "io");
	uint64_t cread, cwritten;
	int rstatus, wstatus;

	if(!fio)
		return 1;

	rstatus = get_int_attribute(fio, "rchar", &cread);
	wstatus = get_int_attribute(fio, "wchar", &cwritten);

	fclose(fio);

	if(rstatus || wstatus)
		return 1;

	io->delta_chars_read    = cread - io->chars_read;
	io->delta_chars_written = cwritten - io->chars_written;

	io->chars_read = cread;
	io->chars_written = cwritten;

	return 0;
}

void acc_io_usage(struct io_info *acc, struct io_info *other)
{
	acc->delta_chars_read    += other->delta_chars_read;
	acc->delta_chars_written += other->delta_chars_written;
}

/***
 * Functions to track a working directory, or filesystem.
 ***/

int get_device_id(char *path)
{
	struct stat dinfo;

	if(stat(path, &dinfo) != 0)
	{
		debug(D_DEBUG, "stat call on '%s' failed : %s\n", path, strerror(errno));
		return -1;
	}

	return dinfo.st_dev;
}

struct filesys_info *lookup_or_create_fs(char *path)
{
	uint64_t dev_id = get_device_id(path);
	struct filesys_info *inventory = itable_lookup(filesysms, dev_id);

	if(!inventory) 
	{
		debug(D_DEBUG, "filesystem %d added to monitor.\n", dev_id);

		inventory = (struct filesys_info *) malloc(sizeof(struct filesys_info));
		inventory->path = xxstrdup(path);
		inventory->id   = dev_id;
		itable_insert(filesysms, dev_id, (void *) inventory);
		get_dsk_usage(inventory->path, &inventory->disk_initial);
	}

	inc_fs_count(inventory);

	return inventory;
}

struct wdir_info *lookup_or_create_wd(struct wdir_info *previous, char *path)
{
	struct wdir_info *inventory; 

	if(strlen(path) < 1 || access(path, F_OK) != 0)
		return previous;

	inventory = hash_table_lookup(wdirs, path);

	if(!inventory) 
	{
		debug(D_DEBUG, "working directory '%s' added to monitor.\n", path);

		inventory = (struct wdir_info *) malloc(sizeof(struct wdir_info));
		inventory->path = xxstrdup(path);
		hash_table_insert(wdirs, inventory->path, (void *) inventory);

		inventory->fs = lookup_or_create_fs(inventory->path);
	}

	if(inventory != previous)
	{
		inc_wd_count(inventory);
		if(previous)
			dec_wd_count(previous);
	}

	debug(D_DEBUG, "filesystem of %s is %d\n", inventory->path, inventory->fs->id);

	return inventory;
}

/***
 * Functions to track a single process, workind directory, or
 * filesystem.
 ***/

int monitor_process_once(struct process_info *p)
{
	debug(D_DEBUG, "monitoring process: %d\n", p->pid);

	p->wall_time = 0;

	get_cpu_time_usage(p->pid, &p->cpu);
	get_mem_usage(p->pid, &p->mem);
	get_io_usage(p->pid, &p->io);

	return 0;
}

int monitor_wd_once(struct wdir_info *d)
{
	debug(D_DEBUG, "monitoring dir %s\n", d->path);

	d->wall_time = 0;
	get_wd_usage(d);

	return 0;
}

int monitor_fs_once(struct filesys_info *f)
{
	f->wall_time = 0;

	get_dsk_usage(f->path, &f->disk);

	f->disk.f_bfree  = f->disk_initial.f_bfree  - f->disk.f_bfree;
	f->disk.f_bavail = f->disk_initial.f_bavail - f->disk.f_bavail;
	f->disk.f_ffree  = f->disk_initial.f_ffree  - f->disk.f_ffree;

	return 0;
}

/***
 * Functions to track the whole process tree.  They call the
 * functions defined just above, accumulating the resources of
 * all the processes.
***/

void monitor_processes_once(struct process_info *acc)
{
	uint64_t pid;
	struct process_info *p;

	bzero(acc, sizeof( struct process_info ));

	acc->wall_time = secs_since_launched();

	itable_firstkey(processes);
	while(itable_nextkey(processes, &pid, (void **) &p))
	{
		monitor_process_once(p);

		acc_mem_usage(&acc->mem, &p->mem);
		
		acc_cpu_time_usage(&acc->cpu, &p->cpu);

		acc_io_usage(&acc->io, &p->io);
	}
}

void monitor_wds_once(struct wdir_info *acc)
{
	struct wdir_info *d;
	char *path;

	bzero(acc, sizeof( struct wdir_info ));

	acc->wall_time = secs_since_launched();

	hash_table_firstkey(wdirs);
	while(hash_table_nextkey(wdirs, &path, (void **) &d))
	{
		monitor_wd_once(d);
		acc_wd_usage(acc, d);
	}
}

void monitor_fss_once(struct filesys_info *acc)
{
	struct   filesys_info *f;
	uint64_t dev_id;

	bzero(acc, sizeof( struct filesys_info ));

	acc->wall_time = secs_since_launched();

	itable_firstkey(filesysms);
	while(itable_nextkey(filesysms, &dev_id, (void **) &f))
	{
		monitor_fs_once(f);
		acc_dsk_usage(&acc->disk, &f->disk);
	}
}

/***
 * Logging functions. The process tree is summarized in struct
 * tree_info's, computing current value, maximum, and minimums.
***/

void monitor_summary_header()
{
	int i;

	fprintf(log_file, "#");
	for(i = 0; resources[i]; i++)
		fprintf(log_file, "%s\t", resources[i]);

	fprintf(log_file, "\n");
}

void monitor_collate_tree(struct tree_info *tr, struct process_info *p, struct wdir_info *d, struct filesys_info *f)
{
	tr->wall_time                = secs_since_launched();

	tr->max_concurrent_processes = (int64_t) itable_size(processes);
	tr->cpu_time                 = (p->cpu.delta_user_time + p->cpu.delta_kernel_time) + tr->cpu_time;
	tr->virtual_memory           = (int64_t) p->mem.virtual;
	tr->resident_memory          = (int64_t) p->mem.resident;
	tr->bytes_read               = (int64_t) (p->io.delta_chars_read + tr->bytes_read);
	tr->bytes_written            = (int64_t) (p->io.delta_chars_written + tr->bytes_written);
	tr->workdir_number_files_dirs = (int64_t) (d->files + d->directories);
	tr->workdir_footprint         = (int64_t) d->byte_count;

	tr->fs_nodes                 = (int64_t) f->disk.f_ffree;
}

void monitor_find_max_tree(struct tree_info *result, struct tree_info *tr)
{
	if(result->wall_time < tr->wall_time)
		result->wall_time = tr->wall_time;

	if(result->max_concurrent_processes < tr->max_concurrent_processes)
		result->max_concurrent_processes = tr->max_concurrent_processes;

	if(result->cpu_time < tr->cpu_time)
		result->cpu_time = tr->cpu_time;

	if(result->virtual_memory < tr->virtual_memory)
		result->virtual_memory = tr->virtual_memory;

	if(result->resident_memory < tr->resident_memory)
		result->resident_memory = tr->resident_memory;

	if(result->bytes_read < tr->bytes_read)
		result->bytes_read = tr->bytes_read;

	if(result->bytes_written < tr->bytes_written)
		result->bytes_written = tr->bytes_written;

	if(result->workdir_number_files_dirs < tr->workdir_number_files_dirs)
		result->workdir_number_files_dirs = tr->workdir_number_files_dirs;

	if(result->workdir_footprint < tr->workdir_footprint)
		result->workdir_footprint = tr->workdir_footprint;

	if(result->fs_nodes < tr->fs_nodes)
		result->fs_nodes = tr->fs_nodes;
}

void monitor_log_row(struct tree_info *tr)
{
	fprintf(log_file, "%lf\t", tr->wall_time + secs_initial);
	fprintf(log_file, "%" PRId64 "\t", tr->max_concurrent_processes);
	fprintf(log_file, "%lf\t", tr->cpu_time);
	fprintf(log_file, "%" PRId64 "\t", tr->virtual_memory);
	fprintf(log_file, "%" PRId64 "\t", tr->resident_memory);
	fprintf(log_file, "%" PRId64 "\t", tr->bytes_read);
	fprintf(log_file, "%" PRId64 "\t", tr->bytes_written);
                               
	fprintf(log_file, "%" PRId64 "\t", tr->workdir_number_files_dirs);
	fprintf(log_file, "%lf\n", ((double) tr->workdir_footprint / ONE_MEGABYTE));
                               
	/* are we going to keep monitoring the whole filesystem? */
	// fprintf(log_file, "%" PRId64 "\n", tr->fs_nodes);

}


int monitor_final_summary()
{
	int status = 0;

	if(over_limit_str)
		status = -1;

	fprintf(log_file_summary, "%-30s\t%" PRId64 "\n", "max_concurrent_processes:", tree_max.max_concurrent_processes);
	fprintf(log_file_summary, "%-30s\t%lf\n",         "wall_time:", tree_max.wall_time);
	fprintf(log_file_summary, "%-30s\t%lf\n",         "cpu_time:", tree_max.cpu_time);
	fprintf(log_file_summary, "%-30s\t%" PRId64 "\n", "virtual_memory:", tree_max.virtual_memory);
	fprintf(log_file_summary, "%-30s\t%" PRId64 "\n", "resident_memory:", tree_max.resident_memory);
	fprintf(log_file_summary, "%-30s\t%" PRId64 "\n", "bytes_read:", tree_max.bytes_read);
	fprintf(log_file_summary, "%-30s\t%" PRId64 "\n", "bytes_written:", tree_max.bytes_written);
	fprintf(log_file_summary, "%-30s\t%" PRId64 "\n", "workdir_number_files_dirs:", tree_max.workdir_number_files_dirs);
	fprintf(log_file_summary, "%-30s\t%lf\n",         "workdir_footprint:", ((double) tree_max.workdir_footprint/ONE_MEGABYTE));

	if(status && !first_process_exit_status)
		return status;
	else
		return first_process_exit_status;
}

/***
 * Functions that modify the processes tracking table, and
 * cleanup of processes in the zombie state.
 ***/

int ping_process(pid_t pid)
{
	return (kill(pid, 0) == 0);
}


void monitor_track_process(pid_t pid)
{
	char *newpath;
	struct process_info *p; 
	
	if(!ping_process(pid))
		return;

	p = itable_lookup(processes, pid);

	if(p)
		return;

	p = malloc(sizeof(struct process_info));
	bzero(p, sizeof(struct process_info));

	p->pid = pid;
	p->running = 0;

	newpath = getcwd(NULL, 0);
	p->wd   = lookup_or_create_wd(NULL, newpath);
	free(newpath);

	itable_insert(processes, p->pid, (void *) p);

	p->running = 1;
}

void monitor_untrack_process(uint64_t pid)
{
	struct process_info *p = itable_lookup(processes, pid);

	if(p)
		p->running = 0;
}

void ping_processes(void)
{
	uint64_t pid;
	struct process_info *p;

	itable_firstkey(processes);
	while(itable_nextkey(processes, &pid, (void **) &p))
		if(!ping_process(pid))
		{
			debug(D_DEBUG, "cannot find %d process.\n", pid);
			monitor_untrack_process(pid);
		}
}

void cleanup_zombie(struct process_info *p)
{
	int status;

	debug(D_DEBUG, "cleaning process: %d\n", p->pid);

	//die zombie die
	waitpid(p->pid, &status, WNOHANG);

	if(p->pid == first_process_pid)
	{
		first_process_exit_status = WEXITSTATUS(status);

		/* BUG: Need to add a call to getrusage here */
		/* BUG: end and wall_time should be computed in a final
		 * summary, not here */
		tree_max.wall_time = secs_since_epoch() - secs_initial;

		fprintf(log_file_summary, "%-30s\t%lf\n", "end:", tree_max.wall_time + secs_initial);

		if( WIFEXITED(status) )
		{
			debug(D_DEBUG, "process %d finished: %d.\n", p->pid, first_process_exit_status );
			fprintf(log_file_summary, "%-30s\tnormal\n", "exit_type:");
		} 
		else if ( WIFSIGNALED(status) )
		{
			debug(D_DEBUG, "process %d terminated: %s.\n", p->pid, strsignal(WTERMSIG(status)) );

			if(over_limit_str)
			{
				fprintf(log_file_summary, "%-30s\tlimit\n", "exit_type:");
				fprintf(log_file_summary, "%-30s\t%s\n", "limits_exceeded:", over_limit_str);
			}
			else
			{
				fprintf(log_file_summary, "%-30s\tsignal\n", "exit_type:");
				fprintf(log_file_summary, "%-30s\t%d %s\n", "signal:", WTERMSIG(status), strsignal(WTERMSIG(status)));
			}
		} 

		fprintf(log_file_summary, "%-30s\t%d\n", "exit_status:", first_process_exit_status);
	}

	if(p->wd)
		dec_wd_count(p->wd);

	itable_remove(processes, p->pid);
	free(p);
}

void cleanup_zombies(void)
{
	uint64_t pid;
	struct process_info *p;

	itable_firstkey(processes);
	while(itable_nextkey(processes, &pid, (void **) &p))
		if(!p->running)
			cleanup_zombie(p);
}

// Return the pid of the child that sent the signal, without removing the
// waitable state.
pid_t waiting_child()
{
	pid_t pid;
	
#if defined(__APPLE__) || defined(__FreeBSD__)
	int status;
	pid = wait4(-1, &status, WNOWAIT, NULL);
#else
	siginfo_t cinfo;
	if(waitid(P_ALL, 0, &cinfo, WEXITED | WNOWAIT) == 0)
		pid = cinfo.si_pid;
	else
		return -1;
#endif

	return pid;
}

/* sigchild signal handler */
void monitor_check_child(const int signal)
{
	uint64_t pid;

	//zombie, tell us who you were!
	pid = waiting_child();

	debug(D_DEBUG, "SIGCHLD from %d\n", pid);
	struct process_info *p = itable_lookup(processes, pid);
	if(!p)
		return;

	if(p->pid == first_process_pid)
	{
		debug(D_DEBUG, "adding all processes to cleanup list.\n");
		itable_firstkey(processes);
		while(itable_nextkey(processes, &pid, (void **) &p))
			monitor_untrack_process(pid);
	}
	else
	{
		debug(D_DEBUG, "adding process %d to cleanup list.\n", pid);
		monitor_untrack_process(p->pid);
	}
}

//SIGINT, SIGQUIT, SIGTERM signal handler.
void monitor_final_cleanup(int signum)
{
	uint64_t pid;
	struct   process_info *p;
	int      status;

	signal(SIGCHLD, SIG_DFL);

	//ask politely to quit
	itable_firstkey(processes);
	while(itable_nextkey(processes, &pid, (void **) &p))
	{
		debug(D_DEBUG, "sending %s to process %d.\n", strsignal(signum), pid);

		kill(pid, signum);
	}

	ping_processes();
	cleanup_zombies();

	if(itable_size(processes) > 0)
		sleep(1);

	//we did ask...
	itable_firstkey(processes);
	while(itable_nextkey(processes, &pid, (void **) &p))
	{
		debug(D_DEBUG, "sending %s to process %d.\n", strsignal(SIGKILL), pid);

		kill(pid, SIGKILL);

		monitor_untrack_process(pid);
	}

	cleanup_zombies();

	if(lib_helper_extracted)
		unlink(lib_helper_name);

	status = monitor_final_summary();

	fclose(log_file);
	fclose(log_file_summary);

	exit(status);
}

// The following keeps getting uglier and uglier! Rethink how to do it!
//
#define over_limit_check(tr, fld, fmt)\
	if(tr->fld > 0 && tree_limits.fld - tr->fld < 0)\
	{\
		char *tmp;\
		if(over_limit_str)\
		{\
			tmp = string_format("%s, " #fld " %" fmt " > %" fmt, over_limit_str, (tr)->fld, tree_limits.fld);\
			free(over_limit_str);\
			over_limit_str = tmp;\
		}\
		else\
			over_limit_str = string_format(#fld " %" fmt " > %" fmt, (tr)->fld, tree_limits.fld);\
	}

/* return 0 means above limit, 1 means limist ok */
int monitor_check_limits(struct tree_info *tr)
{
	over_limit_str = NULL;

	over_limit_check(tr, wall_time, "lf");
	over_limit_check(tr, max_concurrent_processes, PRId64);
	over_limit_check(tr, cpu_time, "lf");
	over_limit_check(tr, virtual_memory, PRId64);
	over_limit_check(tr, resident_memory, PRId64);
	over_limit_check(tr, bytes_read, PRId64);
	over_limit_check(tr, bytes_written, PRId64);
	over_limit_check(tr, workdir_number_files_dirs, PRId64);
	over_limit_check(tr, workdir_footprint, PRId64);

	if(over_limit_str)
		return 0;
	else
		return 1;
}

/***
 * Functions that communicate with the helper library,
 * (un)tracking resources as messages arrive.
***/

void write_helper_lib(void)
{	
	FILE *flib;
	uint64_t n;

	if(access(lib_helper_name, R_OK | X_OK) == 0)
	{
		lib_helper_extracted = 0;
		return;
	}

	flib = fopen(lib_helper_name, "w");
	if(!flib)
		return;

	n = sizeof(lib_helper_data);

	copy_buffer_to_stream(lib_helper_data, flib, n);
	fclose(flib);

	chmod(lib_helper_name, 0777);

	lib_helper_extracted = 1;
}

void monitor_dispatch_msg(void)
{
	struct monitor_msg msg;
	struct process_info *p;

	recv_monitor_msg(monitor_queue_fd, &msg);

	debug(D_DEBUG,"message \"%s\" from %d\n", str_msgtype(msg.type), msg.origin);

	p = itable_lookup(processes, (uint64_t) msg.origin);

	if(!p && msg.type != BRANCH)
	/* We either got a malformed message, or a message that
	 * recently terminated. There is not much we can do right
	 * now, but to ignore the message */
		return;

	switch(msg.type)
	{
		case BRANCH:
			monitor_track_process(msg.origin);
			if(tree_max.max_concurrent_processes < itable_size(processes))
				tree_max.max_concurrent_processes = itable_size(processes);
			break;
		case END:
			monitor_untrack_process(msg.data.p);
			break;
		case CHDIR:
			p->wd = lookup_or_create_wd(p->wd, msg.data.s);
			break;
		case OPEN:
			break;
		case READ:
			break;
		case WRITE:
			break;
		default:
			break;
	};

	if(!monitor_check_limits(&tree_max))
		monitor_final_cleanup(SIGTERM);
}

int wait_for_messages(int interval)
{
	struct timeval timeout;

	/* wait for interval miliseconds. */
	timeout.tv_sec  = 0;
	timeout.tv_usec = 0;

	debug(D_DEBUG, "sleeping for: %ld seconds\n", interval);

	//If grandchildren processes cannot talk to us, simply wait.
	//Else, wait, and check socket for messages.
	if(monitor_queue_fd < 0)
	{
		select(0, NULL, NULL, NULL, &timeout);
	}
	else
	{
		int count = 1;
		while(count > 0)
		{
			fd_set rset;
			FD_ZERO(&rset);
			FD_SET(monitor_queue_fd, &rset);
			timeout.tv_sec   = interval;
			timeout.tv_usec  = 0;
			interval = 0;                     //Next loop we do not wait at all
			count = select(monitor_queue_fd + 1, &rset, NULL, NULL, &timeout);

			if(count > 0)
				monitor_dispatch_msg();
		}
	}

	return 0;
}

/***
 * Functions to fork the very first process. This process is
 * created and suspended before execv, until a SIGCONT is sent
 * from the monitor. 
***/

//Very first process signal handler.
void wakeup_after_fork(int signum)
{
	if(signum == SIGCONT)
		signal(SIGCONT, SIG_DFL);
}

pid_t monitor_fork(void)
{
	pid_t pid;
	sigset_t set;
	void (*prev_handler)(int signum);

	pid = fork();

	prev_handler = signal(SIGCONT, wakeup_after_fork);
	sigfillset(&set);
	sigdelset(&set, SIGCONT);

	if(pid > 0)
	{
		debug(D_DEBUG, "fork %d -> %d\n", getpid(), pid);

		monitor_track_process(pid);

		signal(SIGCONT, prev_handler);
		kill(pid, SIGCONT);
	}
	else
	{
		//sigsuspend(&set);
		signal(SIGCONT, prev_handler);
	}

	return pid;
}

struct process_info *spawn_first_process(const char *cmd)
{
	pid_t pid;

	fprintf(log_file_summary, "command: %s\n", cmd);
	fprintf(log_file_summary, "%-30s\t%lf\n", "start:", secs_initial);

	pid = monitor_fork();

	monitor_summary_header();

	if(pid > 0)
	{
		first_process_pid = pid;
		close(STDIN_FILENO);
		close(STDOUT_FILENO);
		setpgid(pid, 0);
	}
	else if(pid < 0)
		fatal("fork failed: %s\n", strerror(errno));
	else //child
	{
		debug(D_DEBUG, "executing: %s\n", cmd);
		execlp("sh", "sh", "-c", cmd, (char *) NULL);
		//We get here only if execlp fails.
		fatal("error executing %s:\n", cmd, strerror(errno));
	}

	return itable_lookup(processes, pid);

}


static void show_help(const char *cmd)
{
	fprintf(stdout, "Use: %s [options] command-line-and-options\n", cmd);
	fprintf(stdout, "-i <n>			Interval bewteen observations, in seconds. (default=%d)\n", DEFAULT_INTERVAL);
	fprintf(stdout, "-d <subsystem>		Enable debugging for this subsystem.\n");
	fprintf(stdout, "-l <maxfile>		Use maxfile with list of var: value pairs for resource limits.");
	fprintf(stdout, "-o <logfile>		Write log to logfile (default=log-PID)\n");
}


int monitor_resources(long int interval /*in seconds */)
{
	uint64_t round;

	struct process_info  p;
	struct wdir_info     d;
	struct filesys_info  f;

	struct tree_info     tree_now;
	bzero(&tree_now, sizeof(struct tree_info));

	// Loop while there are processes to monitor, that is 
	// itable_size(processes) > 0). The check is done again in a
	// if/break pair below to mitigate a race condition in which
	// the last process exits after the while(...) is tested, but
	// before we reach select.
	round = 1;
	while(itable_size(processes) > 0)
	{ 
		ping_processes();

		monitor_processes_once(&p);
		monitor_wds_once(&d);
		monitor_fss_once(&f);

		monitor_collate_tree(&tree_now, &p, &d, &f);

		if(round == 1)
			memcpy(&tree_max, &tree_now, sizeof(struct tree_info));
		else
			monitor_find_max_tree(&tree_max, &tree_now);

		monitor_log_row(&tree_now);

		if(!monitor_check_limits(&tree_now))
			monitor_final_cleanup(SIGTERM);

		cleanup_zombies();
		//If no more process are alive, break out of loop.
		if(itable_size(processes) < 1)
			break;
		
		wait_for_messages(interval);

		//cleanup processes which by terminating may have awaken
		//select.
		cleanup_zombies();

		round++;
	}

	return 0;
}

int main(int argc, char **argv) {
	int i;
	char cmd[1024] = {'\0'};
	char c;
	uint64_t interval = DEFAULT_INTERVAL;
	char *log_path = NULL;

	debug_config(argv[0]);

	signal(SIGCHLD, monitor_check_child);
	signal(SIGINT,  monitor_final_cleanup);
	signal(SIGQUIT, monitor_final_cleanup);
	signal(SIGTERM, monitor_final_cleanup);

	secs_initial = secs_since_epoch();
	initialize_limits_tree(&tree_limits, INTMAX_MAX);

	while((c = getopt(argc, argv, "d:i:L:l:o:")) > 0)
	{
		switch (c) {
			case 'd':
				debug_flags_set(optarg);
				break;
			case 'i':
				interval = strtoll(optarg, NULL, 10);
				if(interval < 1)
					fatal("interval cannot be set to less than one second.");
				break;
			case 'o':
				log_path = xxstrdup(optarg);
				break;
			case 'l':
				parse_limits_file(optarg, &tree_limits);
				break;
			case 'L':
				parse_limits_string(optarg, &tree_limits);
				break;
			default:
				show_help(argv[0]);
				return 1;
				break;
		}
	}

#ifdef CCTOOLS_USE_RMONITOR_HELPER_LIB
	write_helper_lib();
	monitor_helper_init("./librmonitor_helper.so", &monitor_queue_fd);
#endif

	processes    = itable_create(0);
	wdirs = hash_table_create(0,0);
	filesysms    = itable_create(0);
	
	wdirs_rc = itable_create(0);
	filesys_rc      = itable_create(0);

	//this is ugly, concatenating command and arguments
	if(optind < argc)
	{
		for(i = optind; i < argc; i++)
		{
			strcat(cmd, argv[i]);
			strcat(cmd, " ");
		}
	}
	else
	{
		show_help(argv[0]);
		return 1;
	}
	
	open_log_files(log_path);

	spawn_first_process(cmd);

	monitor_resources(interval);

	monitor_final_cleanup(SIGTERM);

	return 0;
}

