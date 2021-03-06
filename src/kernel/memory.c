/**
 * MIT License
 *
 * Copyright (c) 2020 王赛宇

 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:

 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.

 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * @author wangsaiyu@cqu.edu.cn
 * 本文件是对memory.h中内容的实现，实现了对内存的初始化与管理
 */

/* =========================================================================
 =                                 头的引入                                  =
 =========================================================================*/

#include "position.h"
#include "printk.h"
#include "memory.h"
#include "slab.h"
#include "lib.h"

/* =========================================================================
 =                             私有函数定义                                  =
 =========================================================================*/

/**
 * 获取cr3的内存地址
 * @return 一个长整形，表示cr3的内存地址
 */
unsigned long * GetGDT(){
    unsigned long * tmp;
    __asm__ __volatile__    (
    "movq    %%cr3,    %0    \n\t"
    :"=r"(tmp)
    :
    :"memory"
    );
    return tmp;
}

/* =========================================================================
 =                             全局参数定义                                  =
 =========================================================================*/

// 全局内存信息描述子声明于memory.h， 定义于memory.c， 全局变量
struct GlobalMemoryDescriptor memory_management_struct = {{0},0};;

int ZONE_DMA_INDEX = 0;
int ZONE_NORMAL_INDEX = 0;
int ZONE_UNMAPED_INDEX = 0;

unsigned long * Global_CR3 = NULL;

void free_pages(struct Page * page,int number){	
	int i = 0;
	if (page == NULL) {
		printk("free_pages() ERROR: page is invalid\n");
		return ;
	}	
	if (number >= 64 || number <= 0) {
		printk("free_pages() ERROR: number is invalid\n");
		return ;	
	}
	for (i = 0;i<number;i++,page++) {
		*(memory_management_struct.bits_map + ((page->PHY_address >> PAGE_2M_SHIFT) >> 6)) &= ~(1UL << (page->PHY_address >> PAGE_2M_SHIFT) % 64);
		page->zone_struct->page_using_count--;
		page->zone_struct->page_free_count++;
		page->attribute = 0;
	}
}


unsigned long page_clean(struct Page * page) {
	page->reference_count--;
	page->zone_struct->total_pages_link--;
	if (!page->reference_count) {
		page->attribute &= PG_PTable_Maped;
	}
	return 1;
}

/**
 * 获取页的状态
 * @param page 需要获取状态的页
 */
unsigned long get_page_attribute(struct Page * page) {
    if(page == NULL) {
        printk("get_page_attribute() ERROR: page == NULL\n");
        return 0;
    } else {
		return page->attribute;
	}
}

/**
 * 更新一个页的状态
 * @param page 被更改状态的页
 * @param flags 将page的状态更改为flags
 */
unsigned long set_page_attribute(struct Page * page,unsigned long flags) {
    if(page == NULL) {
        printk("set_page_attribute() ERROR: page == NULL\n");
        return 0;
    } else {
        page->attribute = flags;
        return 1;
    }
}


/**
 * 对某个内存页进行初始化， 在申请新的page时，对申请到的page调用本函数进行初始化
 *  @param page 指针，指向想要被初始化的 page
 *  @param flag 初始化时的参数
 */
unsigned long PageInit(struct Page * page, unsigned long flag){
    if (!page->attribute) { // 如果该页没有使用过
        *(memory_management_struct.bits_map + ((page->PHY_address >> PAGE_2M_SHIFT) >> 6)) |= 1UL << (page->PHY_address >> PAGE_2M_SHIFT) % 64;
        page->attribute = flag; // 将状态置为flag
        page->reference_count++; // 被使用了，ref ++
        // 对对应的zone进行修改
        page->zone_struct->page_using_count++;
        page->zone_struct->page_free_count--;
        page->zone_struct->total_pages_link++;
    } else if ((page->attribute & PG_Referenced) || (page->attribute & PG_K_Share_To_U) ||
               (flag & PG_Referenced) || (flag & PG_K_Share_To_U)) {
        // 如果已经被引用，或是有共享属性，那么就不需要在调整可用页数，而是直接改变引用数量，并且调整页的属性即可
        page->attribute |= flag;
        page->reference_count++;
        page->zone_struct->total_pages_link++;
    } else { // 既没有被引用，又没有共享属性，而且状态也不为空，那么就 添加页表属性，并置位bit映射位图的相应位。
        *(memory_management_struct.bits_map + ((page->PHY_address >> PAGE_2M_SHIFT) >> 6)) |= 1UL << (page->PHY_address >> PAGE_2M_SHIFT) % 64;
        page->attribute |= flag;
    }
    return 0UL;
}

