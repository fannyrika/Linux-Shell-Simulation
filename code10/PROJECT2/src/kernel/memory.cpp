#include "memory.h"
#include "os_constant.h"
#include "stdlib.h"
#include "asm_utils.h"
#include "stdio.h"
#include "program.h"
#include "os_modules.h"
#include "sync.h"
MemoryManager::MemoryManager()
{
    initialize();
}

SpinLock mem_lock;

//内存描述符初始化
void initialize_block_desc(mem_block_desc* desc_array){
    int index;
    int block_size = 16;
    
    for(index = 0; index < DESC_CNT; index++){
        desc_array[index].block_size = block_size;
        desc_array[index].block_per_arena = (PAGE_SIZE - sizeof(arena)) / block_size;
        desc_array[index].free_list.initialize();
        block_size *= 2;
    }
}


void MemoryManager::initialize()
{
    mem_lock.initialize();
    this->totalMemory = 0;
    this->totalMemory = getTotalMemory();

    // 预留的内存
    int usedMemory = 256 * PAGE_SIZE + 0x100000;
    if (this->totalMemory < usedMemory)
    {
        printf("memory is too small, halt.\n");
        asm_halt();
    }
    // 剩余的空闲的内存
    int freeMemory = this->totalMemory - usedMemory;

    int freePages = freeMemory / PAGE_SIZE;
    int kernelPages = freePages / 2;
    int userPages = freePages - kernelPages;

    int kernelPhysicalStartAddress = usedMemory;
    int userPhysicalStartAddress = usedMemory + kernelPages * PAGE_SIZE;

    int kernelPhysicalBitMapStart = BITMAP_START_ADDRESS;
    int userPhysicalBitMapStart = kernelPhysicalBitMapStart + ceil(kernelPages, 8);
    int kernelVirtualBitMapStart = userPhysicalBitMapStart + ceil(userPages, 8);

    kernelPhysical.initialize(
        (char *)kernelPhysicalBitMapStart,
        kernelPages,
        kernelPhysicalStartAddress);

    userPhysical.initialize(
        (char *)userPhysicalBitMapStart,
        userPages,
        userPhysicalStartAddress);

    kernelVirtual.initialize(
        (char *)kernelVirtualBitMapStart,
        kernelPages,
        KERNEL_VIRTUAL_START);

    printf("total memory: %d bytes ( %d MB )\n",
           this->totalMemory,
           this->totalMemory / 1024 / 1024);

    printf("kernel pool\n"
           "    start address: 0x%x\n"
           "    total pages: %d ( %d MB )\n"
           "    bitmap start address: 0x%x\n",
           kernelPhysicalStartAddress,
           kernelPages, kernelPages * PAGE_SIZE / 1024 / 1024,
           kernelPhysicalBitMapStart);

    printf("user pool\n"
           "    start address: 0x%x\n"
           "    total pages: %d ( %d MB )\n"
           "    bit map start address: 0x%x\n",
           userPhysicalStartAddress,
           userPages, userPages * PAGE_SIZE / 1024 / 1024,
           userPhysicalBitMapStart);

    printf("kernel virtual pool\n"
           "    start address: 0x%x\n"
           "    total pages: %d  ( %d MB ) \n"
           "    bit map start address: 0x%x\n",
           KERNEL_VIRTUAL_START,
           userPages, kernelPages * PAGE_SIZE / 1024 / 1024,
           kernelVirtualBitMapStart);

    //初始化内核内存描述符
    initialize_block_desc(k_block_descs);
}

int MemoryManager::allocatePhysicalPages(enum AddressPoolType type, const int count)
{
    int start = -1;

    if (type == AddressPoolType::KERNEL)
    {
        start = kernelPhysical.allocate(count);
    }
    else if (type == AddressPoolType::USER)
    {
        start = userPhysical.allocate(count);
    }

    return (start == -1) ? 0 : start;
}

void MemoryManager::releasePhysicalPages(enum AddressPoolType type, const int paddr, const int count)
{
    if (type == AddressPoolType::KERNEL)
    {
        kernelPhysical.release(paddr, count);
    }
    else if (type == AddressPoolType::USER)
    {

        userPhysical.release(paddr, count);
    }
}

int MemoryManager::getTotalMemory()
{

    if (!this->totalMemory)
    {
        int memory = *((int *)MEMORY_SIZE_ADDRESS);
        // ax寄存器保存的内容
        int low = memory & 0xffff;
        // bx寄存器保存的内容
        int high = (memory >> 16) & 0xffff;

        this->totalMemory = low * 1024 + high * 64 * 1024;
    }

    return this->totalMemory;
}

