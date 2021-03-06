#include <asm/errno.h>
#include <hex.h>
#include <pal_linux.h>
#include <pal_rtld.h>

#include "gsgx.h"
#include "sgx_arch.h"
#include "sgx_enclave.h"
#include "sgx_internal.h"

static int g_gsgx_device = -1;
static int g_isgx_device = -1;

static void* g_zero_pages       = NULL;
static size_t g_zero_pages_size = 0;

int open_gsgx(void)
{
    g_gsgx_device = INLINE_SYSCALL(open, 3, GSGX_FILE, O_RDWR | O_CLOEXEC, 0);
    if (IS_ERR(g_gsgx_device)) {
        SGX_DBG(DBG_E, "Cannot open device " GSGX_FILE ". Please make sure the"
                " Graphene SGX kernel module is loaded.\n");
        return -ERRNO(g_gsgx_device);
    }

    g_isgx_device = INLINE_SYSCALL(open, 3, ISGX_FILE, O_RDWR | O_CLOEXEC, 0);
    if (IS_ERR(g_isgx_device)) {
        SGX_DBG(DBG_E, "Cannot open device " ISGX_FILE ". Please make sure the"
                " Intel SGX kernel module is loaded.\n");
        INLINE_SYSCALL(close, 1, g_gsgx_device);
        g_gsgx_device = -1;
        return -ERRNO(g_isgx_device);
    }

    return 0;
}

int read_enclave_token(int token_file, sgx_arch_token_t * token)
{
    struct stat stat;
    int ret;
    ret = INLINE_SYSCALL(fstat, 2, token_file, &stat);
    if (IS_ERR(ret))
        return -ERRNO(ret);

    if (stat.st_size != sizeof(sgx_arch_token_t)) {
        SGX_DBG(DBG_I, "size of token size does not match\n");
        return -EINVAL;
    }

    int bytes = INLINE_SYSCALL(read, 3, token_file, token, sizeof(sgx_arch_token_t));
    if (IS_ERR(bytes))
        return -ERRNO(bytes);

#ifdef SGX_DCAP
    SGX_DBG(DBG_I, "Read dummy DCAP token\n");
#else
    SGX_DBG(DBG_I, "Read token:\n");
    SGX_DBG(DBG_I, "    valid:                 0x%08x\n",   token->body.valid);
    SGX_DBG(DBG_I, "    attr.flags:            0x%016lx\n", token->body.attributes.flags);
    SGX_DBG(DBG_I, "    attr.xfrm:             0x%016lx\n", token->body.attributes.xfrm);
    SGX_DBG(DBG_I, "    mr_enclave:            %s\n", ALLOCA_BYTES2HEXSTR(token->body.mr_enclave.m));
    SGX_DBG(DBG_I, "    mr_signer:             %s\n", ALLOCA_BYTES2HEXSTR(token->body.mr_signer.m));
    SGX_DBG(DBG_I, "    LE cpu_svn:            %s\n", ALLOCA_BYTES2HEXSTR(token->cpu_svn_le.svn));
    SGX_DBG(DBG_I, "    LE isv_prod_id:        %02x\n", token->isv_prod_id_le);
    SGX_DBG(DBG_I, "    LE isv_svn:            %02x\n", token->isv_svn_le);
    SGX_DBG(DBG_I, "    LE masked_misc_select: 0x%08x\n",   token->masked_misc_select_le);
    SGX_DBG(DBG_I, "    LE attr.flags:         0x%016lx\n", token->attributes_le.flags);
    SGX_DBG(DBG_I, "    LE attr.xfrm:          0x%016lx\n", token->attributes_le.xfrm);
#endif

    return 0;
}

int read_enclave_sigstruct(int sigfile, sgx_arch_enclave_css_t * sig)
{
    struct stat stat;
    int ret;
    ret = INLINE_SYSCALL(fstat, 2, sigfile, &stat);
    if (IS_ERR(ret))
        return -ERRNO(ret);

    if ((size_t)stat.st_size != sizeof(sgx_arch_enclave_css_t)) {
        SGX_DBG(DBG_I, "size of sigstruct size does not match\n");
        return -EINVAL;
    }

    int bytes = INLINE_SYSCALL(read, 3, sigfile, sig, sizeof(sgx_arch_enclave_css_t));
    if (IS_ERR(bytes))
        return -ERRNO(bytes);

    return 0;
}

#define SE_LEAF    0x12

