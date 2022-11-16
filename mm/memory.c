/*
 *  linux/mm/memory.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 * demand-loading started 01.12.91 - seems it is high on the list of
 * things wanted, and it should be easy to implement. - Linus
 */

/*
 * Ok, demand-loading was easy, shared pages a little bit tricker. Shared
 * pages started 02.12.91, seems to work. - Linus.
 *
 * Tested sharing by executing about 30 /bin/sh: under the old kernel it
 * would have taken more than the 6M I have free, but it worked well as
 * far as I could see.
 *
 * Also corrected some "invalidate()"s - I wasn't doing enough of them.
 */


/*
 *页表结构：高20位为页表或者物理地址的基地址，标志位D(是否写过)，A(是否读过)，U/S(访问权限)
 *R/W(读，写，执行)，P(内存是否有效)
 */

#include <signal.h>

#include <asm/system.h>

#include <linux/sched.h>
#include <linux/head.h>
#include <linux/kernel.h>

volatile void do_exit(long code);

static inline volatile void oom(void)
{
	printk("out of memory\n\r");
	do_exit(SIGSEGV);
}

// 通过重置CR3为0，刷新“页变换高速缓存”

#define invalidate() \
__asm__("movl %%eax,%%cr3"::"a" (0))

/* these are not to be changed without changing head.s etc */

#define LOW_MEM 0x100000                      // 1M，1M以内的内存是内核专用的
#define PAGING_MEMORY (15*1024*1024)          // 16M
#define PAGING_PAGES (PAGING_MEMORY>>12)      // >>12即除以4K算出有多少个页
#define MAP_NR(addr) (((addr)-LOW_MEM)>>12)   // 计算页的编号
#define USED 100                              // 页面占用标志

// 判断地址是否在当前的代码段中

#define CODE_SPACE(addr) ((((addr)+4095)&~4095) < \
current->start_code + current->end_code)

static long HIGH_MEMORY = 0;                  // 存放实际物理内存的最高端地址

// 复制一页内存

#define copy_page(from,to) \
__asm__("cld ; rep ; movsl"::"S" (from),"D" (to),"c" (1024):"cx","di","si")

/*
 *主内存区中所有物理内存页的状态。每个字节描述一个物理内存页的占用状态。
 *其中的值表示被占用的次数，0 表示对应的物理内存空闲着。当申请一页物理
 *内存时，就将对应字节的值增 1。
 */

static unsigned char mem_map [ PAGING_PAGES ] = {0,};

/*
 * Get physical address of first (actually last :-) free page, and mark it
 * used. If no free pages left, return 0.
 */

/***************IMPORTANT***********
 * free_page和get_free_page函数是针对物理内存的，与线性地址无关
 */

/*
 *get_free_page()函数用于在主内存区中申请一页空闲内存页，并返回物理内存页的起始地址。它首先
 *扫描内存页面字节图数组 mem_map[]，寻找值是 0 的字节项（对应空闲页面）。若无则返回 0 结束，表
 *示物理内存已使用完。若找到值为 0 的字节，则将其置 1，并换算出对应空闲页面的起始地址。然后对
 *该内存页面作清零操作。最后返回该空闲页面的物理内存起始地址。
 *注意！！！！：这里只是在主内存区域申请一页空闲内存页，此处还没映射至线性地址，put_page函数完成映射
 */

unsigned long get_free_page(void)
{
register unsigned long __res asm("ax");

__asm__("std ; repne ; scasb\n\t"
	"jne 1f\n\t"
	"movb $1,1(%%edi)\n\t"
	"sall $12,%%ecx\n\t"
	"addl %2,%%ecx\n\t"
	"movl %%ecx,%%edx\n\t"
	"movl $1024,%%ecx\n\t"
	"leal 4092(%%edx),%%edi\n\t"
	"rep ; stosl\n\t"
	"movl %%edx,%%eax\n"
	"1:"
	:"=a" (__res)
	:"0" (0),"i" (LOW_MEM),"c" (PAGING_PAGES),
	"D" (mem_map+PAGING_PAGES-1)
	:"di","cx","dx");
return __res;
}


/*
 * Free a page of memory at physical address 'addr'. Used by
 * 'free_page_tables()'
 */