int MemoryManager::allocatePages(enum AddressPoolType type, const int count)
{
    // 第一步：从虚拟地址池中分配若干虚拟页
    int virtualAddress = allocateVirtualPages(type, count);
    if (!virtualAddress)
    {
        return 0;
    }

    bool flag;
    int physicalPageAddress;
    int vaddress = virtualAddress;

    // 依次为每一个虚拟页指定物理页
    for (int i = 0; i < count; ++i, vaddress += PAGE_SIZE)
    {
        flag = false;
        // 第二步：从物理地址池中分配一个物理页
        physicalPageAddress = allocatePhysicalPages(type, 1);
        if (physicalPageAddress)
        {
            //printf("allocate physical page 0x%x\n", physicalPageAddress);

            // 第三步：为虚拟页建立页目录项和页表项，使虚拟页内的地址经过分页机制变换到物理页内。
            flag = connectPhysicalVirtualPage(vaddress, physicalPageAddress);
        }
        else
        {
            flag = false;
        }

        // 分配失败，释放前面已经分配的虚拟页和物理页表
        if (!flag)
        {
            // 前i个页表已经指定了物理页
            releasePages(type, virtualAddress, i);
            // 剩余的页表未指定物理页
            releaseVirtualPages(type, virtualAddress + i * PAGE_SIZE, count - i);
            return 0;
        }
    }

    return virtualAddress;
}

int MemoryManager::allocateVirtualPages(enum AddressPoolType type, const int count)
{
    int start = -1;

    if (type == AddressPoolType::KERNEL)
    {
        start = kernelVirtual.allocate(count);
    }
    else if (type == AddressPoolType::USER)
    {
        start = programManager.running->userVirtual.allocate(count);
    }

    return (start == -1) ? 0 : start;
}

bool MemoryManager::connectPhysicalVirtualPage(const int virtualAddress, const int physicalPageAddress)
{
    // 计算虚拟地址对应的页目录项和页表项
    int *pde = (int *)toPDE(virtualAddress);
    int *pte = (int *)toPTE(virtualAddress);

    // 页目录项无对应的页表，先分配一个页表
    if (!(*pde & 0x00000001))
    {
        // 从内核物理地址空间中分配一个页表
        int page = allocatePhysicalPages(AddressPoolType::KERNEL, 1);
        if (!page)
            return false;

        // 使页目录项指向页表
        *pde = page | 0x7;
        // 初始化页表
        char *pagePtr = (char *)(((int)pte) & 0xfffff000);
        memset(pagePtr, 0, PAGE_SIZE);
    }

    // 使页表项指向物理页
    *pte = physicalPageAddress | 0x7;

    return true;
}

int MemoryManager::toPDE(const int virtualAddress)
{
    return (0xfffff000 + (((virtualAddress & 0xffc00000) >> 22) * 4));
}

int MemoryManager::toPTE(const int virtualAddress)
{
    return (0xffc00000 + ((virtualAddress & 0xffc00000) >> 10) + (((virtualAddress & 0x003ff000) >> 12) * 4));
}

void MemoryManager::releasePages(enum AddressPoolType type, const int virtualAddress, const int count)
{
    int vaddr = virtualAddress;
    int *pte;
    for (int i = 0; i < count; ++i, vaddr += PAGE_SIZE)
    {
        // 第一步，对每一个虚拟页，释放为其分配的物理页
        releasePhysicalPages(type, vaddr2paddr(vaddr), 1);

        // 设置页表项为不存在，防止释放后被再次使用
        pte = (int *)toPTE(vaddr);
        *pte = 0;
    }

    // 第二步，释放虚拟页
    releaseVirtualPages(type, virtualAddress, count);
}

int MemoryManager::vaddr2paddr(int vaddr)
{
    int *pte = (int *)toPTE(vaddr);
    int page = (*pte) & 0xfffff000;
    int offset = vaddr & 0xfff;
    return (page + offset);
}

void MemoryManager::releaseVirtualPages(enum AddressPoolType type, const int vaddr, const int count)
{
    if (type == AddressPoolType::KERNEL)
    {
        kernelVirtual.release(vaddr, count);
    }
    else if (type == AddressPoolType::USER)
    {
        programManager.running->userVirtual.release(vaddr, count);
    }
}


//返回arena中第idx个内存块的地址
mem_block* arena2block(arena* a, int idx){
    return (mem_block*)((uint32)a + sizeof(arena) + idx * a->desc->block_size);
}


