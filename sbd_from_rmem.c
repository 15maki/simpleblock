/*
 * A sample, extra-simple block driver. Updated for kernel 2.6.31.
 *
 * (C) 2003 Eklektix, Inc.
 * (C) 2010 Pat Patterson <pat at superpat dot com>
 * Redistributable under the terms of the GNU GPL.
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>

#include <linux/kernel.h> /* printk() */
#include <linux/fs.h>     /* everything... */
#include <linux/errno.h>  /* error codes */
#include <linux/types.h>  /* size_t */
#include <linux/vmalloc.h>
#include <linux/genhd.h>
#include <linux/blkdev.h>
#include <linux/hdreg.h>
#include <linux/random.h>

#include <linux/time.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>

MODULE_LICENSE("Dual BSD/GPL");

static int major_num = 0;
module_param(major_num, int, 0);
 
static int npages = 2048 * 1024; 
module_param(npages, int, 0); 

static int get_record = 0;
module_param(get_record, int, 0);

size_t logical_block_size = 4096;

/*
 * We can tweak our hardware sector size, but the kernel talks to us
 * in terms of small sectors, always.
 */
#define KERNEL_SECTOR_SIZE 	512
#define SECTORS_PER_PAGE	(logical_block_size / KERNEL_SECTOR_SIZE)
#define MERGE 0
/*
 * Our request queue
 */
static struct request_queue *Queue;

/*
 * The internal representation of our device.
 */
static struct sbd_device {
	unsigned long size;
	spinlock_t lock;
	u8 **data;
	struct gendisk *gd;
} device;

typedef struct 
{
	long timestamp; 
	int page;
	int length;
	int batch;
} __attribute__((packed)) access_record;

#define RECORD_SIZE 20

u64 inject_latency = 0;

/* latency in ns: default 1 us */
u64 latency_ns = 1000ULL;

u64 end_to_end_latency_ns = 0ULL;

/* bandwidth in bps: default 10 Gbps */
u64 bandwidth_bps = 10000000000ULL;

/* read/write statistics in bytes */
atomic64_t counter_read;
atomic64_t counter_write;
u64 line_count = 0; 
int batch_count = 0;



spinlock_t rx_lock;
spinlock_t tx_lock;
spinlock_t log_lock;
spinlock_t cdf_lock;

#define LOG_BATCH_SIZE	2000000
access_record* request_log = NULL;
#define FCT_MAX_SIZE 4096
u64 fct_by_size[FCT_MAX_SIZE];
int fct_record_count = 0;
int log_head = 0;
int log_tail = 0;
u64 overflow = 0;
u64 version = 5;



static u64 get_fct(int batch_size)
{
  int i;
  int start = batch_size < FCT_MAX_SIZE? batch_size:FCT_MAX_SIZE-1;
  for(i = start; i >= 0; i--)
    if (fct_by_size[i])
      return fct_by_size[i];
  return 0;
}

/*
 * Handle an I/O request.
 */