/**
 * 读取内存中存储的内存块信息
 * 默认读取基地址为0x7e00，这里填写线性地址
 * 会输出每一块内存的信息，并且输出总可用内存
 */ 
void InitMemory(){
	unsigned long TotalMem = 0 ;
	struct MemoryBlockE820 *p = NULL;	
	
	printk("Display Physics Address MAP,Type(1:RAM,2:ROM or Reserved,3:ACPI Reclaim Memory,4:ACPI NVS Memory,Others:Undefine)\n");
	p = (struct MemoryBlockE820 *)0xffff800000007e00; // 将基地址对齐

	// ==========循环遍历，输出内存信息==========
	for(int i = 0;i < 32;i++, p++){
		if (p->type > 4 || p->length == 0 || p->type < 1) break;		
        // 输出内存信息
		printk("%d ::: Address:%#018lx\tLength:%#018lx\tType:%#010x\n", i, p->baseAddr,p->length,p->type);
        // 统计可用内存
		unsigned long tmp = 0;
		if (p->type == 1) TotalMem += p->length;
		// 存储内存块信息， 更新长度
		memory_management_struct.e820[i] = (struct MemoryBlockE820){p->baseAddr, p->length, p->type};
		memory_management_struct.e820_length = i;
	}

    // 输出总可用内存
	printk("OS Can Used Total RAM:%#018lx\n",TotalMem);

	TotalMem = 0;
	/**
	* ==========循环遍历，统计输出内存可用页的数量==========*/
	for(int i = 0; i <= memory_management_struct.e820_length; i ++){
		if (memory_management_struct.e820[i].type != 1) continue;
		unsigned long start, end;
		start = PAGE_2M_ALIGN(memory_management_struct.e820[i].baseAddr); // 计算开头的向后对齐地址
		end = (
			(memory_management_struct.e820[i].baseAddr + memory_management_struct.e820[i].length)
			 >> PAGE_2M_SHIFT
		) << PAGE_2M_SHIFT; // 计算结尾的向前对齐地址
		if (end <= start) continue;
        TotalMem += (end - start) >> PAGE_2M_SHIFT;
	}
	printk("OS Can Used Total 2M PAGEs:%#010x=%010d\n",TotalMem,TotalMem);

	// 计算最后一块内存的地址
	TotalMem = memory_management_struct.e820[memory_management_struct.e820_length].baseAddr + \
				memory_management_struct.e820[memory_management_struct.e820_length].length;
		
	printk("last Memory Address :%#010x=%010d\n",TotalMem,TotalMem);

	// ==========初始化 全局描述子中 与 bits map 相关的成员变量 及其内存空间==========
	// 计算bits_map的首地址， 计算方法为：内核程序的结束地址向后对齐， 即： 内核地址结束后的4k向后对齐
	memory_management_struct.bits_map = (unsigned long *)((memory_management_struct.end_brk + PAGE_4K_SIZE - 1) & PAGE_4K_MASK);
	// 计算bits_size， 即bits_map的大小， 其大小为最后一块可用内存地址/ 每一页的大小， 即：所有内存划分为页的页数
	memory_management_struct.bits_size = TotalMem >> PAGE_2M_SHIFT; 
	// 计算位图的内存长度
	memory_management_struct.bits_length = (((unsigned long)(TotalMem >> PAGE_2M_SHIFT) + sizeof(long) * 8 - 1) / 8) & ( ~ (sizeof(long) - 1));
	// 将bitsmap置位，全都置位为1
	memset(memory_management_struct.bits_map,0xff,memory_management_struct.bits_length);

	// 输出相关信息
	printk("bits_map : %d, bits_size: %d, bits_length: %d\n", 
	memory_management_struct.bits_map, memory_management_struct.bits_size, memory_management_struct.bits_length);

	// ==========初始化 pages 及其内存空间==========
	// 记录本数组的初始地址， 初始化方法： bits_map的结束地址向后对齐
	memory_management_struct.pages_struct = (struct Page *)(((unsigned long)memory_management_struct.bits_map + memory_management_struct.bits_length + PAGE_4K_SIZE - 1) & PAGE_4K_MASK);
	// 记录页的数量
	memory_management_struct.pages_size = TotalMem >> PAGE_2M_SHIFT;
	// 记录page占用的物理空间
	memory_management_struct.pages_length = ((TotalMem >> PAGE_2M_SHIFT) * sizeof(struct Page) + sizeof(long) - 1) & ( ~ (sizeof(long) - 1));
	// 全都清空为0
	memset(memory_management_struct.pages_struct,0x00,memory_management_struct.pages_length);

	// 输出相关信息
	printk("pages_struct : %d, pages_size: %d, pages_length: %d\n", 
	memory_management_struct.pages_struct, memory_management_struct.pages_size, memory_management_struct.pages_length);

	// ==========初始化 zones 及其内存空间==========*/
	// 同样的方法：pages_struct首地址加物理长度
	memory_management_struct.zones_struct = (struct Zone *)(((unsigned long)memory_management_struct.pages_struct + memory_management_struct.pages_length + PAGE_4K_SIZE - 1) & PAGE_4K_MASK);
	// 初始化的时候，还没有zones，zones需要在后期进行统计
	memory_management_struct.zones_size   = 0;
	// 计算zone占用的内存空间，暂时按照五个zone来计算
	memory_management_struct.zones_length = (5 * sizeof(struct Zone) + sizeof(long) - 1) & (~(sizeof(long) - 1));
	// 全部清空为0
	memset(memory_management_struct.zones_struct,0x00,memory_management_struct.zones_length);	//init zones memory
	// 输出相关信息
	printk("zones_struct : %d, zones_size: %d, zones_length: %d\n", 
		memory_management_struct.zones_struct, memory_management_struct.zones_size, memory_management_struct.zones_length);

	// ==========初始化 zones & pages 的内容信息 ==========
	for(int cur_block = 0; cur_block <= memory_management_struct.e820_length; cur_block ++ ){

		// 如果内存块不可用，那么跳过
		if (memory_management_struct.e820[cur_block].type != 1) continue;

		/*===================计算开始、结尾地址===================*/

		// 和上面统计可用页一样， 这里起点是右规后的地址
		unsigned long start = PAGE_2M_ALIGN(memory_management_struct.e820[cur_block].baseAddr);
		// 计算结尾的向前对齐地址
		unsigned long end = (
			(memory_management_struct.e820[cur_block].baseAddr + memory_management_struct.e820[cur_block].length)
			>> PAGE_2M_SHIFT
		) << PAGE_2M_SHIFT; 
		// 如果没有可用空间就跳过
		if (end <= start) continue;
		/*===================初始化zone===================*/
		// 指针指向
		struct Zone* curZone = memory_management_struct.zones_struct + memory_management_struct.zones_size;
		memory_management_struct.zones_size ++;
		curZone->zone_start_address = start;
		curZone->zone_end_address = end;
		curZone->zone_length = end - start;
		curZone->page_using_count = 0;
		curZone->page_free_count = (curZone->zone_length >> PAGE_2M_SHIFT);
		curZone->total_pages_link = 0; // 总引用为0
		curZone->attribute = 0;
		curZone->GMD_struct = &memory_management_struct; // 这里取的是地址，因为前面的是指针
		curZone->pages_length = (curZone->zone_length >> PAGE_2M_SHIFT);
		// 基地址+起点所在页的数量
		curZone->pages_group =  (struct Page *)(memory_management_struct.pages_struct + (start >> PAGE_2M_SHIFT));
		printk("BLOCK ID :: %d, cur zone start page number ::: %d\n", cur_block ,(start >> PAGE_2M_SHIFT) );

		/*===================初始化Pages===================*/
		struct Page* curPage = curZone->pages_group; // 指向最开始的页地址
		
		// 遍历该内存区域中的每一个页， 写完一个页之后， 就接着写下一个页， 直到把当前zone中的页写完
		for(int curPageId = 0; curPageId < curZone->pages_length; curPageId ++, curPage ++){
			
			curPage->zone_struct = curZone; // 将当前的页指向当前的zone
			// 当前page的物理地址就是： 当前zone的物理地址 + 第curPageId * 每一页的大小，其中curPageId从0开始
			curPage->PHY_address = start + PAGE_2M_SIZE * curPageId;
			curPage->attribute = 0;
			curPage->reference_count = 0;
			curPage->age = 0;
			// 把当前struct page结构体所代表的物理地址转换成bits_map映射位图中对应的位。
			// 这里的bits_map之中，每个变量都是一个unsigned long 类型的 64位整数
			// 每一位可以表示该位所对应的page是否使用
			*(memory_management_struct.bits_map + ((curPage->PHY_address >> PAGE_2M_SHIFT) >> 6)) ^= 
				1UL << (curPage->PHY_address >> PAGE_2M_SHIFT) % 64;
		}
	}

	printk("PAGE_SIZE ::: %#010x\n", PAGE_2M_SIZE);

	// ==========对第一段内存进行初始化（第一段比较特殊，包含多个物理内存段所以要特殊处理）==========
	// 将pages_struct的初始地址指向
	memory_management_struct.pages_struct->zone_struct = memory_management_struct.zones_struct;
	memory_management_struct.pages_struct->PHY_address = 0UL;
	set_page_attribute(memory_management_struct.pages_struct,PG_PTable_Maped | PG_Kernel_Init | PG_Kernel);
	memory_management_struct.pages_struct->reference_count = 1;
	memory_management_struct.pages_struct->age = 0;
	memory_management_struct.zones_length = (memory_management_struct.zones_size * sizeof(struct Zone) + sizeof(long) - 1) & ( ~ (sizeof(long) - 1));

	// ==========其他的设置==========
	// 两个不同类型的地址区间，现在付的值暂时没有意义
	ZONE_DMA_INDEX = 0;
	ZONE_NORMAL_INDEX = 0;
	ZONE_UNMAPED_INDEX = 0;

	for(int i = 0;i < memory_management_struct.zones_size;i++) {
		struct Zone * z = memory_management_struct.zones_struct + i;
		printk("zone_start_address:%#018lx,zone_end_address:%#018lx,\n\
		zone_length:%#018lx,pages_group:%#018lx,pages_length:%#018lx\n", \
		z->zone_start_address,z->zone_end_address,z->zone_length,z->pages_group,z->pages_length);
		// 如果起始地址符合条件，那么就将其设置为非映射区间
		if (z->zone_start_address == 0x100000000 && !ZONE_UNMAPED_INDEX)
			ZONE_UNMAPED_INDEX = i;
	}
	
	// 记录向后规格化后的结束地址
	memory_management_struct.end_of_struct = (unsigned long)((unsigned long)
		memory_management_struct.zones_struct + memory_management_struct.zones_length + sizeof(long) * 32
	) & ( ~ (sizeof(long) - 1));

	// ==========输出提示信息以及将前面的页设置为 正在使用+经过页表映射的页+内核初始化程序+内核层页==========

	printk("start_code:%#018lx,end_code:%#018lx,end_data:%#018lx, end_brk:%#018lx,end_of_struct:%#018lx\n",memory_management_struct.start_code, memory_management_struct.end_code,memory_management_struct.end_data,memory_management_struct.end_brk, memory_management_struct.end_of_struct);
	// 获取结构体所在页
	int curPageId = CONVERT_VIRTUAL_ADDRESS_TO_PHYSICAL_ADDRESS(memory_management_struct.end_of_struct) >> PAGE_2M_SHIFT;
	
	// 对前面的页进行初始化
	for(int pageId = 0;pageId <= curPageId;pageId++) {
		struct Page * tmp_page =  memory_management_struct.pages_struct + pageId;
		PageInit(tmp_page,PG_PTable_Maped | PG_Kernel_Init | PG_Active | PG_Kernel);
		*(memory_management_struct.bits_map + ((tmp_page->PHY_address >> PAGE_2M_SHIFT) >> 6)) |= 1UL << (tmp_page->PHY_address >> PAGE_2M_SHIFT) % 64;
		tmp_page->zone_struct->page_using_count++;
		tmp_page->zone_struct->page_free_count--;
	}
	

	Global_CR3 = GetGDT();

	printk("Global_CR3\t:%#018lx\n",Global_CR3);
	printk("*Global_CR3\t:%#018lx\n",*CONVERT_PHYSICAL_ADDRESS_TO_VIRTUAL_ADDRESS(Global_CR3) & (~0xff));
	printk("**Global_CR3\t:%#018lx\n",*CONVERT_PHYSICAL_ADDRESS_TO_VIRTUAL_ADDRESS(*CONVERT_PHYSICAL_ADDRESS_TO_VIRTUAL_ADDRESS (Global_CR3) & (~0xff)) & (~0xff));

	for(int i = 0; i < 10; ++i)
		*(CONVERT_PHYSICAL_ADDRESS_TO_VIRTUAL_ADDRESS(Global_CR3)  + i) = 0UL;

	FLUSH_TLB();
}

