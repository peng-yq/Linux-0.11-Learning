/*
 *  linux/fs/inode.c
 *
 *  (C) 1991  Linus Torvalds
 */

#include <string.h>
#include <sys/stat.h>

#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <asm/system.h>

struct m_inode inode_table[NR_INODE]={{0,},};

static void read_inode(struct m_inode * inode);
static void write_inode(struct m_inode * inode);

// 等待指定的 i 节点可用
// 如果 i 节点已被锁定，则将当前任务置为不可中断的等待状态。直到该 i 节点解锁
static inline void wait_on_inode(struct m_inode * inode)
{
	cli();
	while (inode->i_lock)
		sleep_on(&inode->i_wait);
	sti();
}

static inline void lock_inode(struct m_inode * inode)
{
	cli();
	while (inode->i_lock)
		sleep_on(&inode->i_wait);
	// 锁定节点
	inode->i_lock=1;
	sti();
}

// 直接解锁节点，然后唤醒等待该节点的进程
static inline void unlock_inode(struct m_inode * inode)
{
	inode->i_lock=0;
	wake_up(&inode->i_wait);
}

// 释放内存中设备 dev 的所有 i 节点。
// 扫描内存中的 i 节点表数组，如果是指定设备使用的 i 节点就释放之
void invalidate_inodes(int dev)
{
	int i;
	struct m_inode * inode;

	inode = 0+inode_table;					// 指针指向inode_table数组首项
	for(i=0 ; i<NR_INODE ; i++,inode++) {
		wait_on_inode(inode);				// 等待节点可用，解锁
		// 匹配指定设备
		if (inode->i_dev == dev) {
			// 节点引用次数不为0则显示出错
			if (inode->i_count)
				printk("inode in use on removed disk\n\r");
			// 释放节点
			inode->i_dev = inode->i_dirt = 0;
		}
	}
}

// 同步所有 i 节点
// 同步内存与设备上的所有 i 节点信息
void sync_inodes(void)
{
	int i;
	struct m_inode * inode;

	inode = 0+inode_table;
	for(i=0 ; i<NR_INODE ; i++,inode++) {
		wait_on_inode(inode);
		// 如果节点已修改且不是管道节点则写入
		if (inode->i_dirt && !inode->i_pipe)
			write_inode(inode);
	}
}

/*
 *文件数据块映射到盘块的处理操作。(block 位图处理函数，bmap - block map)
 *参数：inode – 文件的 i 节点；block – 文件中的数据块号；create - 创建标志
 *如果创建标志置位，则在对应逻辑块不存在时就申请新磁盘块
 *返回 block 数据块对应在设备上的逻辑块号（盘块号）
 */
static int _bmap(struct m_inode * inode,int block,int create)
{
	struct buffer_head * bh;
	int i;

	if (block<0)
		panic("_bmap: block<0");
	// 如果块号大于直接块数 + 间接块数 + 二次间接块数，超出文件系统表示范围，则死机
	// 7是i_zone[9]的前7项
	if (block >= 7+512+512*512)
		panic("_bmap: block>big");
	// 直接块
	if (block<7) {
		// 如果创建标志置位，并且 i 节点中对应该块的逻辑块（区段）字段为 0，则向相应设备申请一磁盘
		// 块（逻辑块，区块），并将盘上逻辑块号（盘块号）填入逻辑块字段中。然后设置 i 节点修改时间，
		// 置 i 节点已修改标志。最后返回逻辑块号。
		if (create && !inode->i_zone[block])
			if (inode->i_zone[block]=new_block(inode->i_dev)) {
				inode->i_ctime=CURRENT_TIME;
				inode->i_dirt=1;
			}
		return inode->i_zone[block];
	}
	// 如果该块号>=7，并且小于 7+512，则说明是一次间接块。下面对一次间接块进行处理
	block -= 7;
	if (block<512) {
		if (create && !inode->i_zone[7])
			if (inode->i_zone[7]=new_block(inode->i_dev)) {
				inode->i_dirt=1;
				inode->i_ctime=CURRENT_TIME;
			}
		// 若此时 i 节点间接块字段中为 0，表明申请磁盘块失败
		if (!inode->i_zone[7])
			return 0;
		// 读取设备上的一次间接块
		if (!(bh = bread(inode->i_dev,inode->i_zone[7])))
			return 0;
		// 取该间接块上第 block 项中的逻辑块号（盘块号）
		i = ((unsigned short *) (bh->b_data))[block];
		if (create && !i)
			if (i=new_block(inode->i_dev)) {
				((unsigned short *) (bh->b_data))[block]=i;
				bh->b_dirt=1;
			}
		// 最后释放该间接块，返回磁盘上新申请的对应 block 的逻辑块的块号
		brelse(bh);
		return i;
	}
	// 二次间接块
	block -= 512;
	if (create && !inode->i_zone[8])
		if (inode->i_zone[8]=new_block(inode->i_dev)) {
			inode->i_dirt=1;
			inode->i_ctime=CURRENT_TIME;
		}
	if (!inode->i_zone[8])
		return 0;
	if (!(bh=bread(inode->i_dev,inode->i_zone[8])))
		return 0;
	i = ((unsigned short *)bh->b_data)[block>>9];
	if (create && !i)
		if (i=new_block(inode->i_dev)) {
			((unsigned short *) (bh->b_data))[block>>9]=i;
			bh->b_dirt=1;
		}
	brelse(bh);
	if (!i)
		return 0;
	if (!(bh=bread(inode->i_dev,i)))
		return 0;
	i = ((unsigned short *)bh->b_data)[block&511];
	if (create && !i)
		if (i=new_block(inode->i_dev)) {
			((unsigned short *) (bh->b_data))[block&511]=i;
			bh->b_dirt=1;
		}
	brelse(bh);
	return i;
}