static void sbd_transfer(struct sbd_device *dev, sector_t sector,
		unsigned long nsect, char *buffer, int write, u64 slowdown) 
{
	int i;
	int page;
	int npage;
	u64 begin = 0ULL;
//	struct timeval tms;
//	access_record record;
        struct timespec time;
        long timestamp;

	if (sector % SECTORS_PER_PAGE != 0 || nsect % SECTORS_PER_PAGE != 0) {
		pr_err("incorrect align: %lu %lu %d\n", sector, nsect, write);
		return;
	}
	//page = sector / SECTORS_PER_PAGE;
	//npage = nsect / SECTORS_PER_PAGE;

	page = sector / SECTORS_PER_PAGE;
    npage = nsect / SECTORS_PER_PAGE;
//	printk("page=%d,npage=%d,nsect=%ld,sectorsperpage=%ld",page,npage,nsect,SECTORS_PER_PAGE);

	if (page + npage - 1 >= npages) {
		printk (KERN_NOTICE "sbd: Beyond-end write (%d %d %d)\n", page, npage, npages);
		return;
	}

//	if(get_record){
//		do_gettimeofday(&tms);
//		record.timestamp = tms.tv_sec * 1000 * 1000 + tms.tv_usec;
//	}

	if(inject_latency)
		begin = sched_clock();

	if (write) {
                getnstimeofday(&time);
                timestamp = time.tv_sec * 1000000000L + time.tv_nsec;
  //              printk("/write=%ld\n",timestamp);
		spin_lock(&tx_lock);
		for (i = 0; i < npage; i++)
            memcpy(dev->data[page+i], buffer, logical_block_size);
		atomic64_add(npage * logical_block_size, &counter_write);


		if(inject_latency){
//      begin = sched_clock();
			while ((sched_clock() - begin) < 
					(((npage * logical_block_size * 8ULL) * 1000000000) / bandwidth_bps) * slowdown / 10000) {
				/* wait for transmission delay */
				;
			}
		}


		spin_unlock(&tx_lock);
	} else {
                getnstimeofday(&time);
                timestamp = time.tv_sec * 1000000000L + time.tv_nsec;
                //printk("/read=%ld\n",timestamp);
		printk("page=%d\n",page);
		spin_lock(&rx_lock);

		for (i = 0; i < npage; i++)
			memcpy(buffer,dev->data[page+i], logical_block_size);
		atomic64_add(npage * logical_block_size, &counter_read);		

		
		if (inject_latency){
//			begin = sched_clock();
			while ((sched_clock() - begin) < 
					(((npage * logical_block_size * 8ULL) * 1000000000) / bandwidth_bps) * slowdown / 10000) {
				/* wait for transmission delay */
				;
			}
		}

//		if(get_record)
//			record.timestamp = record.timestamp * -1;

		spin_unlock(&rx_lock);
	}
  /*
	if(get_record){
		record.page = page;
		//record.length = npage;
		record.count = count;
	
		spin_lock(&log_lock);
		request_log[log_head] = record;
		line_count += npage;
		log_head = (log_head + 1)%LOG_BATCH_SIZE;
		if(log_head == log_tail)
			overflow = 1;
		spin_unlock(&log_lock);	
	}
  */
}



