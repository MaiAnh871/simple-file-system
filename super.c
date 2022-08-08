#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/buffer_head.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/statfs.h>

#include "simplefs.h"

/**
 * @brief Lookaside cache solves the memory fragmentation problem. 
 * Lookaside cache is a memory pool that only holds objects of a certain type of structure.
 * The example has a lookaside cache that contains only instances of the task_struct structure (describes the process).
 * Or have a lookaside cache that only contains instances of the inode structure.
 * When the driver requests to allocate one or more variables of this type, the kernel simply takes some objects in
 * lookaside cache out, then give it to the driver. When the driver is no longer needed, the driver returns the kernel. Kernel will
 * mark it as unused (or available) so that it can be allocated to the following requests.
 * Linux uses the kmem_cache structure to represent a lookaside cache.
 */
static struct kmem_cache *simplefs_inode_cache;

int simplefs_init_inode_cache(void)
{
    /**
     * @brief Creates a cache of objects
     * 
     * @param 1 name - A string which is used in /proc/slabinfo to identify this cache.
     * @param 2 size - The size of objects to be created in this cache.
     * @param 3 offset - The offset to use within the page.
     * @param 4 flags - SLAB flags
     * @param 5 ctor - A constructor for the objects.
     */
    simplefs_inode_cache = kmem_cache_create(
        "simplefs_cache", sizeof(struct simplefs_inode_info), 0, 0, NULL);
    if (!simplefs_inode_cache)
        return -ENOMEM;
    return 0;
}

void simplefs_destroy_inode_cache(void)
{
    /* Destroys the cache and releases all associated resources. All allocated objects must have been previously freed. */
    kmem_cache_destroy(simplefs_inode_cache);
}

/* Allocate the space of simplefs_inode_info in the cache, after creating a simplefs inode, return vfs_inode to VFS. */
static struct inode *simplefs_alloc_inode(struct super_block *sb)
{
    /**
     * @brief Allocate an object from this cache.
     * 
     * @param 1 cp - Pointer to the object cache.
     * @param 2 kmflag - Propagated kmflag values. GFP_KERNEL means that allocation is performed on behalf of 
     * a process running in the kernel space. This means that the calling function is executing a system call on
     * behalf of a process.Using GFP_KERNEL means that kmalloc can put the current process to sleep waiting 
     * for a page when called in low-memory situations.
     */
    struct simplefs_inode_info *ci =
        kmem_cache_alloc(simplefs_inode_cache, GFP_KERNEL);
    if (!ci)
        return NULL;

    /*
     * These are initializations that only need to be done
     * once, because the fields are idempotent across use
     * of the inode, so let the slab aware of that.
     */
    inode_init_once(&ci->vfs_inode);
    return &ci->vfs_inode; // Return vfs_inode to VFS.
}

static void simplefs_destroy_inode(struct inode *inode)
{
    struct simplefs_inode_info *ci = SIMPLEFS_INODE(inode);

    /* Free an object - ci which was previously allocated from this cache - simplefs_inode_cache. */
    kmem_cache_free(simplefs_inode_cache, ci);
}

/**
 * @brief Extract the information of the VFS inode in the cache and write the information in the struct simplefs_inode_info back to the storage device.
 * 
 * @param inode 
 * @param wbc A control structure which tells the writeback code what to do.  These are always on the stack, and hence 
 * need no locking.  They are always initialised in a manner such that unspecified fields are set to zero.
 * The writeback code paths which walk the superblocks and inodes are getting an increasing arguments passed to them.
 * The patch wraps those args into the new `struct writeback_control', and uses that instead.  
 * There is no functional change. The new writeback_control structure is passed down through the
 * writeback paths in the place where the old `nr_to_write' pointer used to be.
 * 
 * @return int 
 */
static int simplefs_write_inode(struct inode *inode,
                                struct writeback_control *wbc)
{
    /* Inode on disk */
    struct simplefs_inode *disk_inode;

    /* Inode in VFS */
    struct simplefs_inode_info *ci = SIMPLEFS_INODE(inode);

    /* Superblock on disk */
    struct super_block *sb = inode->i_sb;

    /* Block in VFS*/
    struct simplefs_sb_info *sbi = SIMPLEFS_SB(sb);

    /**
     * @brief Historically, a buffer_head was used to map a single block within a page, and of course as the unit of I/O through the
     * filesystem and block layers.  Nowadays the basic I/O unit is the bio, and buffer_heads are used for extracting block
     * mappings (via a get_block_t call), for tracking state within a page (via a page_mapping) and for wrapping bio submission
     * for backward compatibility reasons (e.g. submit_bh).
     * 
     */
    struct buffer_head *bh;

    /* Inode number */
    uint32_t ino = inode->i_ino;

    /* Number of blocks */
    uint32_t inode_block = (ino / SIMPLEFS_INODES_PER_BLOCK) + 1;

    /* Remainder */
    uint32_t inode_shift = ino % SIMPLEFS_INODES_PER_BLOCK;