/*
 *free_page()用于释放指定地址处的一页物理内存。它首先判断指定的内存地址是否<1M，若是则返
 *回，因为 1M 以内是内核专用的；若指定的物理内存地址大于或等于实际内存最高端地址，则显示出错
 *信息；然后由指定的内存地址换算出页面号: (addr - 1M)/4K；接着判断页面号对应的 mem_map[]字节项
 *是否为 0，若不为 0，则减 1 返回；否则对该字节项清零，并显示“试图释放一空闲页面”的出错信息。
 */
void free_page(unsigned long addr)
{
	if (addr < LOW_MEM) return;
	if (addr >= HIGH_MEMORY)
		panic("trying to free nonexistent page");
	addr -= LOW_MEM;
	addr >>= 12;
	if (mem_map[addr]--) return;
	mem_map[addr]=0;
	panic("trying to free free page");
}

/*
 * This function frees a continuos block of page tables, as needed
 * by 'exit()'. As does copy_page_tables(), this handles only 4Mb blocks.
 */

/*
 *free_page_tables()用于释放指定线性地址和长度（页表个数）对应的物理内存页。它首先判断指定的
 *线性地址是否在 4M 的边界上，若不是则显示出错信息，并死机；然后判断指定的地址值是否=0，若是，
 *则显示出错信息“试图释放内核和缓冲区所占用的空间”，并死机；接着计算在页目录表中所占用的目录
 *项数 size，也即页表个数，并计算对应的起始目录项号；然后从对应起始目录项开始，释放所占用的所
 *有 size 个目录项；同时释放对应目录项所指的页表中的所有页表项和相应的物理内存页；最后刷新页变
 *换高速缓冲。
 */

int free_page_tables(unsigned long from,unsigned long size)
{
	// Linus真的太牛了，这里一开始定义为unsigned long *类型的指针，则后面++一次增加4个字节
	unsigned long *pg_table;
	unsigned long * dir, nr;

	if (from & 0x3fffff)
		panic("free_page_tables called with wrong alignment");
	if (!from)
		panic("Trying to free up swapper memory space");
	/*
	 *计算参数size给出的长度所占的页目录项数（4MB的进位整数倍）,也即所占页表数
     *因为1个页表可管理4MB物理内存，所以这里用右移22位的方式把需要复制的内存长度值
     *除以4MB.其中加上0x3fffff(即4MB-1)用于得到进位整数倍结果，即操作若有余数则进1.
     *例如，如果原size=4.01Mb，那么可得到结果sieze=2
	 */
	size = (size + 0x3fffff) >> 22;
	/*
	 *计算给出的线性基地址对应的真实目录项地址。对应的目录项号＝from>>22.因为每
     *项占4字节，并且由于页目录表从物理地址0开始存放，因此实际目录项指针＝目录
     *项号<<2，也即(from>>20)。& 0xffc确保目录项指针范围有效，即用于屏蔽目录项
     *指针最后2位。因为只移动了20位，因此最后2位是页表项索引的内容，应屏蔽掉。
	 */
	dir = (unsigned long *) ((from>>20) & 0xffc); /* _pg_dir = 0 */
	// --> !!!!哈皮了这里，其实是-- >
	for ( ; size-->0 ; dir++) {
		// 如果p位为0则继续，p位为0即无效表示对应页表不存在
		if (!(1 & *dir))
			continue;
		pg_table = (unsigned long *) (0xfffff000 & *dir); // 低12位为0，取页表地址
		// 释放每一个页表中的页表项
		for (nr=0 ; nr<1024 ; nr++) {
			if (1 & *pg_table)                      // p为1则释放
				free_page(0xfffff000 & *pg_table);  // 其实就是mem_map[]为0
			// 页表项清0
			*pg_table = 0;
			pg_table++;
		}
		free_page(0xfffff000 & *dir);
		*dir = 0;
	}
	// 刷新页表
	invalidate();
	return 0;
}

