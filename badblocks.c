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

unsigned char patterns[4][BLK_SIZE];

void init_pattern(void)
{
    memset(patterns[0], 0x00, BLK_SIZE);
    memset(patterns[1], 0xff, BLK_SIZE);
    memset(patterns[2], 0x55, BLK_SIZE);
    memset(patterns[3], 0xaa, BLK_SIZE);
}

// 自动读取块设备 /sys/block/xxx/size
// devname: /dev/block/mmcblk0p1
// out_size: 返回总扇区（512B）
int get_block_device_size(const char *devname, unsigned long long *out_size)
{
    char sys_path[256];
    char *p;
    const char *basename;

    // 提取 mmcblk0p1 从 /dev/block/mmcblk0p1
    basename = strrchr(devname, '/');
    if(basename == NULL){
        basename = devname;
    }else{
        basename++;
    }
    snprintf(sys_path, sizeof(sys_path), "/sys/block/%s/size", basename);

    FILE *fp = fopen(sys_path, "r");
    if(!fp){
        printf("Failed to open %s\n", sys_path);
        return -1;
    }
    if(fscanf(fp, "%llu", out_size) !=1){
        fclose(fp);
        return -1;
    }
    fclose(fp);
    return 0;
}

// 只读扫描
int read_test(int fd, unsigned long long start_blk, unsigned long long end_blk)
{
    unsigned long long blk;
    int bad = 0;
    off64_t offset;
    unsigned long long total = end_blk - start_blk + 1;

    for(blk = start_blk; blk <= end_blk; blk++)
    {
        offset = (off64_t)blk * BLK_SIZE;
        if(lseek64(fd, offset, SEEK_SET) == -1)
        {
            printf("Warning: Seek fail block %llu, errno=%d (out of range)\n", blk, errno);
            continue;
        }
        ssize_t ret = read(fd, rbuf, BLK_SIZE);
        if(ret != BLK_SIZE)
        {
            printf("READ BAD BLOCK: %llu, ret=%zd errno=%d\n", blk, ret, errno);
            bad++;
        }
        // 每5000个扇区打印一次进度
        if ((blk - start_blk) % 5000 == 0)
        {
            printf("[Progress] Scanned %llu / %llu, found bad: %d\n", blk-start_blk, total, bad);
            fflush(stdout);
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
    unsigned long long total = end_blk - start_blk +1;

    for(pat_idx = 0; pat_idx < total_pat; pat_idx++)
    {
        printf("=== Test pattern %d/4 ===\n", pat_idx+1);
        for(blk = start_blk; blk <= end_blk; blk++)
        {
            offset = (off64_t)blk * BLK_SIZE;
            if(lseek64(fd, offset, SEEK_SET) == -1)
            {
                printf("Warning: Seek fail block %llu, errno=%d (out of range)\n", blk, errno);
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
            if ((blk - start_blk) % 5000 == 0)
            {
                printf("[Pattern%d Progress] Scanned %llu / %llu, bad: %d\n",pat_idx+1, blk-start_blk, total, bad);
                fflush(stdout);
            }
        }
    }
    return bad;
}

int main(int argc, char** argv)
{
    if(argc !=3)
    {
        printf("Usage: %s dev_path start_block mode\n", argv[0]);
        printf("mode: 0=read-only safe scan, 1=destructive write test(DATA LOSS!)\n");
        printf("Example: %s /dev/block/mmcblk0p1 0 0\n", argv[0]);
        printf("Auto read partition size, no need input end_block\n");
        return 1;
    }
    const char *dev = argv[1];
    unsigned long long start = atoll(argv[2]);
    int mode = atoi(argv[3]);
    unsigned long long dev_sectors;

    // 自动读取分区总扇区
    if(get_block_device_size(dev, &dev_sectors) !=0)
    {
        printf("Get block size failed!\n");
        return -1;
    }
    unsigned long long end = dev_sectors -1;
    printf("Auto detect partition total sectors: %llu (512B/sector)\n", dev_sectors);
    printf("Scan range: start=%llu , auto end=%llu\n", start, end);

    if(start > end)
    {
        printf("Error: start_block > end_block\n");
        return -1;
    }

    int fd = open(dev, O_RDONLY);
    if(mode == 1)
    {
        fd = open(dev, O_RDWR);
    }
    if(fd < 0)
    {
        printf("Open %s failed, errno=%d, need root!\n", dev, errno);
        return -1;
    }
    init_pattern();
    int bad_blocks =0;
    printf("badblocks start. dev:%s, mode=%s\n",
           dev, (mode==1)?"Write-Destructive":"ReadOnly");

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
        printf("✅ NO BAD BLOCK found.\n");
    }else{
        printf("❌ TOTAL BAD BLOCKS: %d\n", bad_blocks);
    }
    return 0;
}
