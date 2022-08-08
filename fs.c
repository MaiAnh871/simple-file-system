#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/module.h>

#include "simplefs.h"

static struct file_system_type simplefs_file_system_type = {
    .owner = THIS_MODULE,
    .name = "simplefs",
    .mount = simplefs_mount,
    .kill_sb = simplefs_kill_sb,
    .fs_flags = FS_REQUIRES_DEV,
    .next = NULL,
};

/**
 * @brief Mount a simplefs partition
 * 
 * @param fs_type describes the filesystem, partly initialized by the specific filesystem code
 * @param flags mount flags
 * @param dev_name the device name we are mounting
 * @param data arbitrary mount options, usually comes as an ASCII string
 * @return struct dentry* A dentry (short for "directory entry") is what the Linux kernel uses to keep track of 
 * the hierarchy of files in directories. Each dentry maps an inode number to a file name and a parent directory.
 */
struct dentry *simplefs_mount(struct file_system_type *fs_type,
                              int flags,
                              const char *dev_name,
                              void *data)
{
    /**
     * @brief Mount a filesystem residing on a block device
     * 
     */
    struct dentry *dentry =
        mount_bdev(fs_type, flags, dev_name, data, simplefs_fill_super);
    if (IS_ERR(dentry))
        pr_err("'%s' mount failure\n", dev_name);
    else
        pr_info("'%s' mount success\n", dev_name);

    return dentry;
}

/* Unmount a simplefs partition */
void simplefs_kill_sb(struct super_block *sb)
{
    kill_block_super(sb);

    pr_info("Unmounted disk\n");
}

static int __init simplefs_init(void)
{
    /* Creates a cache of objects */
    int ret = simplefs_init_inode_cache();
    if (ret) {
        pr_err("Inode cache creation failed\n");
        goto end;
    }

    /* Register a new filesystem */
    ret = register_filesystem(&simplefs_file_system_type);
    if (ret) {
        pr_err("Register_filesystem() failed\n");
        goto end;
    }

    pr_info("Module loaded\n");
end:
    return ret;
}

static void __exit simplefs_exit(void)
{
    /* Remove a file system that was previously successfully registered with the kernel. */
    int ret = unregister_filesystem(&simplefs_file_system_type);
    if (ret)
        pr_err("Unregister_filesystem() failed\n");

    /* Destroys the cache and releases all associated resources. All allocated objects must have been previously freed. */
    simplefs_destroy_inode_cache();

    pr_info("Module unloaded\n");
}

module_init(simplefs_init);
module_exit(simplefs_exit);

MODULE_LICENSE("Dual BSD/GPL");
MODULE_AUTHOR("National Cheng Kung University, Taiwan");
MODULE_DESCRIPTION("a simple file system");