    if (ino >= sbi->nr_inodes)
        return 0;

    /* sb_bread reads the corresponding block - inode_block from the device specified in sb and stores it in a buffer */
    bh = sb_bread(sb, inode_block); // Reads last inode_block?
    if (!bh)
        return -EIO;

    /* Data read from sb_bread/ Read from inode_block */
    disk_inode = (struct simplefs_inode *) bh->b_data;
    disk_inode += inode_shift;

    /* Update the mode using what the generic inode has */
    disk_inode->i_mode = inode->i_mode;
    disk_inode->i_uid = i_uid_read(inode);
    disk_inode->i_gid = i_gid_read(inode);
    disk_inode->i_size = inode->i_size;
    disk_inode->i_ctime = inode->i_ctime.tv_sec;
    disk_inode->i_atime = inode->i_atime.tv_sec;
    disk_inode->i_mtime = inode->i_mtime.tv_sec;
    disk_inode->i_blocks = inode->i_blocks;
    disk_inode->i_nlink = inode->i_nlink;
    disk_inode->ei_block = ci->ei_block;

    /* 
     * The function char *strncpy(char *dest, const char *src, size_t n) copies up to n copies up to n characters 
     * from the string pointed to by src to dest. In case the length of src is less than n, the remainder or remainder 
     * of dest will be filled with null values. 
     */
    strncpy(disk_inode->i_data, ci->i_data, sizeof(ci->i_data));

    /* 
     * Mark a buffer_head as needing writeout. It will set the dirty bit against the buffer, then set its backing page
     * dirty, then tag the page as dirty in its address_space's radix tree and then attach the address_space's inode to 
     * its superblock's dirty inode list. 
     */
    mark_buffer_dirty(bh);

    /*
     * For a data-integrity writeout, we need to wait upon any in-progress I/O
     * and then start new I/O and then wait upon it.  The caller must have a ref on
     * the buffer_head.
     */
    sync_dirty_buffer(bh);

    /* Frees the specified buffer. */
    brelse(bh);

    return 0;
}

static void simplefs_put_super(struct super_block *sb)
{
    struct simplefs_sb_info *sbi = SIMPLEFS_SB(sb);
    if (sbi) {
        kfree(sbi->ifree_bitmap);
        kfree(sbi->bfree_bitmap);
        kfree(sbi);
    }
}

static int simplefs_sync_fs(struct super_block *sb, int wait)
{
    struct simplefs_sb_info *sbi = SIMPLEFS_SB(sb);
    struct simplefs_sb_info *disk_sb;
    int i;

    /* Flush superblock */
    struct buffer_head *bh = sb_bread(sb, 0);
    if (!bh)
        return -EIO;

    disk_sb = (struct simplefs_sb_info *) bh->b_data;

    disk_sb->nr_blocks = sbi->nr_blocks;
    disk_sb->nr_inodes = sbi->nr_inodes;
    disk_sb->nr_istore_blocks = sbi->nr_istore_blocks;
    disk_sb->nr_ifree_blocks = sbi->nr_ifree_blocks;
    disk_sb->nr_bfree_blocks = sbi->nr_bfree_blocks;
    disk_sb->nr_free_inodes = sbi->nr_free_inodes;
    disk_sb->nr_free_blocks = sbi->nr_free_blocks;

    mark_buffer_dirty(bh);
    if (wait)
        sync_dirty_buffer(bh);
    brelse(bh);

    /* Flush free inodes bitmask */
    for (i = 0; i < sbi->nr_ifree_blocks; i++) {
        int idx = sbi->nr_istore_blocks + i + 1;

        bh = sb_bread(sb, idx);
        if (!bh)
            return -EIO;

        memcpy(bh->b_data, (void *) sbi->ifree_bitmap + i * SIMPLEFS_BLOCK_SIZE,
               SIMPLEFS_BLOCK_SIZE);

        mark_buffer_dirty(bh);
        if (wait)
            sync_dirty_buffer(bh);
        brelse(bh);
    }

    /* Flush free blocks bitmask */
    for (i = 0; i < sbi->nr_bfree_blocks; i++) {
        int idx = sbi->nr_istore_blocks + sbi->nr_ifree_blocks + i + 1;

        bh = sb_bread(sb, idx);
        if (!bh)
            return -EIO;

        memcpy(bh->b_data, (void *) sbi->bfree_bitmap + i * SIMPLEFS_BLOCK_SIZE,
               SIMPLEFS_BLOCK_SIZE);

        mark_buffer_dirty(bh);
        if (wait)
            sync_dirty_buffer(bh);
        brelse(bh);
    }

    return 0;
}

static int simplefs_statfs(struct dentry *dentry, struct kstatfs *stat)
{
    struct super_block *sb = dentry->d_sb;
    struct simplefs_sb_info *sbi = SIMPLEFS_SB(sb);

    stat->f_type = SIMPLEFS_MAGIC;
    stat->f_bsize = SIMPLEFS_BLOCK_SIZE;
    stat->f_blocks = sbi->nr_blocks;
    stat->f_bfree = sbi->nr_free_blocks;
    stat->f_bavail = sbi->nr_free_blocks;
    stat->f_files = sbi->nr_inodes - sbi->nr_free_inodes;
    stat->f_ffree = sbi->nr_free_inodes;
    stat->f_namelen = SIMPLEFS_FILENAME_LEN;

    return 0;
}

