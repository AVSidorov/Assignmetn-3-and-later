/**
 * @file aesdchar.c
 * @brief Functions and data related to the AESD char driver implementation
 *
 * Based on the implementation of the "scull" device driver, found in
 * Linux Device Drivers example code.
 *
 * @author Dan Walkes
 * @date 2019-10-22
 * @copyright Copyright (c) 2019
 *
 */

#include <linux/module.h>
#include <linux/init.h>

/* #include <linux/kernel.h>*/	/* printk() */
#include <linux/printk.h>
#include <linux/types.h>
#include <linux/cdev.h>
#include <linux/fs.h> // file_operations
#include <linux/uaccess.h>	/* copy_*_user */
#include <linux/slab.h>		/* kmalloc() */
#include "aesdchar.h"

int aesd_major =   0; // use dynamic major
int aesd_minor =   0;

MODULE_AUTHOR("Anton Sidorov");
MODULE_LICENSE("Dual BSD/GPL");

struct aesd_dev aesd_device;

int aesd_open(struct inode *inode, struct file *filp)
{
	struct aesd_dev *dev; /* device information */

	PDEBUG("open");


	dev = container_of(inode->i_cdev, struct aesd_dev, cdev);
	filp->private_data = dev; /* for other methods */

	return 0;
}

int aesd_release(struct inode *inode, struct file *filp)
{
	PDEBUG("release");
	return 0;
}

ssize_t aesd_read(struct file *filp, char __user *buf, size_t count,
                loff_t *f_pos)
{
    ssize_t retval = 0;

    struct aesd_dev *dev = filp->private_data;

    const char *pos;

    aesd_buffer_entry_t *entry = NULL;
    size_t offs_entry; // for getting offset inside entry
    size_t offs_full=*f_pos; //for avoiding "transfer" pointer and calculations

	PDEBUG("read %zu bytes with offset %lld",count,*f_pos);

	if (mutex_lock_interruptible(&dev->circ_buf_lock))
		return -ERESTARTSYS;

	entry = aesd_circular_buffer_find_entry_offset_for_fpos(&dev->circ_buf, offs_full, &offs_entry);
	if (!entry){ //Entry not found
		PDEBUG("entry not found");
		retval=0;
		goto out;
	}

	/* Calculate size to return. Return no more than one command (till end entry buffer)*/
	if ((entry->size - offs_entry) < count){
		PDEBUG("Reduce out count. Till end of entry buffer (end of command)");
		count = entry->size - offs_entry;
	}

	PDEBUG("%zu bytes will be copied to user", count);

	pos = entry->buffptr + offs_entry;

	PDEBUG("%s will be copied to user", pos);

	if (!pos){
		PDEBUG("Buffer is NULL");
		goto out;
	}

	retval = copy_to_user(buf, pos, count);
	if (retval) {
		PDEBUG("fail copy to user");
		retval = -EFAULT;
		goto out;
	}

	*f_pos += count;
	offs_full = *f_pos; // for correct printk
	PDEBUG("New file position %zu ", offs_full);
	PDEBUG("aesd_read returns %zu ", retval);
	retval = count;

	out: mutex_unlock(&dev->circ_buf_lock);
	return retval;
}