static void sbd_request(struct request_queue *q) 
{
	struct request *req;
	u64 begin = 0ULL;
	u64 slowdown = 10000;
	access_record record;
	struct timeval tms;
	int count = 0;
	int last_page = -2;
	int last_dir = -1;

	bool has_req = false;
	sector_t sector, last_sector;
	unsigned long nsect, last_nsect;
	char *buffer, *last_buffer;
	int write, last_write;

	if(inject_latency){
		begin = sched_clock();
		while ((sched_clock() - begin) < latency_ns) {
			/* wait for RTT latency */
			;
		}
	}

	req = blk_fetch_request(q);
	if(req){
		if(get_record){
			spin_lock(&log_lock);
			record.batch = batch_count++;
			spin_unlock(&log_lock);
			do_gettimeofday(&tms);
			record.timestamp = tms.tv_sec * 1000 * 1000 + tms.tv_usec;
		}
		while (req != NULL) {
			// blk_fs_request() was removed in 2.6.36 - many thanks to
			// Christian Paro for the heads up and fix...
			//if (!blk_fs_request(req)) {
			if (req == NULL || (req->cmd_type != REQ_TYPE_FS)) {
				printk (KERN_NOTICE "Skip non-CMD request\n");
				__blk_end_request_all(req, -EIO);
				continue;
			}
			if(get_record)
			{
				if(rq_data_dir(req) == last_dir && last_page + 1 == blk_rq_pos(req) / SECTORS_PER_PAGE)
				{
					record.length++;
				}
				else
				{
					if(last_dir != -1 || last_page != -2)
					{
						spin_lock(&log_lock);
				                request_log[log_head] = record;
				                log_head = (log_head + 1)%LOG_BATCH_SIZE;
				                if(log_head == log_tail)
                        				overflow = 1;
						spin_unlock(&log_lock);
					}
					record.length = 1;
					record.page = blk_rq_pos(req) / SECTORS_PER_PAGE * (rq_data_dir(req)?1:-1);
				}
			}
			#if MERGE
			if(!has_req)
			{
				last_sector = blk_rq_pos(req);
				last_nsect = blk_rq_cur_sectors(req);
				last_buffer = bio_data(req->bio);
				last_write = rq_data_dir(req);
				has_req = true;
			}
			else
			{
				sector = blk_rq_pos(req);
				nsect = blk_rq_cur_sectors(req);
				buffer = bio_data(req->bio);
				write = rq_data_dir(req);
				if(last_sector + last_nsect == sector && last_buffer + nsect * KERNEL_SECTOR_SIZE == buffer && write == last_write)
				{
					last_nsect += nsect;
				}
				else
				{
					sbd_transfer(&device, last_sector, last_nsect, last_buffer, last_write, slowdown);
					last_sector = sector;
					last_nsect = nsect;
					last_buffer = buffer;
					last_write = write;
				}
			}
			#else
			sbd_transfer(&device, blk_rq_pos(req), blk_rq_cur_sectors(req),
					bio_data(req->bio), rq_data_dir(req), slowdown);
			#endif
			if(get_record)
			{
				last_dir = rq_data_dir(req);
				last_page = blk_rq_pos(req) / SECTORS_PER_PAGE;
			}
			count++;
			if ( ! __blk_end_request_cur(req, 0) ) {
				#if MERGE
				sbd_transfer(&device, last_sector, last_nsect, last_buffer, last_write, slowdown);
				has_req = false;
				#endif
				req = blk_fetch_request(q);
			}
		}
		if(get_record)
		{
			spin_lock(&log_lock);
	                request_log[log_head] = record;
	                log_head = (log_head + 1)%LOG_BATCH_SIZE;
	                if(log_head == log_tail)
				overflow = 1;
			spin_unlock(&log_lock);
		}
	}
	if(fct_record_count){
    u64 fct;
    begin = sched_clock();
		fct = get_fct(count);
		while ((sched_clock() - begin) < fct) {
			// wait for RTT latency */
			;
		}
	}
}

/*
 * The HDIO_GETGEO ioctl is handled in blkdev_ioctl(), which
 * calls this. We need to implement getgeo, since we can't
 * use tools such as fdisk to partition the drive otherwise.
 */
int sbd_getgeo(struct block_device * block_device, struct hd_geometry * geo) {
	long size;

	/* We have no real geometry, of course, so make something up. */
	size = device.size * (logical_block_size / KERNEL_SECTOR_SIZE);
	geo->cylinders = (size & ~0x3f) >> 6;
	geo->heads = 4;
	geo->sectors = 16;
	geo->start = 0;
	return 0;
}

/*
 * The device operations structure.
 */
static struct block_device_operations sbd_ops = {
	.owner  = THIS_MODULE,
	.getgeo = sbd_getgeo
};

