/*
 * (C) 2022, Cornell University
 * All rights reserved.
 */

/* Author: Yunhao Zhang
 * Description: memory management unit (MMU)
 * implementation of 2 translation mechanisms: page table and software TLB
 */

#include "egos.h"
#include "disk.h"
#include "servers.h"
#include <string.h>

/* Interface of the paging device, see earth/dev_page.c */
void  paging_init();
int   paging_invalidate_cache(int frame_id);
int   paging_write(int frame_id, int page_no);
char* paging_read(int frame_id, int alloc_only);

/* Allocation and free of physical frames */
#define NFRAMES 256
struct frame_mapping {
    int use;     /* Is the frame allocated? */
    int pid;     /* Which process owns the frame? */
    int page_no; /* Which virtual page is the frame mapped to? */
} table[NFRAMES];

int mmu_alloc(int* frame_id, void** cached_addr) {
    for (int i = 0; i < NFRAMES; i++)
        if (!table[i].use) {
            *frame_id = i;
            *cached_addr = paging_read(i, 1);
            table[i].use = 1;
            return 0;
        }
    FATAL("mmu_alloc: no more available frames");
}

int mmu_free(int pid) {
    for (int i = 0; i < NFRAMES; i++)
        if (table[i].use && table[i].pid == pid) {
            paging_invalidate_cache(i);
            memset(&table[i], 0, sizeof(struct frame_mapping));
        }
}

/* Software TLB Translation */
int soft_tlb_map(int pid, int page_no, int frame_id) {
    table[frame_id].pid = pid;
    table[frame_id].page_no = page_no;
}

int soft_tlb_switch(int pid) {
    static int curr_vm_pid = -1;
    if (pid == curr_vm_pid) return 0;

    /* Unmap curr_vm_pid from the user address space */
    for (int i = 0; i < NFRAMES; i++)
        if (table[i].use && table[i].pid == curr_vm_pid)
            paging_write(i, table[i].page_no);

    /* Map pid to the user address space */
    for (int i = 0; i < NFRAMES; i++)
        if (table[i].use && table[i].pid == pid)
            memcpy((void*)(table[i].page_no << 12), paging_read(i, 0), PAGE_SIZE);

    curr_vm_pid = pid;
}

/* Page Table Translation
 *
 * The code below creates an identity mapping using RISC-V Sv32;
 * Read section4.3 of RISC-V manual (references/riscv-privileged-v1.10.pdf)
 *
 * mmu_map() and mmu_switch() using page tables is given to students
 * as a course project. After this project, every process should have
 * its own set of page tables. mmu_map() will modify entries in these
 * tables and mmu_switch() will modify satp (page table base register)
 */

#define OS_RWX   0xF
#define USER_RWX 0x1F
static unsigned int frame_id, *root, *leaf;

/* 32 is a number large enough for demo purpose */
static unsigned int* pid_to_pagetable_base[32];

void setup_identity_region(int pid, unsigned int addr, int npages, int flag) {
    int vpn1 = addr >> 22;

    if (root[vpn1] & 0x1) {
        // Leaf has been allocated
        leaf = (void*)((root[vpn1] << 2) & 0xFFFFF000);
    } else {
        // Leaf has not been allocated
        earth->mmu_alloc(&frame_id, (void**)&leaf);
        table[frame_id].pid = pid;
        memset(leaf, 0, PAGE_SIZE);
        root[vpn1] = ((unsigned int)leaf >> 2) | 0x1;
    }

    /* Setup the entries in the leaf page table */
    int vpn0 = (addr >> 12) & 0x3FF;
    for (int i = 0; i < npages; i++)
        leaf[vpn0 + i] = ((addr + i * PAGE_SIZE) >> 2) | flag;
}

void pagetable_identity_mapping(int pid) {
    /* Allocate the root page table and set the page table base (satp) */
    earth->mmu_alloc(&frame_id, (void**)&root);
    table[frame_id].pid = pid;
    memset(root, 0, PAGE_SIZE);
    pid_to_pagetable_base[pid] = root;

    /* Allocate the leaf page tables */
    setup_identity_region(pid, 0x02000000, 16, OS_RWX);   /* CLINT */
    setup_identity_region(pid, 0x10013000, 1, OS_RWX);    /* UART0 */
    setup_identity_region(pid, 0x10024000, 1, OS_RWX);    /* SPI1 */
    setup_identity_region(pid, 0x20400000, 1024, OS_RWX); /* boot ROM */
    setup_identity_region(pid, 0x20800000, 1024, OS_RWX); /* disk image */
    setup_identity_region(pid, 0x80000000, 1024, OS_RWX); /* DTIM memory */

    for (int i = 0; i < 8; i++)           /* ITIM memory is 32MB on QEMU */
        setup_identity_region(pid, 0x08000000 + i * 0x400000, 1024, OS_RWX);
}