/**
 * 根据给出的需求，返回一个初始化后的page列表的起始地址
 * @param zone_select 需求的页的类型
 * @param number 需求的页的数量
 * @param page_flags 需求的状态
 */
struct Page* AllocPages(int zone_select,int number, unsigned long page_flags){
	int begin_zone, end_zone;
	unsigned long attribute;
	

	// 请求的页的数量不同
	if (number >= 64 || number <= 0) {
		printk("alloc_pages() ERROR: number is invalid\n");
		return NULL;
	}

	// 根据zone_select选择开始的zone
	switch (zone_select){
		case ZONE_DMA:
			begin_zone = 0;
			end_zone = ZONE_DMA_INDEX;
			attribute = PG_PTable_Maped;
			break;
		case ZONE_NORMAL:
			begin_zone = ZONE_DMA_INDEX;
			end_zone = ZONE_NORMAL_INDEX;
			attribute = PG_PTable_Maped;
			break;
		case ZONE_UNMAPED:
			begin_zone = ZONE_UNMAPED_INDEX;
			end_zone = memory_management_struct.zones_size - 1;
			attribute = 0;
			break;
		default:
			printk("Malloc ERROR ! NO SUCH KIND OF MEMORY!");
			return NULL;
			break;
	}

	// 遍历每一个zone进行检索
	for(int cur_zone_id = begin_zone; cur_zone_id <= end_zone; cur_zone_id ++){
		struct Zone* cur_zone = memory_management_struct.zones_struct + cur_zone_id; // 获取当前zone
		// 如果当前的Zone中，没有足够的页，那么就找下一个
		if (cur_zone->page_free_count < number) continue;
		unsigned long start = (cur_zone->zone_start_address >> PAGE_2M_SHIFT); // 开始的页号
		unsigned long end = (cur_zone->zone_end_address >> PAGE_2M_SHIFT); // 结束的页号 
		unsigned long cur_jump_width = 64 - (start % 64); // 当前想要走到64位对齐的下一个位置需要走多远
		// 顺序检查
		for(int cur_page_id = start; cur_page_id < end; cur_page_id += (cur_page_id % 64 == 0) ? 64 : cur_jump_width ){
			unsigned long *cur_bits_map = memory_management_struct.bits_map + (cur_page_id >> 6); // 将指针指向当前page所在的bitsmap元素
			unsigned long shift_index = (cur_page_id % 64);// 确定当前bitsmap元素的哪一位描述当前页
			for(int check_page_id = shift_index; check_page_id < 64 - shift_index; check_page_id ++){
				if ( !(((*cur_bits_map >> check_page_id) |(*(cur_bits_map + 1) << (64 - check_page_id))) &
				(number == 64 ? 0xffffffffffffffffUL : ((1UL << number) - 1))) ){
					// 如果当前bitsmap元素中，0 ~ number 的没有被使用，那么就将其分配
					unsigned long  st_page;
                    st_page = cur_page_id + check_page_id - 1;
					// 循环初始化
                    for(int init_page_id = 0; init_page_id < number; init_page_id ++){
                        struct Page* init_page_ptr = memory_management_struct.pages_struct + st_page + init_page_id;
                        PageInit(init_page_ptr,page_flags);
						*(memory_management_struct.bits_map + ((init_page_ptr->PHY_address >> PAGE_2M_SHIFT) >> 6)) |= 1UL << (init_page_ptr->PHY_address >> PAGE_2M_SHIFT) % 64;
						cur_zone->page_using_count++;
						cur_zone->page_free_count--;
						init_page_ptr->attribute = attribute;
                    }
					// 找到了就返回页
					return (struct Page *)(memory_management_struct.pages_struct + st_page);
				}
			}
		}
	}
	// 没找到就返回空 
	return NULL;
}

