#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <linux/fs.h>
#include <unistd.h>

#include "simplefs.h"

struct superblock {
    struct simplefs_sb_info info;
    char padding[4064]; /* Padding to match block size */
};

/* Returns ceil(a/b) */
static inline uint32_t idiv_ceil(uint32_t a, uint32_t b)
{
    uint32_t ret = a / b;
    if (a % b)
        return ret + 1;
    return ret;
}

/**
 * @brief Initialize Superblock partition, calculate boundary of each partition and metadata information like nr_blocks, nr_inodes etc. 
 * according to the size of the storage device and the size of the various components in the file system.
 * 
 * @param fd A file descriptor is a number that uniquely identifies an open file in a computer's operating system.
 * It describes a data resource, and how that resource may be accessed.
 * @param fstats Struct stat is a system struct that is defined to store information about files. 
 * It is used in several system calls, including fstat, lstat, and stat.
 * @return struct superblock* 
 */
static struct superblock *write_superblock(int fd, struct stat *fstats)
{
    /* Allocates a memory area equal in size to the superblock size. */
    struct superblock *sb = malloc(sizeof(struct superblock));

    /* If fail */
    if (!sb)
        return NULL;

    /* Total number of blocks */
    uint32_t nr_blocks = fstats->st_size / SIMPLEFS_BLOCK_SIZE;

    /* Total number of inodes */
    uint32_t nr_inodes = nr_blocks;

    /* Remainder of the total number of inodes divided by the number of inodes per block */
    uint32_t mod = nr_inodes % SIMPLEFS_INODES_PER_BLOCK;

    /* Rounding the total number of inodes */
    if (mod)
        nr_inodes += SIMPLEFS_INODES_PER_BLOCK - mod;

    /* Number of inode store blocks */    
    uint32_t nr_istore_blocks = idiv_ceil(nr_inodes, SIMPLEFS_INODES_PER_BLOCK);

    /* Number of inode free bitmap blocks
     * Assuming a block size of 1024 bytes, the maximum number of blocks the bitmap can represent: 8 * 1024 = 8192 blocks.  
     */
    uint32_t nr_ifree_blocks = idiv_ceil(nr_inodes, SIMPLEFS_BLOCK_SIZE * 8 ); 

    /* Number of block free bitmap blocks  */
    uint32_t nr_bfree_blocks = idiv_ceil(nr_blocks, SIMPLEFS_BLOCK_SIZE * 8); // Assuming a block size of 1024 bytes, the maximum number of blocks the bitmap can represent: 8 * 1024 = 8192 blocks.
    
    /* The block number for the data block is the number of blocks remaining. */
    uint32_t nr_data_blocks =
        nr_blocks - nr_istore_blocks - nr_ifree_blocks - nr_bfree_blocks;

    /* Set all bit of sb to 0 */
    memset(sb, 0, sizeof(struct superblock));

    sb->info = (struct simplefs_sb_info){
        .magic = htole32(SIMPLEFS_MAGIC), // htole32 convert from host byte order to little-endian order - Little-endian byte ordering places the least significant byte first.
        .nr_blocks = htole32(nr_blocks),
        .nr_inodes = htole32(nr_inodes),
        .nr_istore_blocks = htole32(nr_istore_blocks),
        .nr_ifree_blocks = htole32(nr_ifree_blocks),
        .nr_bfree_blocks = htole32(nr_bfree_blocks),
        .nr_free_inodes = htole32(nr_inodes - 1),
        .nr_free_blocks = htole32(nr_data_blocks - 1),
    };

    /**
     * @brief This function writes length bytes from buffer to the file specified by file descriptor fd. 
     * This binary-only operation is not buffered. 
     * Data is written, starting at the current position of the file pointer associated with the given file.
     * If the file is open for appending, data is written at the end of the file. After the write operation,
     * the file pointer is increased by the number of bytes written.
     * @return The number of bytes written. If an error occurs, -1 is returned and errno is set to EBADF (invalid file handle) or ENOSPC (no space left on device).
     */
    int ret = write(fd, sb, sizeof(struct superblock));