int page_table_map(int pid, int page_no, int frame_id) {
    if (pid >= 32) FATAL("page_table_map: pid too large");

    /* Student's code goes here (page table translation). */

    /* Remove the following line of code and, instead,
     * (1) if page tables for pid do not exist, build the tables;
     * (2) if page tables for pid exist, update entries of the tables
     *
     * Feel free to call or modify the two helper functions:
     * pagetable_identity_mapping() and setup_identity_region()
     */
    soft_tlb_map(pid, page_no, frame_id);

    /* Student's code ends here. */
}

int page_table_switch(int pid) {
    /* Student's code goes here (page table translation). */

    /* Remove the following line of code and, instead,
     * modify the page table base register (satp) similar to
     * the code in mmu_init(); Remember to use the pid argument
     */
    soft_tlb_switch(pid);

    /* Student's code ends here. */
}

// inline int
// pmpcfg(int loc, int L, int A, int X, int W, int R) {
//     return ((L << 7) | (A << 3) | (X << 2) | (W << 1) | (R)) << (loc << 3);
// }

#define PMPCFG(LOC, L, A, X, W, R) (((L << 7) | (A << 3) | (X << 2) | (W << 1) | (R)) << (LOC << 3))

/* MMU Initialization */
void mmu_init() {
    /* Initialize the paging device */
    paging_init();

    /* Initialize MMU interface functions */
    earth->mmu_free = mmu_free;
    earth->mmu_alloc = mmu_alloc;

    /* Setup a PMP region for the whole 4GB address space */
    asm("csrw pmpaddr0, %0" : : "r" (0x40000000));
    asm("csrw pmpcfg0, %0" : : "r" (0xF));

    /* Student's code goes here (PMP memory protection). */
    int oricfg;
    /* Setup PMP TOR region 0x00000000 - 0x20000000 as r/w/x */
    // asm("csrw pmpaddr1, %0"
    //     :
    //     : "r"(0x00000000 >> 2));
    asm("csrw pmpaddr0, %0"
        :
        : "r"(0x20000000 >> 2));
    // int cfg1 = PMPCFG(1, 1, 0, 1, 1, 1);
    int cfg0 = PMPCFG(0, 1, 1, 1, 1, 1);

    /* Setup PMP NAPOT region 0x20400000 - 0x20800000 as r/-/x */
    asm("csrw pmpaddr1, %0"
        :
        : "r"((0x20400000 >> 2) | ((1 << 19) - 1)));
    int cfg1 = PMPCFG(1, 1, 3, 1, 0, 1);

    /* Setup PMP NAPOT region 0x20800000 - 0x20C00000 as r/-/- */
    asm("csrw pmpaddr2, %0"
        :
        : "r"((0x20800000 >> 2) | ((1 << 19) - 1)));
    int cfg2 = PMPCFG(2, 1, 3, 0, 0, 1);

    /* Setup PMP NAPOT region 0x80000000 - 0x80004000 as r/w/- */
    asm("csrw pmpaddr3, %0"
        :
        : "r"((0x80000000 >> 2) | ((1 << 11) - 1)));
    int cfg3 = PMPCFG(3, 1, 3, 0, 1, 1);

    asm("csrr %0, pmpcfg0"
        : "=r"(oricfg));
    asm("csrw pmpcfg0, %0"
        :
        : "r"(oricfg | cfg0 | cfg1 | cfg2 | cfg3));

    /* Student's code ends here. */

    /* Arty board does not support supervisor mode or page tables */
    earth->translation = SOFT_TLB;
    earth->mmu_map = soft_tlb_map;
    earth->mmu_switch = soft_tlb_switch;
    if (earth->platform == ARTY) return;

    /* Choose memory translation mechanism in QEMU */
    CRITICAL("Choose a memory translation mechanism:");
    printf("Enter 0: page tables\r\nEnter 1: software TLB\r\n");

    char buf[2];
    for (buf[0] = 0; buf[0] != '0' && buf[0] != '1'; earth->tty_read(buf, 2));
    earth->translation = (buf[0] == '0') ? PAGE_TABLE : SOFT_TLB;
    INFO("%s translation is chosen", earth->translation == PAGE_TABLE ? "Page table" : "Software");

    if (earth->translation == PAGE_TABLE) {
        /* Setup an identity mapping using page tables */
        pagetable_identity_mapping(0);
        asm("csrw satp, %0" ::"r"(((unsigned int)root >> 12) | (1 << 31)));

        earth->mmu_map = page_table_map;
        earth->mmu_switch = page_table_switch;
    }
}
