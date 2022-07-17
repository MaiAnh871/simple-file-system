/**
 * Notes:
 * Implementing a small filesystem having one file
 *
 * -> What happens when we mount a file system?
 * -> What do we need to provide to the kernel so that we are mountable?
 * -> What inode, dentry and file operations do we have to support?
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/version.h>
#include <linux/fs.h>
#include <linux/sched.h>
#include <linux/statfs.h>
#include <linux/mm.h>
#include <asm/uaccess.h>
#include <asm/errno.h>
#include <linux/buffer_head.h>
#include <linux/pagemap.h> /* unlock_page */

#define RKFS_MAGIC 0xabcd
#define FILE_INODE_NUMBER 2

/* file_system_type */
/* get_sb */
static struct super_block *
rkfs_get_sb(struct file_system_type *, int, const char *, void *);
/* kill_sb */
static void
rkfs_kill_sb(struct super_block *);

/* super_operations */
/* read_inode */
static void
rkfs_super_read_inode(struct inode *inode);
/* write_inode */
static int
rkfs_super_write_inode(struct inode *inode, int sync);

/* inode_operations */
/* lookup */
static struct dentry *
rkfs_inode_lookup(struct inode *parent_inode, struct dentry *dentry, struct nameidata *);

/* file_operations */
static int
rkfs_file_open (struct inode *, struct file *);
/* readdir */
static int
rkfs_file_readdir(struct file *file, void *dirent, filldir_t filldir);
/* release */
static int
rkfs_file_release (struct inode *, struct file *);

/* Address Space Operations */
/* writepage */
static int
rkfs_writepage(struct page *page, struct writeback_control *wbc);
/* readpage */
static int
rkfs_readpage(struct file *file, struct page *page);
/* prepare_write */
static int
rkfs_prepare_write(struct file *file, struct page *page,
		   unsigned from, unsigned to);
/* commit_write */
static int
rkfs_commit_write(struct file *file, struct page *page,
		  unsigned from, unsigned to);


/* readpage */


/*
 * Data declarations
 */

static struct super_operations rkfs_sops = {
  read_inode: rkfs_super_read_inode,
  statfs: simple_statfs, /* handler from libfs */
  write_inode: &rkfs_super_write_inode
};

static struct inode_operations rkfs_iops = {
  lookup: rkfs_inode_lookup
};

static struct file_operations rkfs_fops = {
  open: rkfs_file_open,
  read: &generic_file_read,
  readdir: &rkfs_file_readdir,
  write: &generic_file_write,
  release: &rkfs_file_release,
  fsync: simple_sync_file
};

static struct file_system_type rkfs = {
  name:        "rkfs",
  get_sb:    rkfs_get_sb,
  kill_sb: rkfs_kill_sb,
  owner:        THIS_MODULE
};

static struct address_space_operations rkfs_aops = {
  .readpage = rkfs_readpage,
  .writepage = rkfs_writepage,
  .prepare_write = rkfs_prepare_write,
  .commit_write = rkfs_commit_write
};

static struct inode *rkfs_root_inode;

static char file_buf[PAGE_SIZE] = "Hello World\n";
static int file_size = 12;

/*
 * File-System Operations
 */

static int
rkfs_fill_super(struct super_block *sb, void *data, int silent)
{
  printk("RKFS: rkfs_fill_super\n" );
  sb->s_blocksize = 1024;
  sb->s_blocksize_bits = 10;
  sb->s_magic = RKFS_MAGIC;
  sb->s_op = &rkfs_sops; // super block operations
  sb->s_type = &rkfs; // file_system_type

  rkfs_root_inode = iget(sb, 1); // allocate an inode
  rkfs_root_inode->i_op = &rkfs_iops; // set the inode ops
  rkfs_root_inode->i_mode = S_IFDIR|S_IRWXU;
  rkfs_root_inode->i_fop = &rkfs_fops;

  if(!(sb->s_root = d_alloc_root(rkfs_root_inode))) {
    iput(rkfs_root_inode);
    return -ENOMEM;
  }

  return 0;
}

static struct super_block *
rkfs_get_sb(struct file_system_type *fs_type, int flags, const char *devname, void *data) {
  /* rkfs_fill_super this will be called to fill the superblock */
  return get_sb_single(
		       fs_type,
		       flags,
		       data,
		       &rkfs_fill_super);
}

static void
rkfs_kill_sb(struct super_block *super) {
  kill_anon_super(super);
}

/*
 * Super-Block Operations
 */

static void
rkfs_super_read_inode(struct inode *inode) {
  printk("RKFS: rkfs_super_read_inode\n");
  inode->i_mtime = inode->i_atime = inode->i_ctime = CURRENT_TIME;
  inode->i_mapping->a_ops = &rkfs_aops;
}

static int
rkfs_super_write_inode(struct inode *inode, int wait) {
  printk("RKFS: rkfs_super_write_inode (i_ino = %d) = %d\n",
	 (int)inode->i_ino,
	 (int)i_size_read(inode));
  if(inode->i_ino == FILE_INODE_NUMBER) {
    file_size = i_size_read(inode);
  }
  return 0;
}

/*
 * Inode Operations
 */