ssize_t aesd_write(struct file *filp, const char __user *buf, size_t count,
                loff_t *f_pos)
{

    ssize_t retval = -ENOMEM;

	struct aesd_dev *dev = filp->private_data;

	uint8_t packet = 0;
	char *pos=NULL; // pointer for fill full command(moving inside buffptr)

	qentry_node_t *node = NULL;	//queue node, which points to short (working) entry to store in/get from queue
	aesd_buffer_entry_t *full_cmd = NULL;	//long entry for adding to circular buffer

	char *full_buf = NULL; // buffer to collect full command
	void *del_buf = NULL; // to save pointer to free memory

	size_t buf_size = count*sizeof(char);
	char *buffer = kmalloc(buf_size, GFP_KERNEL);


	PDEBUG("write %zu bytes with offset %lld", buf_size,*f_pos);


	if (!buffer){
		PDEBUG("Error allocate buffer for write data");
		retval = -ENOMEM;
		goto clean_buf;
	}

	memset(buffer, 0, buf_size);

	if (copy_from_user(buffer, buf, buf_size)){
		retval = -EFAULT;
		PDEBUG("Error copy from user buf");
		goto clean_buf;
	}

	 PDEBUG("%s copied from user", buffer);

	// if neccessary trim buf. Then check last symbol in entry buf will work
	if((pos = strchr(buffer,'\n'))){
		PDEBUG("End line found");
		if (((pos-buffer)+1)*sizeof(char) < buf_size){
			memset(pos+1, 0, buf_size - (pos-buffer)*sizeof(char));
			buf_size = (pos-buffer+1)*sizeof(char);
			PDEBUG("Data trimmed");
		}
		pos = NULL;
	}
	retval = buf_size;


	node = kmalloc(sizeof(qentry_node_t), GFP_KERNEL);
	if (!node){
		PDEBUG("Error allocate node");
		retval = -ENOMEM;
		goto clean_buf;
	}
	memset(node, 0, sizeof(qentry_node_t));

	node->entry = kmalloc(sizeof(aesd_buffer_entry_t), GFP_KERNEL);
	if (!node->entry){
		PDEBUG("Allocate entry");
		retval = -ENOMEM;
		goto clean_node;
	}
	memset(node->entry, 0, sizeof(aesd_buffer_entry_t));

	node->entry->buffptr = buffer;
	node->entry->size = buf_size;
	// Store buffer to clear memory in case of fail



	// add full command to circular buffer
	// check can pass. Buffer should be cut at beginning of function
	if (node->entry->buffptr[node->entry->size - 1] == '\n'){
		PDEBUG("Make entry for circular buffer. New line symbol found");
		full_cmd = kmalloc(sizeof(aesd_buffer_entry_t), GFP_KERNEL);
		if (!full_cmd){
			PDEBUG("Error allocation entry for full command");
			retval = -ENOMEM;
			goto clean_entry;
		}
		memset(full_cmd, 0, sizeof(aesd_buffer_entry_t));

		full_buf = kmalloc((dev->queue_size + node->entry->size), GFP_KERNEL);
		if (!full_buf){
			retval = -ENOMEM;
			PDEBUG("Error allocate full command buffer");
			goto clean_full_cmd;
		}
		full_cmd->size = dev->queue_size + node->entry->size;
		memset(full_buf, 0, full_cmd->size);

		packet = 1;
	}



	// in case of full packet last part will be added only if all allocation succeeded
	// caller gets the error and has ability resend (retry) last part and fulfill command
	if (mutex_lock_interruptible(&dev->queue_lock)){
		retval = -ERESTARTSYS;
		PDEBUG("Error lock mutex for queue");
		goto clean_full_buffptr; // don't danger free in case of packet=0 so as
								// full_cmd=full_cmd->buffptr=NULL
	}

	PDEBUG("Add to queue");
	if (STAILQ_EMPTY(&dev->queue)){
		STAILQ_NEXT(node, next) = NULL;
		STAILQ_INSERT_HEAD(&dev->queue,node, next);
	}
	else
		STAILQ_INSERT_TAIL(&dev->queue, node, next);

	// collect full size of packet
	dev->queue_size += node->entry->size;

	mutex_unlock(&dev->queue_lock);



	// here should be all allocation success. Add command to circular buffer
	if (packet){

		if (mutex_lock_interruptible(&dev->queue_lock)){
			PDEBUG("Error lock  mutex for queue. Making full command entry, clean queue.");
			retval = -ERESTARTSYS;
			goto clean_full_buffptr;
		}

		PDEBUG("From queue to circular buffer");
		pos = full_buf;

		while(!STAILQ_EMPTY(&dev->queue)){
			node = STAILQ_FIRST(&dev->queue);
			STAILQ_REMOVE_HEAD(&dev->queue, next);

			if(memcpy(pos, node->entry->buffptr, node->entry->size) == NULL){
				PDEBUG("Error copy to full command buffer");
				retval= -EFAULT;
			}
			pos += node->entry->size;
			kfree(node->entry->buffptr);
			kfree(node->entry);
			kfree(node);
			node = NULL;
		}

		dev->queue_size = 0; //queue_size is part of packet(queue). Access must be locked
		pos = NULL;

		mutex_unlock(&dev->queue_lock);



		if (mutex_lock_interruptible(&dev->circ_buf_lock)){
			PDEBUG("Error lock circular buffer for write full command");
			return -ERESTARTSYS;
			goto clean_full_buffptr;
		}

		PDEBUG("Add to circular buffer %s", full_buf);
		// We need free memory from first command in buffer
		if (dev->circ_buf.full)
			del_buf = (void *)(aesd_circular_buffer_find_entry_offset_for_fpos(&dev->circ_buf, 0, NULL)->buffptr);

		PDEBUG("Save command in buf at %p", full_buf);
		full_cmd->buffptr = full_buf;
		aesd_circular_buffer_add_entry(&dev->circ_buf, full_cmd);

		// data from full_cmd copied to circular buf
		// here free full_cmd
		kfree(full_cmd);
		full_cmd = NULL;

		// here full_cmd already replaced in circular buffer entry, which we stored in del_entry
		if(del_buf){ //here del_cmd NULL or saved entry
		// if buffer not full del_cmd will NULL it's important for first free access to member
			PDEBUG("Free replaced command at %p", del_buf);
			kfree(del_buf);
			//kfree(del_cmd);
			del_buf=NULL;
		}

		mutex_unlock(&dev->circ_buf_lock);


		packet = 0;
	}

	return retval; // All allocated data in use. Return here.

	// Cleanup in case of errors.
	clean_full_buffptr: kfree(full_buf);
	clean_full_cmd: kfree(full_cmd);
	clean_entry: kfree(node->entry);
	clean_node:	kfree(node);
	clean_buf: kfree(buffer);

	return retval;
}