static struct ctl_table sbd_table[] = {
	{
		.procname	= "latency_ns",
		.data		= &latency_ns,
		.maxlen		= sizeof(latency_ns),
		.mode		= 0644,
		.proc_handler	= proc_doulongvec_minmax,
	},
	{
		.procname	= "end_to_end_latency_ns",
		.data		= &end_to_end_latency_ns,
		.maxlen		= sizeof(end_to_end_latency_ns),
		.mode		= 0644,
		.proc_handler	= proc_doulongvec_minmax,
	},
	{
		.procname	= "bandwidth_bps",
		.data		= &bandwidth_bps,
		.maxlen		= sizeof(bandwidth_bps),
		.mode		= 0644,
		.proc_handler	= proc_doulongvec_minmax,
	},
	{
		.procname	= "read_bytes",
		.data		= &counter_read.counter,
		.maxlen		= sizeof(counter_read.counter),
		.mode		= 0644,
		.proc_handler	= proc_doulongvec_minmax,
	},
	{
		.procname	= "write_bytes",
		.data		= &counter_write.counter,
		.maxlen		= sizeof(counter_write.counter),
		.mode		= 0644,
		.proc_handler	= proc_doulongvec_minmax,
	},
	{
		.procname	= "overflow",
		.data		= &overflow,
		.maxlen		= sizeof(overflow),
		.mode		= 0644,
		.proc_handler	= proc_doulongvec_minmax,
	},
	{
		.procname	= "line_count",
		.data		= &line_count,
		.maxlen		= sizeof(line_count),
		.mode		= 0644,
		.proc_handler	= proc_doulongvec_minmax,
	},
	{
		.procname	= "inject_latency",
		.data		= &inject_latency,
		.maxlen		= sizeof(inject_latency),
		.mode		= 0644,
		.proc_handler	= proc_doulongvec_minmax,
	},
	{
		.procname	= "get_record",
		.data		= &get_record,
		.maxlen		= sizeof(get_record),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_minmax,
	},
	{
		.procname	= "version",
		.data		= &version,
		.maxlen		= sizeof(version),
		.mode		= 0644,
		.proc_handler	= proc_doulongvec_minmax,
	},
	{ }
};

static struct ctl_table sbd_root[] = {
	{
		.procname	= "sbd",
		.mode		= 0555,
		.child		= sbd_table,
	},
	{ }
};

static struct ctl_table dev_root[] = {
	{
		.procname	= "fs",
		.mode		= 0555,
		.child		= sbd_root,
	},
	{ }
};

static struct ctl_table_header *sysctl_header;



//read log 
static struct proc_dir_entry* log_file;

static int log_show(struct seq_file *m, void *v)
{
	int i;
	//pr_info("h%d t%d\n", log_head, log_tail);
	spin_lock(&log_lock);
	for(i = 0; i < 200 && log_tail != log_head; i++){
		//seq_printf(m, "%d %ld %d %d %d\n", log_tail, request_log[log_tail].timestamp, 
		//request_log[log_tail].page, request_log[log_tail].length, request_log[log_tail].count);
		seq_write(m, &(request_log[log_tail]), RECORD_SIZE);
		log_tail = (log_tail + 1)%LOG_BATCH_SIZE;
	}
	spin_unlock(&log_lock);
  return 0;
} 

static int log_open(struct inode *inode, struct file *file)
{
    return single_open(file, log_show, NULL);
}

static const struct file_operations log_fops = {
	.owner	= THIS_MODULE,
	.open	= log_open,
	.read	= seq_read,
	.llseek	= seq_lseek,
	.release= single_release,
};
//end read log


//set cdf
static struct proc_dir_entry* cdf_file;

static int cdf_show(struct seq_file *m, void *v)
{
  int i;
  printk(KERN_INFO "Print FCT\n");
  //printk(KERN_INFO "%llu\n", get_rand());
  seq_printf(m, "Total record count %d\n", fct_record_count);
  for (i = 0; i < FCT_MAX_SIZE; i++)
  {
    if(fct_by_size[i])
      seq_printf(m, "%d %llu\n", i, fct_by_size[i]);
  }
  return 0;
}

static int cdf_open(struct inode * sp_inode, struct file *sp_file)
{
  return single_open(sp_file, cdf_show, NULL);
}



static ssize_t cdf_write(struct file *sp_file, const char __user *buf, size_t size, loff_t *offset)
{
  u64 fct;
  int sz;
  spin_lock(&cdf_lock);
  sscanf(buf, "%d %llu", &sz, &fct);
  //printk(KERN_INFO "Writing CDF\n");
  if(sz < FCT_MAX_SIZE && sz > 0)
  {
    fct_by_size[sz] = fct;
    fct_record_count++;
  }
  else
  {
    printk(KERN_INFO "Wrong size: %d, fct %llu\n", sz, fct);
  }
  spin_unlock(&cdf_lock);
  return size;
}