/*
 *  Well, here is one of the most complicated functions in mm. It
 * copies a range of linerar addresses by copying only the pages.
 * Let's hope this is bug-free, 'cause this one I don't want to debug :-)
 *
 * Note! We don't copy just any chunks of memory - addresses have to
 * be divisible by 4Mb (one page-directory entry), as this makes the
 * function easier. It's used only by fork anyway.
 *
 * NOTE 2!! When from==0 we are copying kernel space for the first
 * fork(). Then we DONT want to copy a full page-directory entry, as
 * that would lead to some serious memory waste - we just copy the
 * first 160 pages - 640kB. Even that is more than we need, but it
 * doesn't take any more memory - we don't copy-on-write in the low
 * 1 Mb-range, so the pages can be shared with the kernel. Thus the
 * special case for nr=xxxx.
 */

/*
 *copy_page_tables()用于复制指定线性地址和长度（页表个数）内存对应的页目录项和页表，从而被
 *复制的页目录和页表对应的原物理内存区被共享使用。该函数首先验证指定的源线性地址和目的线性地
 *址是否都在 4Mb 的内存边界地址上，否则就显示出错信息，并死机；然后由指定线性地址换算出对应的
 *起始页目录项（from_dir, to_dir）；并计算需复制的内存区占用的页表数（即页目录项数）；接着开始分别
 *将原目录项和页表项复制到新的空闲目录项和页表项中。页目录表只有一个，而新进程的页表需要申请
 *空闲内存页面来存放；此后再将原始和新的页目录和页表项都设置成只读的页面。当有写操作时就利用
 *页异常中断调用，执行写时复制操作。最后对共享物理内存页对应的字节图数组 mem_map[]的标志进行
 *增 1 操作。
 */

// https://blog.csdn.net/jmh1996/article/details/83515833解析

int copy_page_tables(unsigned long from,unsigned long to,long size)
{
	unsigned long * from_page_table;
	unsigned long * to_page_table;
	unsigned long this_page;
	unsigned long * from_dir, * to_dir;
	unsigned long nr;

	if ((from&0x3fffff) || (to&0x3fffff))
		panic("copy_page_tables called with wrong alignment");
	from_dir = (unsigned long *) ((from>>20) & 0xffc); /* _pg_dir = 0 */
	to_dir = (unsigned long *) ((to>>20) & 0xffc);
	size = ((unsigned) (size+0x3fffff)) >> 22;
	for( ; size-->0 ; from_dir++,to_dir++) {
		if (1 & *to_dir)
			panic("copy_page_tables: already exist");
		if (!(1 & *from_dir))
			continue;
		from_page_table = (unsigned long *) (0xfffff000 & *from_dir);
		if (!(to_page_table = (unsigned long *) get_free_page()))
			return -1;	/* Out of memory, see freeing */
		*to_dir = ((unsigned long) to_page_table) | 7;          // 或7(111)，即设置PTE标志位(U/S，R/w，P)
		// 针对当前处理的页表，设置需复制的页面数。
		// 如果是在内核空间，则仅需复制头 160 页（640KB），
		// 否则需要复制一个页表中的所有 1024 页面
		nr = (from==0)?0xA0:1024;
		for ( ; nr-- > 0 ; from_page_table++,to_page_table++) {
			this_page = *from_page_table;
			if (!(1 & this_page))
				continue;
			// 复位页表项中 R/W 标志(置 0)。
			// (如果 U/S 位是 0，则 R/W 就没有作用。如果 U/S 是 1，而 R/W 是 0，
			// 那么运行在用户层的代码就只能读页面。如果 U/S 和 R/W 都置位，则就有写的权限。
			// p位置1
			this_page &= ~2;
			*to_page_table = this_page;
			// 以下代码在进程0创建进程1时并不会执行，因为此时from还在1MB内存下，即内核数据区
			// 同理此时也不需要设置mem_map[]，因为mem_map[]之管理主内存页面(1MB以上)
			if (this_page > LOW_MEM) {
				// 这里其实是设置父进程对页面也只能读(260行)，后面采用COW
				*from_page_table = this_page;
				this_page -= LOW_MEM;
				this_page >>= 12;
				// 页的引用次数加一
				mem_map[this_page]++;
			}
		}
	}
	invalidate();
	return 0;
}

/*
 * This function puts a page in memory at the wanted address.
 * It returns the physical address of the page gotten, 0 if
 * out of memory (either when trying to access page-table or
 * page.)
 */

