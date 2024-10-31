//
// Created by 甘尧 on 2024/6/22.
//

#include <fcntl.h>
#include <link.h>
#include <sys/mman.h>
#include "base/common_funcs.h"
#include "base/logging.h"
#include "loader.h"

namespace swift::linux {

#define MAX_PHNUM 12

static int ProtFromPhdr(const ElfW(Phdr) * phdr) {
    int prot = 0;
    if (phdr->p_flags & PF_R) prot |= PROT_READ;
    if (phdr->p_flags & PF_W) prot |= PROT_WRITE;
    if (phdr->p_flags & PF_X) prot |= PROT_EXEC;
    return prot;
}

/*
 * Handle the "bss" portion of a segment, where the memory size
 * exceeds the file size and we zero-fill the difference.  For any
 * whole pages in this region, we over-map anonymous pages.  For the
 * sub-page remainder, we zero-fill bytes directly.
 */
static void HandleBss(const ElfW(Phdr) * ph, ElfW(Addr) load_bias, size_t pagesize) {
    if (ph->p_memsz > ph->p_filesz) {
        ElfW(Addr) file_end = ph->p_vaddr + load_bias + ph->p_filesz;
        ElfW(Addr) file_page_end = RoundUp(file_end, pagesize);
        ElfW(Addr) page_end = RoundUp(ph->p_vaddr + load_bias + ph->p_memsz, pagesize);
        if (page_end > file_page_end) {
            auto mmap_res = mmap(reinterpret_cast<void*>(file_page_end),
                                 page_end - file_page_end,
                                 ProtFromPhdr(ph),
                                 MAP_ANON | MAP_PRIVATE | MAP_FIXED,
                                 -1,
                                 0);
            if (mmap_res == MAP_FAILED) PANIC("Map bss segment failed!");
        }
        if (file_page_end > file_end && (ph->p_flags & PF_W)) {
            size_t len = file_page_end - file_end;
            bzero((void*)file_end, len);
        }
    }
}

struct LoadInfo {
    char* elf_path{};
    ElfW(Addr) addr{};
    ElfW(Addr) base{};
    ElfW(Addr) phdr{};
    ElfW(Addr) phnum{};
    Elf64_Half machine{};
    char* interp_path{};
    u8* main_stack_bottom{};
};

/*
 * Open an ELF file and load it into memory.
 */
static LoadInfo LoadElfFile(const char* filename, size_t pagesize) {
    LoadInfo load_result{};
    ASSERT(filename);
    int fd = open(filename, O_RDONLY);
    ElfW(Ehdr) ehdr;
    auto read_res = pread(fd, &ehdr, sizeof(ehdr), 0);
    if (read_res < 0) PANIC("Failed to read ELF header from file! file = {}", filename);
    if (ehdr.e_ident[EI_MAG0] != ELFMAG0 || ehdr.e_ident[EI_MAG1] != ELFMAG1 ||
        ehdr.e_ident[EI_MAG2] != ELFMAG2 || ehdr.e_ident[EI_MAG3] != ELFMAG3 ||
        ehdr.e_version != EV_CURRENT || ehdr.e_ehsize != sizeof(ehdr) ||
        ehdr.e_phentsize != sizeof(ElfW(Phdr)))
        switch (ehdr.e_machine) {
            case EM_ARM:
            case EM_AARCH64:
            case EM_X86_64:
            case EM_386:
                load_result.machine = ehdr.e_machine;
                break;
            default:
                PANIC("ELF file has wrong architecture! file = {}, e_machine = {}",
                      filename,
                      ehdr.e_machine);
                break;
        }
    ElfW(Phdr) phdr[MAX_PHNUM];
    if (ehdr.e_phnum > sizeof(phdr) / sizeof(phdr[0]) || ehdr.e_phnum < 1)
        PANIC("ELF file has unreasonable! file = {}, e_machine = {}", filename, ehdr.e_phnum);
    if (ehdr.e_type != ET_DYN)
        PANIC("ELF file not ET_DYN! file = {}, e_machine = {}", filename, ehdr.e_type);

    read_res = pread(fd, phdr, sizeof(phdr[0]) * ehdr.e_phnum, ehdr.e_phoff);
    if (read_res < 0) PANIC("Failed to read program headers from ELF file! file = {}", filename);

    size_t i = 0;
    while (i < ehdr.e_phnum && phdr[i].p_type != PT_LOAD) ++i;

    if (i == ehdr.e_phnum) PANIC("ELF file has no PT_LOAD header! file = {}", filename);

    /*
     * ELF requires that PT_LOAD segments be in ascending order of p_vaddr.
     * Find the last one to calculate the whole address span of the image.
     */
    const ElfW(Phdr)* first_load = &phdr[i];
    const ElfW(Phdr)* last_load = &phdr[ehdr.e_phnum - 1];
    while (last_load > first_load && last_load->p_type != PT_LOAD) --last_load;
    size_t span = last_load->p_vaddr + last_load->p_memsz - first_load->p_vaddr;
    /*
     * Map the first segment and reserve the space used for the rest and
     * for holes between segments.
     */
    auto mapping = reinterpret_cast<uintptr_t>(
            mmap(reinterpret_cast<void*>(RoundDown(first_load->p_vaddr, pagesize)),
                 span,
                 ProtFromPhdr(first_load),
                 MAP_PRIVATE,
                 fd,
                 RoundDown(first_load->p_offset, pagesize)));

    if (mapping == -1) PANIC("Map segment failed!");

    const ElfW(Addr) load_bias = mapping - RoundDown(first_load->p_vaddr, pagesize);
    if (first_load->p_offset > ehdr.e_phoff ||
        first_load->p_filesz < ehdr.e_phoff + (ehdr.e_phnum * sizeof(ElfW(Phdr))))
        PANIC("First load segment of ELF file does not contain phdrs! file = {}", filename);
    HandleBss(first_load, load_bias, pagesize);
    ElfW(Addr) last_end = first_load->p_vaddr + load_bias + first_load->p_memsz;
    /*
     * Map the remaining segments, and protect any holes between them.
     */
    const ElfW(Phdr) * ph;
    for (ph = first_load + 1; ph <= last_load; ++ph) {
        if (ph->p_type == PT_LOAD) {
            ElfW(Addr) last_page_end = RoundUp(last_end, pagesize);
            last_end = ph->p_vaddr + load_bias + ph->p_memsz;
            ElfW(Addr) start = RoundDown(ph->p_vaddr + load_bias, pagesize);
            ElfW(Addr) end = RoundUp(last_end, pagesize);
            if (start > last_page_end)
                mprotect(reinterpret_cast<void*>(last_page_end), start - last_page_end, PROT_NONE);

            auto mmap_res = mmap(reinterpret_cast<void*>(start),
                                 end - start,
                                 ProtFromPhdr(ph),
                                 MAP_PRIVATE | MAP_FIXED,
                                 fd,
                                 RoundDown(ph->p_offset, pagesize));

            if (mmap_res == MAP_FAILED) PANIC("Map segment failed!");
            HandleBss(ph, load_bias, pagesize);
        }
    }
    /*
     * Find the PT_INTERP header, if there is one.
     */
    for (i = 0; i < ehdr.e_phnum; ++i) {
        if (phdr[i].p_type == PT_INTERP) {
            /*
             * The PT_INTERP isn't really required to sit inside the first
             * (or any) load segment, though it normally does.  So we can
             * easily avoid an extra read in that case.
             */
            if (phdr[i].p_offset >= first_load->p_offset &&
                phdr[i].p_filesz <= first_load->p_filesz) {
                load_result.interp_path = (char*)(phdr[i].p_vaddr + load_bias);
            } else {
                static char interp_buffer[PATH_MAX + 1];
                if (phdr[i].p_filesz >= sizeof(interp_buffer))
                    PANIC("ELF file has unreasonable PT_INTERP size! file = {}", filename);
                read_res = pread(fd, interp_buffer, phdr[i].p_filesz, phdr[i].p_offset);
                if (read_res < 0) PANIC("Cannot read PT_INTERP segment contents!");
                load_result.interp_path = interp_buffer;
            }
            break;
        }
    }
    close(fd);
    load_result.base = load_bias;
    load_result.phdr = ehdr.e_phoff - first_load->p_offset + first_load->p_vaddr + load_bias;
    load_result.phnum = ehdr.e_phnum;
    load_result.addr = ehdr.e_entry + load_bias;
    return load_result;
}

/*
 * This points to a sequence of pointer-size words:
 *      [0]             argc
 *      [1..argc]       argv[0..argc-1]
 *      [1+argc]        NULL
 *      [2+argc..]      envp[0..]
 *                      NULL
 *                      auxv[0].a_type
 *                      auxv[1].a_un.a_val
 *                      ...
 *
 * argv[0] is the uninteresting name of this bootstrap program.  argv[1] is
 * the real program file name we'll open, and also the argv[0] for that
 * program.  We need to modify argc, move argv[1..] back to the argv[0..]
 * position, and also examine and modify the auxiliary vector on the stack.
 */
#define MAIN_STACK_SIZE 1_MB
static LoadInfo current_program;

int Execve(const char* program, std::span<char*> args, std::span<char*> envps) {
    current_program = LoadElfFile(program, getpagesize());

    // mmap main stack
    current_program.main_stack_bottom = reinterpret_cast<u8*>(
            mmap(nullptr, MAIN_STACK_SIZE, PROT_READ | PROT_WRITE, MAP_ANONYMOUS, -1, 0));
    if (current_program.main_stack_bottom == MAP_FAILED) PANIC("Main stack map failed!");

    auto stack_top = current_program.main_stack_bottom + MAIN_STACK_SIZE;

    auto argv = args.size() + 1;
    auto envp_count = envps.size();
//    auto auxv_count =
//    auto stack_elements =
}

}  // namespace swift::linux
