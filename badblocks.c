#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <string.h>
#include <linux/fs.h>

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

// 简易提取mmcblkX 从 mmcblkXpY，不使用regex
static int get_parent_dev(const char *part_name, char *parent_out)
{
    if (strstr(part_name, "mmcblk") != part_name)
        return -1;
    size_t i = 6;
    while (part_name[i] != '\0' && part_name[i] != 'p')
    {
        parent_out[i] = part_name[i];
        i++;
    }
    parent_out[i] = '\0';
    if (i>0)
        return 0;
    return -1;
}

// 方式1：sysfs读取分区size（Android专用路径）
static int get_size_via_sysfs(const char *devname, unsigned long long *out_sectors)
{
    char sys_path[256];
    char part_name[128];
    char parent_dev[128];
    const char *basename;

    basename = strrchr(devname, '/');
    if(basename == NULL){
        basename = devname;
    }else{
        basename++;
    }
    strncpy(part_name, basename, sizeof(part_name)-1);
    part_name[sizeof(part_name)-1] = 0;

    if(get_parent_dev(part_name, parent_dev) ==0)
    {
        snprintf(sys_path, sizeof(sys_path), "/sys/block/%s/%s/size", parent_dev, part_name);
    }else{
        snprintf(sys_path, sizeof(sys_path), "/sys/block/%s/size", part_name);
    }
    printf("[sysfs try] %s\n", sys_path);
    FILE *fp = fopen(sys_path, "r");
    if(!fp){
        return -1;
    }
    if(fscanf(fp, "%llu", out_sectors) !=1){
        fclose(fp);
        return -1;
    }
    fclose(fp);
    return 0;
}

// 方式2：IOCTL BLKGETSIZE64 兜底获取字节总容量，转成512扇区
static int get_size_via_ioctl(int fd, unsigned long long *out_sectors)
{
    unsigned long long bytes = 0;
    if (ioctl(fd, BLKGETSIZE64, &bytes) < 0)
    {
        printf("[ioctl] BLKGETSIZE64 failed, errno=%d\n", errno);
        return -1;
    }
    *out_sectors = bytes / BLK_SIZE;
    return 0;
}

// 统一入口：优先sysfs，失败自动切ioctl
int get_block_device_size(const char *devname, int fd, unsigned long long *out_sectors)
{
    if(get_size_via_sysfs(devname, out_sectors) ==0)
    {
        printf("[OK] Read size from sysfs\n");
        return 0;
    }
    printf("[warn] sysfs read failed, fallback to ioctl\n");
    return get_size_via_ioctl(fd, out_sectors);
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
    if(argc !=4)
    {
        printf("Usage: %s dev_path start_block mode\n", argv[0]);
        printf("mode: 0=read-only safe scan, 1=destructive write test(DATA LOSS!)\n");
        printf("Example: %s /dev/block/mmcblk0p1 0 0\n", argv[0]);
        printf("Auto read partition size(sysfs + ioctl fallback)\n");
        return 1;
    }
    const char *dev = argv[1];
    unsigned long long start = atoll(argv[2]);
    int mode = atoi(argv[3]);
    unsigned long long dev_sectors;

    int fd = open(dev, O_RDONLY);
    if(fd < 0)
    {
        printf("Open %s failed, errno=%d, need root!\n", dev, errno);
        return -1;
    }

    // 传入fd，sysfs失败自动用ioctl
    if(get_block_device_size(dev, fd, &dev_sectors) !=0)
    {
        printf("Get block size failed! sysfs and ioctl both failed.\n");
        close(fd);
        return -1;
    }
    unsigned long long end = dev_sectors -1;
    printf("Auto detect partition total sectors: %llu (512B/sector)\n", dev_sectors);
    printf("Scan range: start=%llu , auto end=%llu\n", start, end);

    if(start > end)
    {
        printf("Error: start_block > end_block\n");
        close(fd);
        return -1;
    }

    // 如果是写模式，重新打开读写句柄
    if(mode == 1)
    {
        close(fd);
        fd = open(dev, O_RDWR);
        if(fd <0){
            printf("Open O_RDWR failed, cannot do write test\n");
            return -1;
        }
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