/*
 *put_page()用于将一指定的物理内存页面映射到指定的线性地址处。它首先判断指定的内存页面地址
 *的有效性，要在 1M 和系统最高端内存地址之外，否则发出警告；然后计算该指定线性地址在页目录表
 *中对应的目录项；此时若该目录项有效（P=1），则取其对应页表的地址；否则申请空闲页给页表使用，
 *并设置该页表中对应页表项的属性。
 *最后返回指定的物理内存页面地址，若内存不够则返回0。
 *Page：物理地址   address：线性地址
 */
unsigned long put_page(unsigned long page,unsigned long address)
{
	unsigned long tmp, *page_table;

/* NOTE !!! This uses the fact that _pg_dir=0 */

	if (page < LOW_MEM || page >= HIGH_MEMORY)
		printk("Trying to put page %p at %p\n",page,address);
	if (mem_map[(page-LOW_MEM)>>12] != 1)
		printk("mem_map disagrees with %p at %p\n",page,address);
	page_table = (unsigned long *) ((address>>20) & 0xffc);
	if ((*page_table)&1)
		page_table = (unsigned long *) (0xfffff000 & *page_table);
	else {
		if (!(tmp=get_free_page()))
			return 0;
		*page_table = tmp|7;
		page_table = (unsigned long *) tmp;
	}
	page_table[(address>>12) & 0x3ff] = page | 7;
/* no need for invalidate */
	return page;
}

/*
 *取消写保护页面函数。用于页异常中断过程中写保护异常的处理（写时复制）。
 *输入参数为页表项指针。
 *[ un_wp_page 意思是取消页面的写保护：Un-Write Protected。
 */

void un_wp_page(unsigned long * table_entry)
{
	unsigned long old_page,new_page;

	old_page = 0xfffff000 & *table_entry;
	if (old_page >= LOW_MEM && mem_map[MAP_NR(old_page)]==1) {
		// R/W位置1，取消写保护
		*table_entry |= 2;
		invalidate();
		return;
	}
	if (!(new_page=get_free_page()))
		oom();
	if (old_page >= LOW_MEM)
		mem_map[MAP_NR(old_page)]--;   // 页面引用次数减一
	*table_entry = new_page | 7;       // 设置标志位
	invalidate();
	copy_page(old_page,new_page);      // 复制页表内容
}	

/*
 * This routine handles present pages, when users try to write
 * to a shared page. It is done by copying the page to a new address
 * and decrementing the shared-page counter for the old page.
 *
 * If it's in code space we exit with a segment error.
 */

/*
 *do_wp_page()是页异常中断过程（在 mm/page.s 中实现）中调用的页写保护处理函数。它首先判断
 *地址是否在进程的代码区域，若是则终止程序（代码不能被改动）；然后执行写时复制页面的操作（Copy
 *on Write）。
 *error_code是CPU自动产生，address是线性地址	
 */

void do_wp_page(unsigned long error_code,unsigned long address)
{
#if 0
/* we cannot do this yet: the estdio library writes to code space */
/* stupid, stupid. I really want the libc.a from GNU */
	if (CODE_SPACE(address)) // 如果地址位于代码空间，则终止执行程序。
		do_exit(SIGSEGV);
#endif
	un_wp_page((unsigned long *)
		(((address>>10) & 0xffc) + (0xfffff000 &
		*((unsigned long *) ((address>>20) &0xffc)))));

}

/*
 *写页面验证。
 *若页面不可写，则复制页面。在 fork.c 第 34 行被调用。
 */

void write_verify(unsigned long address)
{
	unsigned long page;
    // 判断页目录表项是否存在(检查p位)，不存在则直接返回
	if (!( (page = *((unsigned long *) ((address>>20) & 0xffc)) )&1))
		return;
	page &= 0xfffff000;
	// 计算页表项地址
	page += ((address>>10) & 0xffc);
	if ((3 & *(unsigned long *) page) == 1)  /* non-writeable, present */
		un_wp_page((unsigned long *) page);
	return;
}

/*
 *get_empty_page()用于取得一页空闲物理内存并映射到指定线性地址处。主要使用了 get_free_page()
 *和 put_page()函数来实现该功能。
 */

void get_empty_page(unsigned long address)
{
	unsigned long tmp;

	if (!(tmp=get_free_page()) || !put_page(tmp,address)) {
		free_page(tmp);		/* 0 is ok - ignored */
		oom();
	}
}