struct file_operations aesd_fops = {
    .owner =    THIS_MODULE,
    .read =     aesd_read,
    .write =    aesd_write,
    .open =     aesd_open,
    .release =  aesd_release,
};

static int aesd_setup_cdev(struct aesd_dev *dev)
{
    int err, devno = MKDEV(aesd_major, aesd_minor);

    cdev_init(&dev->cdev, &aesd_fops);
    dev->cdev.owner = THIS_MODULE;
    dev->cdev.ops = &aesd_fops;
    err = cdev_add (&dev->cdev, devno, 1);
    if (err) {
        printk(KERN_ERR "Error %d adding aesd cdev", err);
    }
    return err;
}



int aesd_init_module(void)
{
    dev_t dev = 0;
    int result;
    result = alloc_chrdev_region(&dev, aesd_minor, 1,
            "aesdchar");
    aesd_major = MAJOR(dev);
    if (result < 0) {
        printk(KERN_WARNING "Can't get major %d\n", aesd_major);
        return result;
    }
    memset(&aesd_device,0,sizeof(struct aesd_dev));

    /**
     * TODO: initialize the AESD specific portion of the device
     */
	aesd_circular_buffer_init(&aesd_device.circ_buf);
	mutex_init(&aesd_device.circ_buf_lock);

	STAILQ_INIT(&aesd_device.queue);
	mutex_init(&aesd_device.queue_lock);
    result = aesd_setup_cdev(&aesd_device);

    if( result ) {
        unregister_chrdev_region(dev, 1);
    }
    return result;

}

void aesd_cleanup_module(void)
{
	dev_t devno = MKDEV(aesd_major, aesd_minor);

	aesd_buffer_entry_t *entry=NULL;
	uint8_t i=0;

	qentry_node_t *node = NULL;

	cdev_del(&aesd_device.cdev);


	PDEBUG("Queue clean");

	while(!STAILQ_EMPTY(&aesd_device.queue)){
		node = STAILQ_FIRST(&aesd_device.queue);
		STAILQ_REMOVE_HEAD(&aesd_device.queue, next);

		kfree(node->entry->buffptr);
		kfree(node->entry);
		kfree(node);
		node = NULL;
	}

	PDEBUG("Bring queue to initial (null) state");
	STAILQ_INIT(&aesd_device.queue);
	aesd_device.queue_size = 0;

	PDEBUG("Clean Circular Buffer");

	AESD_CIRCULAR_BUFFER_FOREACH(entry, &aesd_device.circ_buf,i){
		kfree(entry->buffptr);
		//kfree(entry);
		entry = NULL;
	}
	PDEBUG("Circular buffer clean all pointers to avoid access");
	aesd_circular_buffer_init(&aesd_device.circ_buf);

    unregister_chrdev_region(devno, 1);
}



module_init(aesd_init_module);
module_exit(aesd_cleanup_module);