int bmap(struct m_inode * inode,int block)
{
	return _bmap(inode,block,0);
}

int create_block(struct m_inode * inode, int block)
{
	return _bmap(inode,block,1);
}

// 释放一个 i 节点(回写入设备)	
void iput(struct m_inode * inode)
{
	if (!inode)
		return;
	// 等待i节点解锁
	wait_on_inode(inode);
	if (!inode->i_count)
		panic("iput: trying to free free inode");
	// 如果是管道 i 节点，则唤醒等待该管道的进程，引用次数减 1，如果还有引用则返回
	// 否则释放管道占用的内存页面，并复位该节点的引用计数值、已修改标志和管道标志，并返回
	// 管道节点比较特殊，是内存，也是文件，所以需要释放page(这里的i_size存放物理内存地址)，也要修改inode信息
	if (inode->i_pipe) {
		wake_up(&inode->i_wait);
		if (--inode->i_count)
			return;
		free_page(inode->i_size);
		inode->i_count=0;
		inode->i_dirt=0;
		inode->i_pipe=0;
		return;
	}
	// 如果 i 节点对应的设备号=0，则将此节点的引用计数递减 1，返回
	if (!inode->i_dev) {
		inode->i_count--;
		return;
	}
	// 如果 i 节点对应的设备号=0，则将此节点的引用计数递减 1，返回
	if (S_ISBLK(inode->i_mode)) {
		sync_dev(inode->i_zone[0]);
		wait_on_inode(inode);
	}
repeat:
	// 如果 i 节点的引用计数大于 1，则递减 1
	if (inode->i_count>1) {
		inode->i_count--;
		return;
	}
	// 如果 i 节点的链接数为 0，则释放该 i 节点的所有逻辑块，并释放该 i 节点
	if (!inode->i_nlinks) {
		truncate(inode);
		free_inode(inode);
		return;
	}
	// 如果该 i 节点已作过修改，则更新该 i 节点，并等待该 i 节点解锁
	if (inode->i_dirt) {
		write_inode(inode);	/* we can sleep - so do again */
		wait_on_inode(inode);
		goto repeat;
	}
	inode->i_count--;
	return;
}

// 从 i 节点表(inode_table)中获取一个空闲 i 节点项。
// 寻找引用计数 count 为 0 的 i 节点，并将其写盘后清零，返回其指针
struct m_inode * get_empty_inode(void)
{
	struct m_inode * inode;
	static struct m_inode * last_inode = inode_table;
	int i;

	do {
		inode = NULL;
		for (i = NR_INODE; i ; i--) {
			// 如果 last_inode 已经指向 i 节点表的最后 1 项之后，则让其重新指向 i 节点表开始处
			if (++last_inode >= inode_table + NR_INODE)
				last_inode = inode_table;
			// 如果 last_inode 所指向的 i 节点的计数值为 0，则说明可能找到空闲 i 节点项
			// 让 inode 指向该 i 节点
			// 如果该 i 节点的已修改标志和锁定标志均为 0，则我们可以使用该 i 节点，退出循环
			if (!last_inode->i_count) {
				inode = last_inode;
				if (!inode->i_dirt && !inode->i_lock)
					break;
			}
		}
		// 如果没有找到空闲 i 节点(inode=NULL)，则将整个 i 节点表打印出来供调试使用，并死机
		if (!inode) {
			for (i=0 ; i<NR_INODE ; i++)
				printk("%04x: %6d\t",inode_table[i].i_dev,
					inode_table[i].i_num);
			panic("No free inodes in mem");
		}
		wait_on_inode(inode);
		while (inode->i_dirt) {
			write_inode(inode);
			wait_on_inode(inode);
		}
	} while (inode->i_count);
	// 已找到空闲 i 节点项。则将该 i 节点项内容清零，并置引用标志为 1，返回该 i 节点指针
	memset(inode,0,sizeof(*inode));
	inode->i_count = 1;
	return inode;
}