static inline void cpuid(uint32_t leaf, uint32_t subleaf, uint32_t info[4])
{
    __asm__ volatile("cpuid"
                 : "=a"(info[0]),
                   "=b"(info[1]),
                   "=c"(info[2]),
                   "=d"(info[3])
                 : "a"(leaf),
                   "c"(subleaf));
}

static size_t get_ssaframesize (uint64_t xfrm)
{
    uint32_t cpuinfo[4];
    uint64_t xfrm_ex;
    size_t xsave_size = 0;

    cpuid(SE_LEAF, 1, cpuinfo);
    xfrm_ex = ((uint64_t) cpuinfo[3] << 32) + cpuinfo[2];

    for (int i = 2; i < 64; i++)
        if ((xfrm & (1ULL << i)) || (xfrm_ex & (1ULL << i))) {
            cpuid(0xd, i, cpuinfo);
            if (cpuinfo[0] + cpuinfo[1] > xsave_size)
                xsave_size = cpuinfo[0] + cpuinfo[1];
        }

    return ALLOC_ALIGN_UP(xsave_size + sizeof(sgx_pal_gpr_t) + 1);
}

bool is_wrfsbase_supported (void)
{
    uint32_t cpuinfo[4];
    cpuid(7, 0, cpuinfo);

    if (!(cpuinfo[1] & 0x1)) {
        SGX_DBG(DBG_E, "The WRFSBASE instruction is not permitted on this"
                " platform. Please make sure the Graphene SGX kernel module"
                " is loaded properly.\n");
        return false;
    }

    return true;
}

int create_enclave(sgx_arch_secs_t * secs,
                   sgx_arch_token_t * token)
{
    assert(secs->size && IS_POWER_OF_2(secs->size));
    assert(IS_ALIGNED(secs->base, secs->size));

    int flags = MAP_SHARED;

    secs->ssa_frame_size = get_ssaframesize(token->body.attributes.xfrm) / g_page_size;
    secs->misc_select = token->masked_misc_select_le;
    memcpy(&secs->attributes, &token->body.attributes, sizeof(sgx_attributes_t));

    /* Do not initialize secs->mr_signer and secs->mr_enclave here as they are
     * not used by ECREATE to populate the internal SECS. SECS's mr_enclave is
     * computed dynamically and SECS's mr_signer is populated based on the
     * SIGSTRUCT during EINIT (see pp21 for ECREATE and pp34 for
     * EINIT in https://software.intel.com/sites/default/files/managed/48/88/329298-002.pdf). */

    uint64_t addr = INLINE_SYSCALL(mmap, 6, secs->base, secs->size,
                                   PROT_NONE, /* newer DCAP driver requires such initial mmap */
                                   flags|MAP_FIXED, g_isgx_device, 0);

    if (IS_ERR_P(addr)) {
        if (ERRNO_P(addr) == 1 && (flags | MAP_FIXED))
            pal_printf("Permission denied on mapping enclave. "
                       "You may need to set sysctl vm.mmap_min_addr to zero\n");

        SGX_DBG(DBG_I, "enclave ECREATE failed in allocating EPC memory "
                "(errno = %ld)\n", ERRNO_P(addr));
        return -ENOMEM;
    }

    assert(secs->base == addr);

    struct sgx_enclave_create param = {
        .src = (uint64_t) secs,
    };
    int ret = INLINE_SYSCALL(ioctl, 3, g_isgx_device, SGX_IOC_ENCLAVE_CREATE, &param);

    if (IS_ERR(ret)) {
        SGX_DBG(DBG_I, "enclave ECREATE failed in enclave creation ioctl - %d\n", ERRNO(ret));
        return -ERRNO(ret);
    }

    if (ret) {
        SGX_DBG(DBG_I, "enclave ECREATE failed - %d\n", ret);
        return -EPERM;
    }

    secs->attributes.flags |= SGX_FLAGS_INITIALIZED;

    SGX_DBG(DBG_I, "enclave created:\n");
    SGX_DBG(DBG_I, "    base:           0x%016lx\n", secs->base);
    SGX_DBG(DBG_I, "    size:           0x%016lx\n", secs->size);
    SGX_DBG(DBG_I, "    misc_select:    0x%08x\n",   secs->misc_select);
    SGX_DBG(DBG_I, "    attr.flags:     0x%016lx\n", secs->attributes.flags);
    SGX_DBG(DBG_I, "    attr.xfrm:      0x%016lx\n", secs->attributes.xfrm);
    SGX_DBG(DBG_I, "    ssa_frame_size: %d\n",       secs->ssa_frame_size);
    SGX_DBG(DBG_I, "    isv_prod_id:    0x%08x\n",   secs->isv_prod_id);
    SGX_DBG(DBG_I, "    isv_svn:        0x%08x\n",   secs->isv_svn);

    return 0;
}