/*
 * try_to_share() checks the page at address "address" in the task "p",
 * to see if it exists, and if it is clean. If so, share it with the current
 * task.
 *
 * NOTE! This assumes we have checked that p != current, and that they
 * share the same executable.
 */

/*
 *try_to_share()在任务"p"中检查位于地址"address"处的页面，看页面是否存在，是否干净。
 *如果是干净的话，就与当前任务共享。
 *
 * 注意！这里我们已假定 p !=当前任务，并且它们共享同一个执行程序。
 */

static int try_to_share(unsigned long address, struct task_struct * p)
{
	unsigned long from;
	unsigned long to;
	unsigned long from_page;
	unsigned long to_page;
	unsigned long phys_addr;

	from_page = to_page = ((address>>20) & 0xffc);       // 计算内存地址的页目录项位置
	from_page += ((p->start_code>>20) & 0xffc);          // 计算程p起始代码页目录项位置 
	to_page += ((current->start_code>>20) & 0xffc);      // 计算当前进程中起始代码页目录项位置
/* is there a page-directory at from? */
	from = *(unsigned long *) from_page;                 // 计算内存地址页目录项中的内容
	if (!(from & 1))                                     // 检查p位，若p位为0即无效，直接返回0
		return 0;
	from &= 0xfffff000;                                  // 若p位为1则计算出对应页表的基址
	phys_addr = *(unsigned long *) from_page;
/* is the page clean and present? */
	if ((phys_addr & 0x41) != 0x01)                      // 判断dirt位和present位，脏或无效则返回0
		return 0;
	phys_addr &= 0xfffff000;                             // 计算物理内存单元基地址
	if (phys_addr >= HIGH_MEMORY || phys_addr < LOW_MEM)
		return 0;
/* 下面对当前进程(current)的内存地址进行计算*/		
	to = *(unsigned long *) to_page;
	if (!(to & 1))
		if (to = get_free_page())
			*(unsigned long *) to_page = to | 7;         // 申请内存后注意更新页目录项指向
		else
			oom();
	to &= 0xfffff000;
	to_page = to + ((address>>10) & 0xffc);
	if (1 & *(unsigned long *) to_page)
		panic("try_to_share: to_page already exists");
/* share them: write-protect */
	*(unsigned long *) from_page &= ~2;                  // 对P进程页面设置只读即添加写保护
	*(unsigned long *) to_page = *(unsigned long *) from_page;  // 设置当前进程页表项重新指向
	invalidate();
/* 这里物理地址不用加上offset，因为>>12就抵消了*/
	phys_addr -= LOW_MEM;
	phys_addr >>= 12;
	mem_map[phys_addr]++;                                // 页面引用加一
	return 1;
}

/*
 * share_page() tries to find a process that could share a page with
 * the current one. Address is the address of the wanted page relative
 * to the current data space.
 *
 * We first check if it is at all feasible by checking executable->i_count.
 * It should be >1 if there are other tasks sharing this inode.
 */
static int share_page(unsigned long address)
{
	struct task_struct ** p;

	if (!current->executable)                         // 如果进程是不可执行的，则直接返回
		return 0;
	if (current->executable->i_count < 2)             // 如果进程只能单独执行，也直接返回
		return 0;
	for (p = &LAST_TASK ; p > &FIRST_TASK ; --p) {
		// 空闲进程不满足要求
		if (!*p)
			continue;
		// 当前进程不满足要求
		if (current == *p)
			continue;
		// executable不等不满足要求
		if ((*p)->executable != current->executable)
			continue;
		if (try_to_share(address,*p))
			return 1;
	}
	return 0;
}

/*
 *********************************缺页处理函数**************************************
 *do_no_page()是页异常中断过程中调用的缺页处理函数。它首先判断指定的线性地址在一个进程空
 *间中相对于进程基址的偏移长度值。如果它大于代码加数据长度，或者进程刚开始创建，则立刻申请一
 *页物理内存，并映射到进程线性地址中，然后返回；接着尝试进行页面共享操作，若成功，则立刻返回；
 *否则申请一页内存并从设备中读入一页信息；若加入该页信息时，指定线性地址+1 页长度超过了进程代
 *码加数据的长度，则将超过的部分清零。然后将该页映射到指定的线性地址处。
 *error_code是CPU自动产生，address是线性地址
 */