// 获取管道节点。返回为 i 节点指针（如果是 NULL 则失败）
// 首先扫描 i 节点表，寻找一个空闲 i 节点项，然后取得一页空闲内存供管道使用
// 然后将得到的 i 节点的引用计数置为 2(读者和写者)，初始化管道头和尾，置 i 节点的管道类型表示
struct m_inode * get_pipe_inode(void)
{
	struct m_inode * inode;

	if (!(inode = get_empty_inode()))
		return NULL;
	if (!(inode->i_size=get_free_page())) {
		inode->i_count = 0;
		return NULL;
	}
	inode->i_count = 2;	/* sum of readers/writers */
	// 这里对head指针和tail指针进行了初始化
	PIPE_HEAD(*inode) = PIPE_TAIL(*inode) = 0;
	inode->i_pipe = 1;
	return inode;
}

// 从设备上读取指定节点号的 i 节点。
// nr - i 节点号
struct m_inode * iget(int dev,int nr)
{
	struct m_inode * inode, * empty;

	if (!dev)
		panic("iget with dev==0");
	empty = get_empty_inode();
	inode = inode_table;
	while (inode < NR_INODE+inode_table) {
		if (inode->i_dev != dev || inode->i_num != nr) {
			inode++;
			continue;
		}
		wait_on_inode(inode);
		// 在等待该节点解锁的阶段，节点表可能会发生变化，所以再次判断，如果发生了变化，则再次重新
		// 扫描整个 i 节点表
		if (inode->i_dev != dev || inode->i_num != nr) {
			inode = inode_table;
			continue;
		}
		inode->i_count++;
		// 如果该 i 节点是其它文件系统的安装点，则在超级块表中搜寻安装在此 i 节点的超级块。如果没有
		// 找到，则显示出错信息，并释放函数开始获取的空闲节点，返回该 i 节点指针
		if (inode->i_mount) {
			int i;

			for (i = 0 ; i<NR_SUPER ; i++)
				if (super_block[i].s_imount==inode)
					break;
			if (i >= NR_SUPER) {
				printk("Mounted inode hasn't got sb\n");
				if (empty)
					iput(empty);
				return inode;
			}
		// 将该 i 节点写盘。从安装在此 i 节点文件系统的超级块上取设备号，并令 i 节点号为 1
		// 然后重新扫描整个 i 节点表，取该被安装文件系统的根节点
			iput(inode);
			dev = super_block[i].s_dev;
			nr = ROOT_INO;
			inode = inode_table;
			continue;
		}
		// 已经找到相应的 i 节点，因此放弃临时申请的空闲节点，返回该找到的 i 节点
		if (empty)
			iput(empty);
		return inode;
	}
	// 如果在 i 节点表中没有找到指定的 i 节点，则利用前面申请的空闲 i 节点在 i 节点表中建立该节点。
	// 并从相应设备上读取该 i 节点信息。返回该 i 节点
	if (!empty)
		return (NULL);
	inode=empty;
	inode->i_dev = dev;
	inode->i_num = nr;
	read_inode(inode);
	return inode;
}

// 从设备上读取指定 i 节点的信息到内存中（缓冲区中）
static void read_inode(struct m_inode * inode)
{
	struct super_block * sb;
	struct buffer_head * bh;
	int block;
// 首先锁定该 i 节点，取该节点所在设备的超级块
	lock_inode(inode);
	if (!(sb=get_super(inode->i_dev)))
		panic("trying to read inode without dev");
	block = 2 + sb->s_imap_blocks + sb->s_zmap_blocks +
		(inode->i_num-1)/INODES_PER_BLOCK;
	if (!(bh=bread(inode->i_dev,block)))
		panic("unable to read i-node block");
	*(struct d_inode *)inode =
		((struct d_inode *)bh->b_data)
			[(inode->i_num-1)%INODES_PER_BLOCK];
	brelse(bh);
	unlock_inode(inode);
}

static void write_inode(struct m_inode * inode)
{
	struct super_block * sb;
	struct buffer_head * bh;
	int block;

	lock_inode(inode);
	if (!inode->i_dirt || !inode->i_dev) {
		unlock_inode(inode);
		return;
	}
	if (!(sb=get_super(inode->i_dev)))
		panic("trying to write inode without device");
	block = 2 + sb->s_imap_blocks + sb->s_zmap_blocks +
		(inode->i_num-1)/INODES_PER_BLOCK;
	if (!(bh=bread(inode->i_dev,block)))
		panic("unable to read i-node block");
	((struct d_inode *)bh->b_data)
		[(inode->i_num-1)%INODES_PER_BLOCK] =
			*(struct d_inode *)inode;
	bh->b_dirt=1;
	inode->i_dirt=0;
	brelse(bh);
	unlock_inode(inode);
}
