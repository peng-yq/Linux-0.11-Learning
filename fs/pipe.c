/*
 *  linux/fs/pipe.c
 *
 *  (C) 1991  Linus Torvalds
 */

#include <signal.h>

#include <linux/sched.h>
#include <linux/mm.h>	/* for get_free_page */
#include <asm/segment.h>

int read_pipe(struct m_inode * inode, char * buf, int count)
{
	int chars, size, read = 0;

	while (count>0) {
		// 若当前管道中没有数据(size=0)，则唤醒等待该节点的进程
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
	for(i=0;j<2 && i<NR_FILE;i++)
		if (!file_table[i].f_count)
			(f[j++]=i+file_table)->f_count++;
	if (j==1)
		f[0]->f_count=0;
	if (j<2)
		return -1;
	j=0;
	for(i=0;j<2 && i<NR_OPEN;i++)
		if (!current->filp[i]) {
			current->filp[ fd[j]=i ] = f[j];
			j++;
		}
	if (j==1)
		current->filp[fd[0]]=NULL;
	if (j<2) {
		f[0]->f_count=f[1]->f_count=0;
		return -1;
	}
	if (!(inode=get_pipe_inode())) {
		current->filp[fd[0]] =
			current->filp[fd[1]] = NULL;
		f[0]->f_count = f[1]->f_count = 0;
		return -1;
	}
	f[0]->f_inode = f[1]->f_inode = inode;
	f[0]->f_pos = f[1]->f_pos = 0;
	f[0]->f_mode = 1;		/* read */
	f[1]->f_mode = 2;		/* write */
	put_fs_long(fd[0],0+fildes);
	put_fs_long(fd[1],1+fildes);
	return 0;
}