/**
 * 初始化页表
 */
void PageTableInit(){

	Global_CR3 = GetGDT();
	unsigned long* temp;

	// 输出页表的信息
	temp = (unsigned long *)(((unsigned long)CONVERT_PHYSICAL_ADDRESS_TO_VIRTUAL_ADDRESS((unsigned long)Global_CR3 & (~ 0xfffUL))) + 8 * 256);
	printk("1 : %#018lx, %#018lx \n", (unsigned long)temp, *temp);
	temp = CONVERT_PHYSICAL_ADDRESS_TO_VIRTUAL_ADDRESS(*temp);
	printk("2 : %#018lx, %#018lx \n", (unsigned long)temp, *temp);
	temp = CONVERT_PHYSICAL_ADDRESS_TO_VIRTUAL_ADDRESS(*temp);
	printk("3 : %#018lx, %#018lx \n", (unsigned long)temp, *temp);

	// 遍历每一个页，对其进行映射
	for(int i = 0; i < memory_management_struct.zones_size; ++i){
		printk("in loop\n");
		struct Zone* cur_zone = memory_management_struct.zones_struct + i;
		struct Page* cur_page = cur_zone->pages_group;

		if(ZONE_UNMAPED_INDEX && i == ZONE_UNMAPED_INDEX)
			break;
		
		for(int j = 0; j < cur_zone->pages_length; ++j, ++cur_page){
			temp = (unsigned long *)(((unsigned long)CONVERT_PHYSICAL_ADDRESS_TO_VIRTUAL_ADDRESS((unsigned long)Global_CR3 & (~ 0xfffUL))) + (((unsigned long)CONVERT_PHYSICAL_ADDRESS_TO_VIRTUAL_ADDRESS(cur_page->PHY_address) >> PAGE_GDT_SHIFT) & 0x1ff) * 8);
			
			if(*temp == 0)
			{			
				unsigned long * virtual = kmalloc(PAGE_4K_SIZE,0);
				set_mpl4t(temp,mk_mpl4t(CONVERT_VIRTUAL_ADDRESS_TO_PHYSICAL_ADDRESS(virtual),PAGE_KERNEL_GDT));
			}

			temp = (unsigned long *)((unsigned long)CONVERT_PHYSICAL_ADDRESS_TO_VIRTUAL_ADDRESS(*temp & (~ 0xfffUL)) + (((unsigned long)CONVERT_PHYSICAL_ADDRESS_TO_VIRTUAL_ADDRESS(cur_page->PHY_address) >> PAGE_1G_SHIFT) & 0x1ff) * 8);
			
			if(*temp == 0)
			{
				unsigned long * virtual = kmalloc(PAGE_4K_SIZE,0);
				set_pdpt(temp,mk_pdpt(CONVERT_VIRTUAL_ADDRESS_TO_PHYSICAL_ADDRESS(virtual),PAGE_KERNEL_Dir));
			}

			temp = (unsigned long *)((unsigned long)CONVERT_PHYSICAL_ADDRESS_TO_VIRTUAL_ADDRESS(*temp & (~ 0xfffUL)) + (((unsigned long)CONVERT_PHYSICAL_ADDRESS_TO_VIRTUAL_ADDRESS(cur_page->PHY_address) >> PAGE_2M_SHIFT) & 0x1ff) * 8);

			set_pdt(temp,mk_pdt(cur_page->PHY_address,PAGE_KERNEL_Page));

			if(j % 50 == 0)
				color_printk(GREEN,BLACK,"@:%#018lx,%#018lx\t\n",(unsigned long)temp,*temp);
		}
	}
	FLUSH_TLB();
}

