#ifndef MEMORY_H
#define MEMORY_H

#include "address_pool.h"
#include "list.h"

#define  DESC_CNT 7


enum AddressPoolType
{
    USER,
    KERNEL
};




//内存块
typedef struct mem_block{
    ListItem free_elem;
}mem_block;

//内存块描述符
typedef struct mem_block_desc{
    uint32 block_size;	//内存块大小
    uint32 block_per_arena;		//本arena中可容纳此mem_block的数量
    List free_list;				//目前可用的mem_block链表
}mem_block_desc;

//内存仓库
typedef struct arena{
    mem_block_desc* desc;		//此arena关联的mem_block_desc
    //large 为 true时 cnt是页框数，否则表示mem_block的数量
    uint32 cnt;
    bool large;
}arena;




//内存描述符初始化
void initialize_block_desc(mem_block_desc* desc_array);
//返回arena中第idx个内存块的地址
mem_block* arena2block(arena* a, int idx);


void* sys_malloc(uint32 size);

void sys_free(void* ptr);


class MemoryManager
{
public:
    // 可管理的内存容量
    int totalMemory;
    // 内核物理地址池
    AddressPool kernelPhysical;
    // 用户物理地址池
    AddressPool userPhysical;
    // 内核虚拟地址池
    AddressPool kernelVirtual;

    //内核内存块描述符组
    mem_block_desc k_block_descs[DESC_CNT];

public:
    MemoryManager();

    // 初始化地址池
    void initialize();

    // 从type类型的物理地址池中分配count个连续的页
    // 成功，返回起始地址；失败，返回0
    int allocatePhysicalPages(enum AddressPoolType type, const int count);

    // 释放从paddr开始的count个物理页
    void releasePhysicalPages(enum AddressPoolType type, const int startAddress, const int count);

    // 获取内存总容量
    int getTotalMemory();

    // 开启分页机制
    void openPageMechanism();

    // 页内存分配
    int allocatePages(enum AddressPoolType type, const int count);

    // 虚拟页分配
    int allocateVirtualPages(enum AddressPoolType type, const int count);

    // 建立虚拟页到物理页的联系
    bool connectPhysicalVirtualPage(const int virtualAddress, const int physicalPageAddress);

    // 计算virtualAddress的页目录项的虚拟地址
    int toPDE(const int virtualAddress);

    // 计算virtualAddress的页表项的虚拟地址
    int toPTE(const int virtualAddress);

    // 页内存释放
    void releasePages(enum AddressPoolType type, const int virtualAddress, const int count);    

    // 找到虚拟地址对应的物理地址
    int vaddr2paddr(int vaddr);

    // 释放虚拟页
    void releaseVirtualPages(enum AddressPoolType type, const int vaddr, const int count);
};

#endif