    /* If fail */
    if (ret != sizeof(struct superblock)) {
        free(sb);
        return NULL;
    }

    printf(
        "Superblock: (%ld)\n"
        "\tmagic=%#x\n"
        "\tnr_blocks=%u\n"
        "\tnr_inodes=%u (istore=%u blocks)\n"
        "\tnr_ifree_blocks=%u\n"
        "\tnr_bfree_blocks=%u\n"
        "\tnr_free_inodes=%u\n"
        "\tnr_free_blocks=%u\n",
        sizeof(struct superblock), sb->info.magic, sb->info.nr_blocks,
        sb->info.nr_inodes, sb->info.nr_istore_blocks, sb->info.nr_ifree_blocks,
        sb->info.nr_bfree_blocks, sb->info.nr_free_inodes,
        sb->info.nr_free_blocks);

    return sb;
}

/**
 * @brief Initialize the inode storage partition, set the inode of the root directory and point dir_block to the first of the data blocks, 
 * then initialize the remaining inode blocks to 0 using memset.
 * 
 * @param fd File Descriptor
 * @param sb Superblock
 * @return int 
 */
static int write_inode_store(int fd, struct superblock *sb)
{
    /* Allocate a zeroed block for inode store */
    char *block = malloc(SIMPLEFS_BLOCK_SIZE);

    /* If fail */
    if (!block)
        return -1;

    /* Set all bit to 0 */
    memset(block, 0, SIMPLEFS_BLOCK_SIZE);

    /* Root inode (inode 0) */
    struct simplefs_inode *inode = (struct simplefs_inode *) block;

    /* Order of first data block */
    uint32_t first_data_block = 1 + le32toh(sb->info.nr_bfree_blocks) +
                                le32toh(sb->info.nr_ifree_blocks) +
                                le32toh(sb->info.nr_istore_blocks); // le32toh convert from little-endian order to host byte order

    /* Set i_mode of rooth inode */
    inode->i_mode = htole32(S_IFDIR | S_IRUSR | S_IRGRP | S_IROTH | S_IWUSR |
                            S_IWGRP | S_IXUSR | S_IXGRP | S_IXOTH);

    inode->i_uid = 0;
    inode->i_gid = 0;
    inode->i_size = htole32(SIMPLEFS_BLOCK_SIZE);
    inode->i_ctime = inode->i_atime = inode->i_mtime = htole32(0);
    inode->i_blocks = htole32(1);
    inode->i_nlink = htole32(2); // The first Unix filesystem created two entries in every directory: . pointing to the directory itself, and .. pointing to its parent.
    inode->ei_block = htole32(first_data_block);

    /**
     * @brief This function writes length bytes from buffer to the file specified by file descriptor fd. 
     * This binary-only operation is not buffered. 
     * Data is written, starting at the current position of the file pointer associated with the given file.
     * If the file is open for appending, data is written at the end of the file. After the write operation,
     * the file pointer is increased by the number of bytes written.
     * @return The number of bytes written. If an error occurs, -1 is returned and errno is set to EBADF (invalid file handle) or ENOSPC (no space left on device).
     */
    int ret = write(fd, block, SIMPLEFS_BLOCK_SIZE);
    if (ret != SIMPLEFS_BLOCK_SIZE) {
        ret = -1;
        goto end;
    }

    /* Reset inode store blocks to zero */
    memset(block, 0, SIMPLEFS_BLOCK_SIZE);
    uint32_t i;
    for (i = 1; i < sb->info.nr_istore_blocks; i++) {
        /**
         * @brief This function writes length bytes from buffer to the file specified by file descriptor fd. 
         * This binary-only operation is not buffered. 
         * Data is written, starting at the current position of the file pointer associated with the given file.
         * If the file is open for appending, data is written at the end of the file. After the write operation,
         * the file pointer is increased by the number of bytes written.
         * @return The number of bytes written. If an error occurs, -1 is returned and errno is set to EBADF (invalid file handle) or ENOSPC (no space left on device).
         */
        ret = write(fd, block, SIMPLEFS_BLOCK_SIZE);
        if (ret != SIMPLEFS_BLOCK_SIZE) {
            ret = -1;
            goto end;
        }
    }
    ret = 0;

    printf(
        "Inode store: wrote %d blocks\n"
        "\tinode size = %ld B\n",
        i, sizeof(struct simplefs_inode));

end:
    free(block);
    return ret;
}