void do_no_page(unsigned long error_code,unsigned long address)
{
	int nr[4];
	unsigned long tmp;
	unsigned long page;
	int block,i;

    // 计算address对应的页首地址，即减去最后12位的偏移地址
	address &= 0xfffff000;
	// 计算离代码段首地址的偏移
	tmp = address - current->start_code;
	/*executable 是进程的 i 节点结构。该值为 0，表明进程刚开始设置，需要内存；
	 *start_code 是进程代码段地址，end_data 是代码加数据长度。对于 Linux 内核，它的代码段和
     *数据段是起始基址是相同的。
	 *tmp > end_data说明是访问堆或者栈的空间时发生的缺页
     *因此就直接调用 get_empty_page()函数，申请一页物理内存并映射到指定线性地址处即可。
	 */
	if (!current->executable || tmp >= current->end_data) {
		get_empty_page(address);
		return;
	}
	// 是否有进程已经使用
	if (share_page(tmp))
		return;
	if (!(page = get_free_page()))
		oom();
/* remember that 1 block is used for header（程序头需要使用一个block） */
	/*
	 *首先计算缺页所在的数据块项。BLOCK_SIZE = 1024 字节，因此一页内存需要 4 个数据块。
	 *算出要读的硬盘块号，但是最多读四块。
     *tmp/BLOCK_SIZE算出线性地址对应页的
     *页首地址离代码块距离了多少块，然后读取页首
     *地址对应的块号，所以需要加一。比如距离2块的距离，则
     *需要读取的块是第三块
	 */
	block = 1 + tmp/BLOCK_SIZE;
	for (i=0 ; i<4 ; block++,i++)
		nr[i] = bmap(current->executable,block);       // 根据 i 节点信息，取数据块在设备上的对应的逻辑块号。
	bread_page(page,current->executable->i_dev,nr);    // 读设备上一个页面的数据（4 个逻辑块）到指定物理地址 page 处。
	// 在增加了一页内存后，该页内存的部分可能会超过进程的 end_data 位置。下面的循环即是对物理
	// 页面超出的部分进行清零处理
	i = tmp + 4096 - current->end_data;
	tmp = page + 4096;
	while (i-- > 0) {
		tmp--;
		*(char *)tmp = 0;
	}
	if (put_page(page,address))
		return;
	free_page(page);
	oom();
}

/*
 *物理内存初始化。
 *start_mem - 可用作分页处理的物理内存起始位置（已去除 RAMDISK 所占内存空间等）。
 *end_mem - 实际物理内存最大地址。
 *在该版的 Linux 内核中，最多能使用 16Mb 的内存，大于 16Mb 的内存将不于考虑，弃置不用。
 *0 - 1Mb 内存空间用于内核系统（其实是 0-640Kb）。
 */ 
void mem_init(long start_mem, long end_mem)
{
	int i;

	HIGH_MEMORY = end_mem;               // HIGH_MEMORY初始化是为0的
	for (i=0 ; i<PAGING_PAGES ; i++)
		mem_map[i] = USED;
	i = MAP_NR(start_mem);
	end_mem -= start_mem;
	end_mem >>= 12;
	while (end_mem-->0)
		mem_map[i++]=0;
}


// 计算内存空闲页面数
void calc_mem(void)
{
	int i,j,k,free=0;                   // free即空闲页面数
	long * pg_tbl;

	for(i=0 ; i<PAGING_PAGES ; i++)
		if (!mem_map[i]) free++;
	printk("%d pages free (of %d)\n\r",free,PAGING_PAGES);
	// 扫描所有页目录项（除 0，1 项），如果页目录项有效，则统计对应页表中有效页面数，并显示。
	for(i=2 ; i<1024 ; i++) {
		if (1&pg_dir[i]) {
			pg_tbl=(long *) (0xfffff000 & pg_dir[i]);
			for(j=k=0 ; j<1024 ; j++)
				if (pg_tbl[j]&1)
					k++;
			printk("Pg-dir[%d] uses %d pages\n",i,k);
		}
	}
}