/**
 * 显示区域初始化
 */
void FrameBufferInit() {
    unsigned long * tmp;
    unsigned long * tmp1;
    unsigned int * FB_addr = (unsigned int *)CONVERT_PHYSICAL_ADDRESS_TO_VIRTUAL_ADDRESS(0xe0000000);

    Global_CR3 = GetGDT();

    tmp = CONVERT_PHYSICAL_ADDRESS_TO_VIRTUAL_ADDRESS((unsigned long *)((unsigned long)Global_CR3 & (~ 0xfffUL)) + (((unsigned long)FB_addr >> PAGE_GDT_SHIFT) & 0x1ff));
    if (*tmp == 0) {
        unsigned long * virtual = kmalloc(PAGE_4K_SIZE,0);
        set_mpl4t(tmp,mk_mpl4t(CONVERT_VIRTUAL_ADDRESS_TO_PHYSICAL_ADDRESS(virtual),PAGE_KERNEL_GDT));
		
    }

    tmp = CONVERT_PHYSICAL_ADDRESS_TO_VIRTUAL_ADDRESS((unsigned long *)(*tmp & (~ 0xfffUL)) + (((unsigned long)FB_addr >> PAGE_1G_SHIFT) & 0x1ff));
    if (*tmp == 0) {
        unsigned long * virtual = kmalloc(PAGE_4K_SIZE,0);
        set_pdpt(tmp,mk_pdpt(CONVERT_VIRTUAL_ADDRESS_TO_PHYSICAL_ADDRESS(virtual),PAGE_KERNEL_Dir));
    }

    for(int i = 0;i < global_position.screen_buffer_length;i += PAGE_2M_SIZE) {
        tmp1 = CONVERT_PHYSICAL_ADDRESS_TO_VIRTUAL_ADDRESS((unsigned long *)(*tmp & (~ 0xfffUL)) + (((unsigned long)((unsigned long)FB_addr + i) >> PAGE_2M_SHIFT) & 0x1ff));
        unsigned long phy = 0xe0000000 + i;
        set_pdt(tmp1,mk_pdt(phy,PAGE_KERNEL_Page | PAGE_PWT | PAGE_PCD));
    }
	
    global_position.screen_buffer_base_address = (unsigned int *)CONVERT_PHYSICAL_ADDRESS_TO_VIRTUAL_ADDRESS(0xe0000000);

    FLUSH_TLB();
}