static char filename[] = "hello.txt";
static int filename_len = sizeof(filename) - 1;
static struct dentry *
rkfs_inode_lookup(struct inode *parent_inode, struct dentry *dentry, struct nameidata *nameidata) {
  struct inode *file_inode;
  printk("RKFS: rkfs_inode_lookup\n");
  if(parent_inode->i_ino != rkfs_root_inode->i_ino ||
     dentry->d_name.len != filename_len ||
     strncmp(dentry->d_name.name, filename, dentry->d_name.len)) {
    d_add(dentry, NULL);
    goto out;
  }

  file_inode = iget(parent_inode->i_sb, FILE_INODE_NUMBER);
  if(!file_inode)
    return ERR_PTR(-EACCES);
  file_inode->i_size = file_size;
  file_inode->i_mode = S_IFREG|S_IRUSR|S_IWUSR|S_IRGRP|S_IROTH;
  file_inode->i_fop = &rkfs_fops;
  //  file_inode->i_fop
  d_add(dentry, file_inode);

 out:
  return NULL;
}

/*
 * File Operations
 */

static int
rkfs_file_open(struct inode *inode, struct file *file) {
  printk("RKFS: @rkfs_file_open max_readahead = %d (size = %d)\n", (int)file->f_ra.ra_pages, file_size);
  file->f_ra.ra_pages = 0; /* No read-ahead */
  return generic_file_open(inode, file);
}

static int
rkfs_file_release (struct inode *ino, struct file *file) {
  struct dentry *dentry;
  dentry = file->f_dentry;
  return 0;
}

static int
rkfs_file_readdir(struct file *file, void *dirent, filldir_t filldir) {
  struct dentry *de = file->f_dentry;

  if(file->f_pos > 2)
    return 1;
  if(filldir(dirent, ".", 1, file->f_pos++, de->d_inode->i_ino, DT_DIR))
    return 0;
  if(filldir(dirent, "..", 2, file->f_pos++, de->d_parent->d_inode->i_ino, DT_DIR))
    return 0;
  if(filldir(dirent, filename, filename_len, file->f_pos++, FILE_INODE_NUMBER, DT_REG))
    return 0;
  return 1;
}

/* address_space_operations */
static int
rkfs_writepage(struct page *page, struct writeback_control *wbc) {
  void *page_addr = kmap(page);
  printk("[RKFS] offset = %d\n", (int)page->index);
  printk("RKFS: WritePage: [%s] [%s] [%s] [%s]\n",
	 PageUptodate(page) ? "Uptodate" : "Not Uptodate",
	 PageDirty(page) ? "Dirty" : "Not Dirty",
	 PageWriteback(page) ? "PageWriteback Set" : "PageWriteback Cleared",
	 PageLocked(page) ? "Locked" : "Unlocked");
  memcpy(file_buf, page_addr, PAGE_SIZE);
  ClearPageDirty(page);
  if(PageLocked(page))
    unlock_page(page);
  kunmap(page);
  return 0;
}

static int
rkfs_readpage(struct file *file, struct page *page) {
  void *page_addr;
  printk("RKFS: readpage called for page index=[%d]\n", (int)page->index);
  if(page->index > 0) {
    return -ENOSPC;
  }

  printk("RKFS: Page: [%s] [%s] [%s] [%s]\n",
	 PageUptodate(page) ? "Uptodate" : "Not Uptodate",
	 PageDirty(page) ? "Dirty" : "Not Dirty",
	 PageWriteback(page) ? "PageWriteback Set" : "PageWriteback Cleared",
	 PageLocked(page) ? "Locked" : "Unlocked");
  SetPageUptodate(page);
  page_addr = kmap(page);
  if(page_addr)
  	memcpy(page_addr, file_buf, PAGE_SIZE);
  if(PageLocked(page))
    unlock_page(page);
  kunmap(page);
  return 0;
}

static int
rkfs_prepare_write(struct file *file, struct page *page,
		   unsigned from, unsigned to) {
  return 0;
}

/* modified from generic_commit_write. generic_commit_write calls the
 * block device layer to write set up buffer heads for I/O.
 */
static int
rkfs_commit_write(struct file *file, struct page *page,
		  unsigned from, unsigned to) {
  struct inode *inode = page->mapping->host;
  void *page_addr = kmap(page);
  loff_t pos = ((loff_t)page->index << PAGE_CACHE_SHIFT) + to;

  printk("RKFS: commit_write: [%s] [%s] [%s] \n",
	 PageUptodate(page) ? "Uptodate" : "Not Uptodate",
	 PageDirty(page) ? "Dirty" : "Not Dirty",
	 PageLocked(page) ? "Locked" : "Unlocked");

  if(page->index == 0) {
    memcpy(file_buf, page_addr, PAGE_SIZE);
    ClearPageDirty(page);
  }

  SetPageUptodate(page);
  kunmap(page);
   
  /*
   * No need to use i_size_read() here, the i_size
   * cannot change under us because we hold i_sem.
   */
  if (pos > inode->i_size) {
    i_size_write(inode, pos);
    mark_inode_dirty(inode);
  }

   
  return 0;
}


static int
rkfs_init_module(void) {
  int err;

  err = register_filesystem(&rkfs);
  return err;
}

static void
rkfs_cleanup_module(void) {
  unregister_filesystem(&rkfs);
}

module_init(rkfs_init_module);
module_exit(rkfs_cleanup_module);

MODULE_LICENSE("GPL");