//在堆中申请size字节的内存
void* sys_malloc(uint32 size){
    
    mem_block_desc* descs;
    enum AddressPoolType type;
        
    //判断当前正在执行线程的模式
  	//如果是内核线程
    if(!programManager.running->pageDirectoryAddress){
        type = AddressPoolType::KERNEL;
        descs = memoryManager.k_block_descs;
        
    }else{
        type = AddressPoolType::USER;
        descs = programManager.running->u_bloc_sesc;
    }
    
    
    arena* a;
    mem_block* b;
    
    //lock
    mem_lock.lock();
    /* 超过最大内存块1024, 就分配页框 */
    if (size > 1024) {
        uint32 page_cnt =  (size + sizeof(arena) + PAGE_SIZE - 1) / PAGE_SIZE;    // 向上取整需要的页框数

        a = (arena*)memoryManager.allocatePages(type, page_cnt);

        if (a != nullptr) {
            memset(a, 0, page_cnt * PAGE_SIZE);	 // 将分配的内存清0  

            /* 对于分配的大块页框,将desc置为nullptr, cnt置为页框数,large置为true */
            a->desc = nullptr;
            a->cnt = page_cnt;
            a->large = true;
            //unlock
            mem_lock.unlock();
            return (void*)(a + 1);		 // 跨过arena大小，把剩下的内存返回
        } else { 
            //unlock
            mem_lock.unlock();
            return nullptr; 
        }
    }else{  // 若申请的内存小于等于1024,可在各种规格的mem_block_desc中去适配
        uint8 desc_idx;

        /* 从内存块描述符中匹配合适的内存块规格 */
        for (desc_idx = 0; desc_idx < DESC_CNT; desc_idx++) {
            if (size <= descs[desc_idx].block_size) {  // 从小往大后,找到后退出
                break;
            }
        }

        /* 若mem_block_desc的free_list中已经没有可用的mem_block,
         * 就创建新的arena提供mem_block */
        if (descs[desc_idx].free_list.empty()) {
            a = (arena* )memoryManager.allocatePages(type, 1);       // 分配1页框做为arena
            if (a == nullptr) {
                //unlock
                mem_lock.unlock();
                return nullptr;
            }
            memset(a, 0, PAGE_SIZE);

            /* 对于分配的小块内存,将desc置为相应内存块描述符, 
             * cnt置为此arena可用的内存块数,large置为false */
            a->desc = &descs[desc_idx];
            a->large = false;
            a->cnt = descs[desc_idx].block_per_arena;
            uint32 block_idx;

            //enum intr_status old_status = intr_disable();
			// 关中断
            bool status = interruptManager.getInterruptStatus();
            interruptManager.disableInterrupt();
            
            /* 开始将arena拆分成内存块,并添加到内存块描述符的free_list中 */
            for (block_idx = 0; block_idx < descs[desc_idx].block_per_arena; block_idx++) {
                b = arena2block(a, block_idx);
                a->desc->free_list.push_back(&b->free_elem);
            }
            // 恢复中断
   			interruptManager.setInterruptStatus(status);

    	}
        /* 开始分配内存块 */
        
        b = (mem_block* )descs[desc_idx].free_list.front();
        memset(b, 0, descs[desc_idx].block_size);

        a = (arena*)((uint32)b & 0xfffff000);  // 获取内存块b所在的arena
        a->cnt--;		   // 将此arena中的空闲内存块数减1
        //unlock
        mem_lock.unlock();
        return (void*)b;
    
    }   
}


void sys_free(void* ptr){
    if(ptr == nullptr){
        printf("Segment error!\n");
    }

    enum AddressPoolType type;
    if(!programManager.running->pageDirectoryAddress){
        type = AddressPoolType::KERNEL;

    }else{
        type = AddressPoolType::USER;
    }

    mem_lock.lock();
    mem_block* b = (mem_block*)ptr;
    arena* a = (arena*)((uint32)b & 0xfffff000);

    if(a->large && a->desc == nullptr){
        memoryManager.releasePages(type, (int)a, a->cnt);
    }else{
        a->desc->free_list.push_back(&b->free_elem);
        a->cnt++;

        if(a->cnt == a->desc->block_per_arena){
            uint32 index;
            for(index = 0; index < a->desc->block_per_arena; index++){
                b = arena2block(a, index);
                a->desc->free_list.erase(&b->free_elem);
            }
            memoryManager.releasePages(type, (int)a, 1);
        }

        mem_lock.unlock();
    }
}