static struct super_operations simplefs_super_ops = {
    .put_super = simplefs_put_super,
    .alloc_inode = simplefs_alloc_inode,
    .destroy_inode = simplefs_destroy_inode,
    .write_inode = simplefs_write_inode,
    .sync_fs = simplefs_sync_fs,
    .statfs = simplefs_statfs,
};

/* Fill the struct superblock from partition superblock */
int simplefs_fill_super(struct super_block *sb, void *data, int silent)
{
    struct buffer_head *bh = NULL;
    struct simplefs_sb_info *csb = NULL;
    struct simplefs_sb_info *sbi = NULL;
    struct inode *root_inode = NULL;
    int ret = 0, i;

    /* Init sb */
    sb->s_magic = SIMPLEFS_MAGIC;
    sb_set_blocksize(sb, SIMPLEFS_BLOCK_SIZE);
    sb->s_maxbytes = SIMPLEFS_MAX_FILESIZE;
    sb->s_op = &simplefs_super_ops;

    /* Read sb from disk */
    bh = sb_bread(sb, SIMPLEFS_SB_BLOCK_NR);
    if (!bh)
        return -EIO;

    csb = (struct simplefs_sb_info *) bh->b_data;

    /* Check magic number */
    if (csb->magic != sb->s_magic) {
        pr_err("Wrong magic number\n");
        ret = -EINVAL;
        goto release;
    }

    /* Alloc sb_info */
    sbi = kzalloc(sizeof(struct simplefs_sb_info), GFP_KERNEL);
    if (!sbi) {
        ret = -ENOMEM;
        goto release;
    }

    sbi->nr_blocks = csb->nr_blocks;
    sbi->nr_inodes = csb->nr_inodes;
    sbi->nr_istore_blocks = csb->nr_istore_blocks;
    sbi->nr_ifree_blocks = csb->nr_ifree_blocks;
    sbi->nr_bfree_blocks = csb->nr_bfree_blocks;
    sbi->nr_free_inodes = csb->nr_free_inodes;
    sbi->nr_free_blocks = csb->nr_free_blocks;
    sb->s_fs_info = sbi;

    brelse(bh);

    /* Alloc and copy ifree_bitmap */
    sbi->ifree_bitmap =
        kzalloc(sbi->nr_ifree_blocks * SIMPLEFS_BLOCK_SIZE, GFP_KERNEL);
    if (!sbi->ifree_bitmap) {
        ret = -ENOMEM;
        goto free_sbi;
    }

    for (i = 0; i < sbi->nr_ifree_blocks; i++) {
        int idx = sbi->nr_istore_blocks + i + 1;

        bh = sb_bread(sb, idx);
        if (!bh) {
            ret = -EIO;
            goto free_ifree;
        }

        memcpy((void *) sbi->ifree_bitmap + i * SIMPLEFS_BLOCK_SIZE, bh->b_data,
               SIMPLEFS_BLOCK_SIZE);

        brelse(bh);
    }

    /* Alloc and copy bfree_bitmap */
    sbi->bfree_bitmap =
        kzalloc(sbi->nr_bfree_blocks * SIMPLEFS_BLOCK_SIZE, GFP_KERNEL);
    if (!sbi->bfree_bitmap) {
        ret = -ENOMEM;
        goto free_ifree;
    }

    for (i = 0; i < sbi->nr_bfree_blocks; i++) {
        int idx = sbi->nr_istore_blocks + sbi->nr_ifree_blocks + i + 1;

        bh = sb_bread(sb, idx);
        if (!bh) {
            ret = -EIO;
            goto free_bfree;
        }

        memcpy((void *) sbi->bfree_bitmap + i * SIMPLEFS_BLOCK_SIZE, bh->b_data,
               SIMPLEFS_BLOCK_SIZE);

        brelse(bh);
    }

    /* Create root inode */
    root_inode = simplefs_iget(sb, 0);
    if (IS_ERR(root_inode)) {
        ret = PTR_ERR(root_inode);
        goto free_bfree;
    }
#if USER_NS_REQUIRED()
    inode_init_owner(&init_user_ns, root_inode, NULL, root_inode->i_mode);
#else
    inode_init_owner(root_inode, NULL, root_inode->i_mode);
#endif
    
    sb->s_root = d_make_root(root_inode);
    if (!sb->s_root) {
        ret = -ENOMEM;
        goto iput;
    }

    return 0;

iput:
    iput(root_inode);
free_bfree:
    kfree(sbi->bfree_bitmap);
free_ifree:
    kfree(sbi->ifree_bitmap);
free_sbi:
    kfree(sbi);
release:
    brelse(bh);

    return ret;
}
