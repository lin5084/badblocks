#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <string.h>

#define BLK_SIZE 512
static unsigned char wbuf[BLK_SIZE];
static unsigned char rbuf[BLK_SIZE];

// 测试pattern，和原版badblocks一致
unsigned char patterns[4][BLK_SIZE];

void init_pattern(void)
{
    memset(patterns[0], 0x00, BLK_SIZE);
    memset(patterns[1], 0xff, BLK_SIZE);
    memset(patterns[2], 0x55, BLK_SIZE);
    memset(patterns[3], 0xaa, BLK_SIZE);
}

// 只读扫描
int read_test(int fd, unsigned long long start_blk, unsigned long long end_blk)
{
    unsigned long long blk;
    int bad = 0;
    off64_t offset;
    for(blk = start_blk; blk <= end_blk; blk++)
    {
        offset = (off64_t)blk * BLK_SIZE;
        if(lseek64(fd, offset, SEEK_SET) == -1)
        {
            printf("Seek fail block %llu, errno=%d\n", blk, errno);
            bad++;
            continue;
        }
        ssize_t ret = read(fd, rbuf, BLK_SIZE);
        if(ret != BLK_SIZE)
        {
            printf("READ BAD BLOCK: %llu, ret=%zd errno=%d\n", blk, ret, errno);
            bad++;
        }
    }
    return bad;
}

// 破坏性读写测试（-w模式，会清空数据！慎用）
int write_test(int fd, unsigned long long start_blk, unsigned long long end_blk)
{
    unsigned long long blk;
    unsigned int pat_idx;
    int bad = 0;
    off64_t offset;
    int total_pat = 4;

    for(pat_idx = 0; pat_idx < total_pat; pat_idx++)
    {
        printf("=== Test pattern %d/4 ===\n", pat_idx+1);
        for(blk = start_blk; blk <= end_blk; blk++)
        {
            offset = (off64_t)blk * BLK_SIZE;
            if(lseek64(fd, offset, SEEK_SET) == -1)
            {
                printf("Seek fail block %llu, errno=%d\n", blk, errno);
                bad++;
                continue;
            }
            // write
            if(write(fd, patterns[pat_idx], BLK_SIZE) != BLK_SIZE)
            {
                printf("WRITE BAD BLOCK: %llu\n", blk);
                bad++;
                continue;
            }
            fsync(fd);
            // read back
            lseek64(fd, offset, SEEK_SET);
            if(read(fd, rbuf, BLK_SIZE) != BLK_SIZE)
            {
                printf("READBACK BAD BLOCK: %llu\n", blk);
                bad++;
                continue;
            }
            if(memcmp(rbuf, patterns[pat_idx], BLK_SIZE) != 0)
            {
                printf("DATA MISMATCH BAD BLOCK: %llu\n", blk);
                bad++;
            }
        }
    }
    return bad;
}

int main(int argc, char** argv)
{
    if(argc !=5)
    {
        printf("Usage: %s dev_path start_block end_block mode\n", argv[0]);
        printf("mode: 0=read-only, 1=destructive write test (DATA LOSS!)\n");
        printf("Example: %s /dev/block/mmcblk0p1 0 20000 0\n", argv[0]);
        printf("WARNING: mode=1 will overwrite your partition data!!!\n");
        return 1;
    }
    const char *dev = argv[1];
    unsigned long long start = atoll(argv[2]);
    unsigned long long end   = atoll(argv[3]);
    int mode = atoi(argv[4]);

    int fd = open(dev, O_RDONLY);
    if(mode == 1)
    {
        fd = open(dev, O_RDWR);
    }
    if(fd < 0)
    {
        printf("Open %s failed, errno=%d, need root\n", dev, errno);
        return -1;
    }
    init_pattern();
    int bad_blocks =0;
    printf("badblocks start. dev:%s, blk range: %llu ~ %llu, mode=%s\n",
           dev, start, end, (mode==1)?"Write-Destructive":"ReadOnly");

    if(mode == 0)
    {
        bad_blocks = read_test(fd, start, end);
    }else{
        bad_blocks = write_test(fd, start, end);
    }
    close(fd);
    printf("\n==== SCAN FINISHED ====\n");
    if(bad_blocks ==0)
    {
        printf("NO BAD BLOCK found.\n");
    }else{
        printf("TOTAL BAD BLOCKS: %d\n", bad_blocks);
    }
    return 0;
}