/**
 * @brief Initialize the inode bitmap partition is empty, except that the first bit in the entire partition is set to 0 
 * (since the first inode will definitely be reserved for the root directory) and all other bits in the partition are set to first.
 * 
 * @param fd A file descriptor
 * @param sb Superblock
 * @return int 
 */
static int write_ifree_blocks(int fd, struct superblock *sb)
{
    /* Allocate a zeroed block for ifree blocks */
    char *block = malloc(SIMPLEFS_BLOCK_SIZE);

    /* If fail */
    if (!block)
        return -1;

    uint64_t *ifree = (uint64_t *) block;

    /* Set all bits to 1 */
    memset(ifree, 0xff, SIMPLEFS_BLOCK_SIZE);

    /* First ifree block, containing first used inode */
    ifree[0] = htole64(0xfffffffffffffffe);

    /**
     * @brief This function writes length bytes from buffer to the file specified by file descriptor fd. 
     * This binary-only operation is not buffered. 
     * Data is written, starting at the current position of the file pointer associated with the given file.
     * If the file is open for appending, data is written at the end of the file. After the write operation,
     * the file pointer is increased by the number of bytes written.
     * @return The number of bytes written. If an error occurs, -1 is returned and errno is set to EBADF (invalid file handle) or ENOSPC (no space left on device).
     */
    int ret = write(fd, ifree, SIMPLEFS_BLOCK_SIZE);
    if (ret != SIMPLEFS_BLOCK_SIZE) {
        ret = -1;
        goto end;
    }

    /* All ifree blocks except the one containing 2 first inodes */
    ifree[0] = 0xffffffffffffffff;
    uint32_t i;
    for (i = 1; i < le32toh(sb->info.nr_ifree_blocks); i++) {
        ret = write(fd, ifree, SIMPLEFS_BLOCK_SIZE);
        if (ret != SIMPLEFS_BLOCK_SIZE) {
            ret = -1;
            goto end;
        }
    }
    ret = 0;

    printf("Ifree blocks: wrote %d blocks\n", i);

end:
    free(block);

    return ret;
}

/**
 * @brief Initialize an empty block bitmap partition, set the bits in all other partitions to 1 except previously used blocks which will be set to 0.
 * 
 * @param fd A file descriptor
 * @param sb Superblock
 * @return int ret
 */
static int write_bfree_blocks(int fd, struct superblock *sb)
{
    /* Number of used blocks */
    uint32_t nr_used = le32toh(sb->info.nr_istore_blocks) +
                       le32toh(sb->info.nr_ifree_blocks) +
                       le32toh(sb->info.nr_bfree_blocks) + 2;

    /* Allocate a zeroed block for bfree blocks */
    char *block = malloc(SIMPLEFS_BLOCK_SIZE);

    /* If fail */
    if (!block)
        return -1;
    uint64_t *bfree = (uint64_t *) block;

    /*
     * First blocks (incl. sb + istore + ifree + bfree + 1 used block)
     * we suppose it won't go further than the first block
     */
    memset(bfree, 0xff, SIMPLEFS_BLOCK_SIZE);
    uint32_t i = 0;
    while (nr_used) {
        uint64_t line = 0xffffffffffffffff;
        for (uint64_t mask = 0x1; mask; mask <<= 1) {
            line &= ~mask;
            nr_used--;
            if (!nr_used)
                break;
        }
        bfree[i] = htole64(line);
        i++; // i from 0 to nr_bfree_blocks
    }

    /**
     * @brief This function writes length bytes from buffer to the file specified by file descriptor fd. 
     * This binary-only operation is not buffered. 
     * Data is written, starting at the current position of the file pointer associated with the given file.
     * If the file is open for appending, data is written at the end of the file. After the write operation,
     * the file pointer is increased by the number of bytes written.
     * @return The number of bytes written. If an error occurs, -1 is returned and errno is set to EBADF (invalid file handle) or ENOSPC (no space left on device).
     */
    int ret = write(fd, bfree, SIMPLEFS_BLOCK_SIZE);
    if (ret != SIMPLEFS_BLOCK_SIZE) {
        ret = -1;
        goto end;
    }

    /* Other blocks */
    memset(bfree, 0xff, SIMPLEFS_BLOCK_SIZE);
    for (i = 1; i < le32toh(sb->info.nr_bfree_blocks); i++) {
        ret = write(fd, bfree, SIMPLEFS_BLOCK_SIZE);
        if (ret != SIMPLEFS_BLOCK_SIZE) {
            ret = -1;
            goto end;
        }
    }
    ret = 0;

    printf("Bfree blocks: wrote %d blocks\n", i);
end:
    free(block);

    return ret;
}

