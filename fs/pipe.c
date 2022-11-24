/*
 *  linux/fs/pipe.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*管道特点如下：
 *管道是半双工的，数据只能向一个方向流动；需要双方通信时，需要建立起两个管道
 *只能用于父子进程或者兄弟进程之间（具有亲缘关系的进程）
 *单独构成一种独立的文件系统：管道对于管道两端的进程而言，就是一个文件
 *但它不是普通的文件，它不属于某种文件系统，而是自立门户，单独构成一种文件系统，并且只存在于内存中
 *数据的读出和写入：一个进程向管道中写的内容被管道另一端的进程读出
 *写入的内容每次都添加在管道缓冲区的末尾，并且每次都是从缓冲区的头部读出数据

 *管道的主要局限性正体现在它的特点上：
 *只支持单向数据流（现在，某些系统提供全双工管道，但是为了最佳的可移植性，我们决不能预先假定系统提供此特性。）
 *只能用于具有亲缘关系的进程之间
 *没有名字(匿名管道)（有名管道是 FIFO）
 *管道的缓冲区是有限的（管道制存在于内存中，在管道创建时，为缓冲区分配一个页面大小）
 *管道所传送的是无格式字节流，这就要求管道的读出方和写入方必须事先约定好数据的格式，比如多少字节算作一个消息（或命令、或记录）等等
 */

#include <signal.h>

#include <linux/sched.h>
#include <linux/mm.h>	/* for get_free_page */
#include <asm/segment.h>

int read_pipe(struct m_inode * inode, char * buf, int count)
{
	int chars, size, read = 0;

	while (count>0) {
		// 若当前管道中没有数据(size=0)，则唤醒等待该节点的进程（写进程 ）
		// 注意这里的size计算方法和write_pipe函数不一样，这里直接就是PIPE_SIZE，因为“读只能读未读的且已写入的数据”
		while (!(size=PIPE_SIZE(*inode))) {
			wake_up(&inode->i_wait);
			// 如果已没有写管道者，则返回已读字节数，退出
			if (inode->i_count != 2) /* are there any writers? */
				return read;
			// 若有写管道者在该 i 节点上，则睡眠等待写入信息
			sleep_on(&inode->i_wait);
		}
		// 计算“未读”的字节(这里可能包含未写入的占位)
		// 后面步骤和write_pipe大差不差了
		chars = PAGE_SIZE-PIPE_TAIL(*inode);
		if (chars > count)
			chars = count;
		if (chars > size)
			chars = size;
		count -= chars;
		read += chars;
		size = PIPE_TAIL(*inode);
		PIPE_TAIL(*inode) += chars;
		PIPE_TAIL(*inode) &= (PAGE_SIZE-1);
		while (chars-->0)
			put_fs_byte(((char *)inode->i_size)[size++],buf++);
	}
	wake_up(&inode->i_wait);
	return read;
}

// 博客解析:https://blog.csdn.net/lmdyyh/article/details/18282571
int write_pipe(struct m_inode * inode, char * buf, int count)
{
	int chars, size, written = 0;

	while (count>0) {
		// 计算可写的size(包括已读的和未写的)，若当前没有可写的空间了
		while (!(size=(PAGE_SIZE-1)-PIPE_SIZE(*inode))) {
			// 唤醒等待该节点的进程
			wake_up(&inode->i_wait);
			// 若没有读管道者，向进程发送SIGPIPE信号，并返回已写入的字节数并退出(没写则返回-1)
			if (inode->i_count != 2) { /* no readers */
				current->signal |= (1<<(SIGPIPE-1));
				return written?written:-1;
			}
			// 若有读管道者，则睡眠该节点，等待管道腾出空间
			sleep_on(&inode->i_wait);
		}
		// 取管道头部到缓冲区末端空间字节数 chars
		chars = PAGE_SIZE-PIPE_HEAD(*inode);
		// 如果 chars大于还需要写入的字节数 count，则令其等于count
		if (chars > count)
			chars = count;
		// 如果 chars 大于当前管道中空闲空间长度 size，则令其等于size。
		if (chars > size)
			chars = size;
		// 减去等下会写入的字节
		count -= chars;
		// 更新已写入的字节
		written += chars;
		// 获取head指针索引
		size = PIPE_HEAD(*inode);
		// 移动head指针
		PIPE_HEAD(*inode) += chars;
		// 注意这里求余操作又来了，这样超过了PAGE_SIZE的话直接“回滚”，Linus真的骚操作太多了
		PIPE_HEAD(*inode) &= (PAGE_SIZE-1);
		// 写入数据，i_size是管道缓冲块指针
		while (chars-->0)
			((char *)inode->i_size)[size++]=get_fs_byte(buf++);
		// 进入下一波循环，直至全部写入完成
	}

	// 唤醒等待该 i 节点的进程，返回已写入的字节数，退出
	wake_up(&inode->i_wait);
	return written;
}

/* 
 *创建管道系统调用函数
 *在 fildes 所指的数组中创建一对文件句柄(描述符)。这对文件句柄指向一管道 i 节点
 *fildes[0]用于读管道中数据，fildes[1]用于向管道中写入数据
 *成功时返回 0，出错时返回-1
 */
int sys_pipe(unsigned long * fildes)
{
	struct m_inode * inode;
	struct file * f[2];
	int fd[2];
	int i,j;

	j=0;
	// 从file_table(这个就是file结构体数组，数组大小为NR_FILE)中取两个空闲项（引用计数字段为 0 的项），并分别设置引用计数为 1
	for(i=0;j<2 && i<NR_FILE;i++)
		if (!file_table[i].f_count)
			(f[j++]=i+file_table)->f_count++;
	// 如果只有一个空闲项，则释放该项(引用计数复位)
	if (j==1)
		f[0]->f_count=0;
	// 如果没有找到两个空闲项，则返回-1
	if (j<2)
		return -1;
	j=0;
	// 针对上面取得的两个文件结构项，分别分配一文件句柄，并使进程的文件结构指针分别指向这两个文件结构
	for(i=0;j<2 && i<NR_OPEN;i++)
		if (!current->filp[i]) {
			current->filp[ fd[j]=i ] = f[j];
			j++;
		}
	// 如果只有一个空闲文件句柄，则释放该句柄
	if (j==1)
		current->filp[fd[0]]=NULL;
	// 如果没有找到两个空闲句柄，则释放上面获取的两个文件结构项（复位引用计数值），并返回-1
	if (j<2) {
		f[0]->f_count=f[1]->f_count=0;
		return -1;
	}
	// 申请管道 i 节点，0.11中的节点数是32个，并为管道分配缓冲区（1 页内存）。
	//如果不成功，则相应释放两个文件句柄和文件结构项，并返回-1。
	if (!(inode=get_pipe_inode())) {
		current->filp[fd[0]] =
			current->filp[fd[1]] = NULL;
		f[0]->f_count = f[1]->f_count = 0;
		return -1;
	}
	// 初始化两个文件结构，都指向同一个 i 节点，读写指针都置零。
	// 第 1 个文件结构的文件模式置为读，
	// 第 2 个文件结构的文件模式置为写
	f[0]->f_inode = f[1]->f_inode = inode;
	f[0]->f_pos = f[1]->f_pos = 0;
	f[0]->f_mode = 1;		/* read */
	f[1]->f_mode = 2;		/* write */
	// 将文件句柄数组复制到对应的用户数组中，并返回 0，退出
	put_fs_long(fd[0],0+fildes);
	put_fs_long(fd[1],1+fildes);
	return 0;
}