static struct file_operations cdf_fops = {
  .open = cdf_open,
  .read = seq_read,
  .write = cdf_write,
  .llseek = seq_lseek,
  .release = single_release
};
//end set cdf


static int __init sbd_init(void) {
	int i;
	if(sizeof(access_record) != RECORD_SIZE)
		return -ENOMEM;

	pr_info("%d, %p", get_record, request_log);
	if(get_record && request_log == NULL)
	{
		request_log = (access_record*)vmalloc(sizeof(access_record) * LOG_BATCH_SIZE);
		pr_info("Allocated space for %d", LOG_BATCH_SIZE);
	}
	for(i = 0; i < FCT_MAX_SIZE; i++)
		fct_by_size[i] = 0;

  
	pr_info("logical_block_size: %lu", logical_block_size);
	
	spin_lock_init(&rx_lock);
	spin_lock_init(&tx_lock);
	spin_lock_init(&log_lock);
	spin_lock_init(&cdf_lock);

	log_file = proc_create("sbd_log", 0666, NULL, &log_fops);
	cdf_file = proc_create("sbd_cdf", 0666, NULL, &cdf_fops);

	if (!log_file || !cdf_file) {
		return -ENOMEM;
	}


	/*
	 * Set up our internal device.
	 */
	device.size = npages * logical_block_size;
	spin_lock_init(&device.lock);

	device.data = vmalloc(npages * logical_block_size);
	if (device.data == NULL)
		return -ENOMEM;

	for (i = 0; i < npages; i++) {
		device.data[i] = kmalloc(logical_block_size, GFP_KERNEL);
		if (device.data[i] == NULL) {
			int j;
			for (j = 0; j < i - 1; j++)
				kfree(device.data[i]);
			vfree(device.data);
			return -ENOMEM;
		}

		memset(device.data[i], 0, logical_block_size);
		if (i % 100000 == 0)
			pr_info("sbd: allocated %dth page\n", i);
	}

	/*
	 * Get a request queue.
	 */

	Queue = blk_init_queue(sbd_request, &device.lock);
	if (Queue == NULL)
		goto out;
	blk_queue_physical_block_size(Queue, logical_block_size);
	blk_queue_logical_block_size(Queue, logical_block_size);
	blk_queue_io_min(Queue, logical_block_size);
	blk_queue_io_opt(Queue, logical_block_size * 4);
	/*
	 * Get registered.
	 */
	major_num = register_blkdev(major_num, "sbd");
	if (major_num < 0) {
		printk(KERN_WARNING "sbd: unable to get major number\n");
		goto out;
	}
	/*
	 * And the gendisk structure.
	 */

	device.gd = alloc_disk(16);
	if (!device.gd)
		goto out_unregister;
	device.gd->major = major_num;
	device.gd->first_minor = 0;
	device.gd->fops = &sbd_ops;
	device.gd->private_data = &device;
	strcpy(device.gd->disk_name, "sbd0");
	set_capacity(device.gd, npages * SECTORS_PER_PAGE);
	device.gd->queue = Queue;
	add_disk(device.gd);

	sysctl_header = register_sysctl_table(dev_root);

	return 0;

out_unregister:
	unregister_blkdev(major_num, "sbd");
out:
	for (i = 0; i < npages; i++)
		kfree(device.data[i]);
	vfree(device.data);
	return -ENOMEM;
}

static void __exit sbd_exit(void)
{
	int i;

	if(get_record && request_log)
	{
		vfree(request_log);
	}


	del_gendisk(device.gd);
	put_disk(device.gd);
	unregister_blkdev(major_num, "sbd");
	blk_cleanup_queue(Queue);

	for (i = 0; i < npages; i++)
		kfree(device.data[i]);

	vfree(device.data);

	unregister_sysctl_table(sysctl_header);

	remove_proc_entry("sbd_log", NULL);
	remove_proc_entry("sbd_cdf", NULL);

	pr_info("sbd: bye!\n");
}

module_init(sbd_init);
module_exit(sbd_exit);