static int write_data_blocks(int fd, struct superblock *sb)
{
    /* FIXME: unimplemented */
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "Usage: %s disk\n", argv[0]);
        return EXIT_FAILURE;
    }

    /* Open disk image */
    int fd = open(argv[1], O_RDWR);
    if (fd == -1) {
        perror("open():");
        return EXIT_FAILURE;
    }

    /* Get image size */
    struct stat stat_buf;

    /* The fstat() function gets status information about the object specified by the open descriptor descriptor and stores 
     * the information in the area of memory indicated by the buffer argument. The status information is returned in a stat structure 
     */
    int ret = fstat(fd, &stat_buf);
    if (ret) {
        perror("fstat():");
        ret = EXIT_FAILURE;
        goto fclose;
    }

    /* Get block device size */
    if ((stat_buf.st_mode & S_IFMT) == S_IFBLK) {
        long int blk_size = 0;
        /**
         * @brief The ioctl() system call manipulates the underlying device
         * parameters of special files.  In particular, many operating
         * characteristics of character special files (e.g., terminals) may
         * be controlled with ioctl() requests.
         * 
         * @param BLKGETSIZE64 On Linux-based systems the size of a block special device can be obtained using the ioctl request BLKGETSIZE64. 
         * It requires an open file descriptor and produces a 64-bit result which is the size in bytes:
         */
        ret = ioctl(fd, BLKGETSIZE64, &blk_size); 
        if (ret != 0) {
            perror("BLKGETSIZE64:");
            ret = EXIT_FAILURE;
            goto fclose;
        }
        stat_buf.st_size = blk_size;
    }

    /* Check if image is large enough */
    long int min_size = 100 * SIMPLEFS_BLOCK_SIZE;
    if (stat_buf.st_size <= min_size) {
        fprintf(stderr, "File is not large enough (size=%ld, min size=%ld)\n",
                stat_buf.st_size, min_size);
        ret = EXIT_FAILURE;
        goto fclose;
    }

    /* Write superblock (block 0) */
    struct superblock *sb = write_superblock(fd, &stat_buf);
    if (!sb) {
        perror("write_superblock():");
        ret = EXIT_FAILURE;
        goto fclose;
    }

    /* Write inode store blocks (from block 1) */
    ret = write_inode_store(fd, sb);
    if (ret) {
        perror("write_inode_store():");
        ret = EXIT_FAILURE;
        goto free_sb;
    }

    /* Write inode free bitmap blocks */
    ret = write_ifree_blocks(fd, sb);
    if (ret) {
        perror("write_ifree_blocks()");
        ret = EXIT_FAILURE;
        goto free_sb;
    }

    /* Write block free bitmap blocks */
    ret = write_bfree_blocks(fd, sb);
    if (ret) {
        perror("write_bfree_blocks()");
        ret = EXIT_FAILURE;
        goto free_sb;
    }

    /* Write data blocks */
    ret = write_data_blocks(fd, sb);
    if (ret) {
        perror("write_data_blocks():");
        ret = EXIT_FAILURE;
        goto free_sb;
    }

free_sb:
    free(sb);
fclose:
    close(fd);

    return ret;
}