int add_pages_to_enclave(sgx_arch_secs_t * secs,
                         void * addr, void * user_addr,
                         unsigned long size,
                         enum sgx_page_type type, int prot,
                         bool skip_eextend,
                         const char * comment)
{
    sgx_arch_sec_info_t secinfo;
    int ret;

    if (!g_zero_pages) {
        /* initialize with just one page */
        g_zero_pages = (void*)INLINE_SYSCALL(mmap, 6, NULL, g_page_size, PROT_READ,
                                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (IS_ERR_P(g_zero_pages)) {
            SGX_DBG(DBG_I, "Cannot mmap zero pages %ld\n", ERRNO_P(g_zero_pages));
            return -ENOMEM;
        }
        g_zero_pages_size = g_page_size;
    }

    memset(&secinfo, 0, sizeof(sgx_arch_sec_info_t));

    switch (type) {
        case SGX_PAGE_SECS:
            return -EPERM;
        case SGX_PAGE_TCS:
            secinfo.flags |= SGX_SECINFO_FLAGS_TCS;
            break;
        case SGX_PAGE_REG:
            secinfo.flags |= SGX_SECINFO_FLAGS_REG;
            if (prot & PROT_READ)
                secinfo.flags |= SGX_SECINFO_FLAGS_R;
            if (prot & PROT_WRITE)
                secinfo.flags |= SGX_SECINFO_FLAGS_W;
            if (prot & PROT_EXEC)
                secinfo.flags |= SGX_SECINFO_FLAGS_X;
            break;
    }

    char p[4] = "---";
    const char * t = (type == SGX_PAGE_TCS) ? "TCS" : "REG";
    const char * m = skip_eextend ? "" : " measured";

    if (type == SGX_PAGE_REG) {
        if (prot & PROT_READ)
            p[0] = 'R';
        if (prot & PROT_WRITE)
            p[1] = 'W';
        if (prot & PROT_EXEC)
            p[2] = 'X';
    }

    if (size == g_page_size)
        SGX_DBG(DBG_I, "adding page  to enclave: %p [%s:%s] (%s)%s\n",
                addr, t, p, comment, m);
    else
        SGX_DBG(DBG_I, "adding pages to enclave: %p-%p [%s:%s] (%s)%s\n",
                addr, addr + size, t, p, comment, m);

#ifdef SGX_DCAP_16_OR_LATER
    if (!user_addr && g_zero_pages_size < size) {
        /* not enough contigious zero pages to back up enclave pages, allocate more */
        ret = INLINE_SYSCALL(munmap, 2, g_zero_pages, g_zero_pages_size);
        if (IS_ERR(ret)) {
            SGX_DBG(DBG_I, "Cannot unmap zero pages %d\n", ret);
            return -ERRNO(ret);
        }

        g_zero_pages = (void*)INLINE_SYSCALL(mmap, 6, NULL, size, PROT_READ,
                                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (IS_ERR_P(g_zero_pages)) {
            SGX_DBG(DBG_I, "Cannot map zero pages %ld\n", ERRNO_P(g_zero_pages));
            return -ENOMEM;
        }
        g_zero_pages_size = size;
    }

    /* newer DCAP driver (version 1.6+) allows adding a range of pages for performance, use it */
    struct sgx_enclave_add_pages param = {
        .offset     = (uint64_t)addr,
        .src        = (uint64_t)(user_addr ? : g_zero_pages),
        .length     = size,
        .secinfo    = (uint64_t)&secinfo,
        .flags      = skip_eextend ? 0 : SGX_PAGE_MEASURE,
        .count      = 0,  /* output parameter, will be checked after IOCTL */
    };

    ret = INLINE_SYSCALL(ioctl, 3, g_isgx_device, SGX_IOC_ENCLAVE_ADD_PAGES, &param);
    if (IS_ERR(ret)) {
        SGX_DBG(DBG_I, "Enclave EADD returned %d\n", ret);
        return -ERRNO(ret);
    }
    if (param.count != param.length) {
        SGX_DBG(DBG_I, "Enclave EADD didn't add all pages: added %lluB but expected %lluB\n",
                param.count, param.length);
        return -ERRNO(ret);
    }
#else
    /* older drivers (DCAP v1.5- and old out-of-tree) only supports adding one page at a time */
    struct sgx_enclave_add_page param = {
        .addr       = secs->base + (uint64_t) addr,
        .src        = (uint64_t) (user_addr ? : g_zero_pages),
        .secinfo    = (uint64_t) &secinfo,
        .mrmask     = skip_eextend ? 0 : (uint16_t) -1,
    };

    uint64_t added_size = 0;
    while (added_size < size) {
        ret = INLINE_SYSCALL(ioctl, 3, g_isgx_device, SGX_IOC_ENCLAVE_ADD_PAGE, &param);
        if (IS_ERR(ret)) {
            SGX_DBG(DBG_I, "Enclave EADD returned %d\n", ret);
            return -ERRNO(ret);
        }

        param.addr += g_page_size;
        if (param.src != (uint64_t)g_zero_pages)
            param.src += g_page_size;
        added_size += g_page_size;
    }
#endif /* SGX_DCAP_16_OR_LATER */

    /* need to change permissions for EADDed pages; actual permissions are capped by
     * permissions specified in SECINFO so here we specify the broadest set */
    ret = mprotect(secs->base + addr, size, PROT_READ | PROT_WRITE | PROT_EXEC);
    if (IS_ERR(ret)) {
        SGX_DBG(DBG_I, "Changing protections of EADDed pages returned %d\n", ret);
        return -ERRNO(ret);
    }

    return 0;
}

int init_enclave(sgx_arch_secs_t * secs,
                 sgx_arch_enclave_css_t * sigstruct,
                 sgx_arch_token_t * token)
{
#ifdef SGX_DCAP
    __UNUSED(token);
#endif
    unsigned long enclave_valid_addr = secs->base + secs->size - g_page_size;

    SGX_DBG(DBG_I, "enclave initializing:\n");
    SGX_DBG(DBG_I, "    enclave id:   0x%016lx\n", enclave_valid_addr);
    SGX_DBG(DBG_I, "    mr_enclave:   %s\n", ALLOCA_BYTES2HEXSTR(sigstruct->body.enclave_hash.m));

    struct sgx_enclave_init param = {
#ifndef SGX_DCAP_16_OR_LATER
        .addr           = enclave_valid_addr,
#endif
        .sigstruct      = (uint64_t) sigstruct,
#ifndef SGX_DCAP
        .einittoken     = (uint64_t) token,
#endif
    };
    int ret = INLINE_SYSCALL(ioctl, 3, g_isgx_device, SGX_IOC_ENCLAVE_INIT, &param);

    if (IS_ERR(ret)) {
        return -ERRNO(ret);
    }

    if (ret) {
        const char * error;
        /* DEP 3/22/17: Try to improve error messages */
        switch(ret) {
        case SGX_INVALID_SIG_STRUCT:
            error = "Invalid SIGSTRUCT";          break;
        case SGX_INVALID_ATTRIBUTE:
            error = "Invalid enclave attribute";  break;
        case SGX_INVALID_MEASUREMENT:
            error = "Invalid measurement";        break;
        case SGX_INVALID_SIGNATURE:
            error = "Invalid signature";          break;
        case SGX_INVALID_LICENSE:
            error = "Invalid EINIT token";        break;
        case SGX_INVALID_CPUSVN:
            error = "Invalid CPU SVN";            break;
        default:
            error = "Unknown reason";             break;
        }
        SGX_DBG(DBG_I, "enclave EINIT failed - %s\n", error);
        return -EPERM;
    }

    /* all enclave pages were EADDed, don't need zero pages anymore */
    ret = INLINE_SYSCALL(munmap, 2, g_zero_pages, g_zero_pages_size);
    if (IS_ERR(ret)) {
        SGX_DBG(DBG_I, "Cannot unmap zero pages %d\n", ret);
        return -ERRNO(ret);
    }

    return 0;
}

int destroy_enclave(void * base_addr, size_t length)
{

    SGX_DBG(DBG_I, "destroying enclave...\n");

    int ret = INLINE_SYSCALL(munmap, 2, base_addr, length);

    if (IS_ERR(ret)) {
        SGX_DBG(DBG_I, "enclave EDESTROY failed\n");
        return -ERRNO(ret);
    }

    return 0;
}
