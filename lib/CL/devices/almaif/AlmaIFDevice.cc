/* AlmaIFDevice.cc - Interface class for accessing AlmaIF device.
 * Responsible for setting up AlmaIF regions and device-backend specific
 * init (e.g. initializing a simulator or mmapping physical accelerator.)

   Copyright (c) 2022 Topi Leppänen / Tampere University

   Permission is hereby granted, free of charge, to any person obtaining a copy
   of this software and associated documentation files (the "Software"), to
   deal in the Software without restriction, including without limitation the
   rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
   sell copies of the Software, and to permit persons to whom the Software is
   furnished to do so, subject to the following conditions:

   The above copyright notice and this permission notice shall be included in
   all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
   FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
   IN THE SOFTWARE.
*/

#include "AlmaIFDevice.hh"

#include "AlmaifShared.hh"
#include "AlmaifCompile.hh"

#include "bufalloc.h"
#include "pocl_cache.h"
#include "pocl_file_util.h"
#include "pocl_timing.h"

#include <iostream>

AlmaIFDevice::AlmaIFDevice() {}

AlmaIFDevice::~AlmaIFDevice() {
  delete ControlMemory;
  delete InstructionMemory;
  delete CQMemory;
  delete DataMemory;
  memory_region_t *el, *tmp;
  LL_FOREACH_SAFE(AllocRegions, el, tmp) { free(el); }
}

void AlmaIFDevice::discoverDeviceParameters() {
  // Reset accelerator
  ControlMemory->Write32(ALMAIF_CONTROL_REG_COMMAND, ALMAIF_RESET_CMD);

  if (ControlMemory->Read32(ALMAIF_INFO_CORE_COUNT) != 1) {
    POCL_ABORT_UNIMPLEMENTED("Multicore accelerators");
  }

  uint32_t interface_version = ControlMemory->Read32(ALMAIF_INFO_IF_TYPE);

  if (interface_version == ALMAIF_VERSION_2) {
    /*Only AamuDSP should be using the old interface, if somethine else is,
     * you should modify this part to be more generic
    The mmap for opencl use case goes like this:
    BaseAddress                                       -->ControlMemory
    BaseAddress + segment_size                        --> Imem
    BaseAddress + 3*segment_size                      --> Dmem (for buffers)
    BaseAddress + 3*segment_size + Dmem_size - PRIVATE_MEM_SIZE - 4*64 --> Cqmem
    BaseAddress + 3*segment_size + Dmem_size - PRIVATE_MEM_SIZE        --> Local
    scratchpad memory for stack etc Where segment_size = 0x10000 (size of imem)
    */
    imem_size = ControlMemory->Read32(ALMAIF_INFO_IMEM_SIZE_LEGACY);
    // cq_size = ControlMemory->Read32(ALMAIF_INFO_PMEM_SIZE_LEGACY);
    cq_size = 4 * 64;
    // dmem_size = ControlMemory->Read32(ALMAIF_INFO_PMEM_SIZE_LEGACY);
    int private_mem_size =
        pocl_get_int_option("POCL_ALMAIF_PRIVATE_MEM_SIZE", ALMAIF_DEFAULT_PRIVATE_MEM_SIZE);

    dmem_size = ControlMemory->Read32(ALMAIF_INFO_PMEM_SIZE_LEGACY) -
                private_mem_size - cq_size;
    PointerSize = 4;
    RelativeAddressing = false;

    uint32_t segment_size = imem_size;
    imem_start = segment_size;
    dmem_start = 3 * segment_size;
    cq_start = dmem_start + dmem_size;
    cq_start += ControlMemory->PhysAddress;
    imem_start += ControlMemory->PhysAddress;
    dmem_start += ControlMemory->PhysAddress;
  } else if (interface_version == ALMAIF_VERSION_3) {
    uint64_t feature_flags =
        ControlMemory->Read64(ALMAIF_INFO_FEATURE_FLAGS_LOW);

    PointerSize = ControlMemory->Read32(ALMAIF_INFO_PTR_SIZE);
    // Turn on the relative addressing if the target has no axi master.
    RelativeAddressing =
        (feature_flags & ALMAIF_FF_BIT_AXI_MASTER) ? (false) : (true);

    imem_size = ControlMemory->Read32(ALMAIF_INFO_IMEM_SIZE);
    cq_size = ControlMemory->Read32(ALMAIF_INFO_CQMEM_SIZE_LOW);
    dmem_size = ControlMemory->Read32(ALMAIF_INFO_DMEM_SIZE_LOW);

    imem_start = ControlMemory->Read64(ALMAIF_INFO_IMEM_START_LOW);
    cq_start = ControlMemory->Read64(ALMAIF_INFO_CQMEM_START_LOW);
    dmem_start = ControlMemory->Read64(ALMAIF_INFO_DMEM_START_LOW);

    if (RelativeAddressing) {
      POCL_MSG_PRINT_ALMAIF("Almaif: Enabled relative addressing\n");
      cq_start += ControlMemory->PhysAddress;
      imem_start += ControlMemory->PhysAddress;
      dmem_start += ControlMemory->PhysAddress;
    }

  } else {
    POCL_ABORT_UNIMPLEMENTED("Unsupported AlmaIF version\n");
  }
  POCL_MSG_PRINT_ALMAIF("cq_start=%p imem_start=%p dmem_start=%p\n",
                       (void *)cq_start, (void *)imem_start,
                       (void *)dmem_start);
  POCL_MSG_PRINT_ALMAIF("cq_size=%u imem_size=%u dmem_size=%u\n", cq_size,
                       imem_size, dmem_size);
  POCL_MSG_PRINT_ALMAIF("ControlMemory->PhysAddress=%zu\n",
                       ControlMemory->PhysAddress);
  AllocRegions = (memory_region_t *)calloc(1, sizeof(memory_region_t));
  pocl_init_mem_region(AllocRegions,
                       dmem_start + ALMAIF_DEFAULT_CONSTANT_MEM_SIZE,
                       dmem_size - ALMAIF_DEFAULT_CONSTANT_MEM_SIZE);
  POCL_MSG_PRINT_ALMAIF(
      "Reserved %d bytes at the start of global memory for constant data\n",
      ALMAIF_DEFAULT_CONSTANT_MEM_SIZE);
}

void AlmaIFDevice::loadProgramToDevice(almaif_kernel_data_s *kd,
                                       cl_kernel kernel,
                                       _cl_command_node *cmd) {
  assert(kd);

  if (kd->imem_img_size == 0) {
    char img_file[POCL_MAX_PATHNAME_LENGTH];
    char cachedir[POCL_MAX_PATHNAME_LENGTH];
    // first try specialized
    pocl_cache_kernel_cachedir_path(img_file, kernel->program,
                                    cmd->program_device_i, kernel,
                                    "/parallel.img", cmd, 1);
    if (pocl_exists(img_file)) {
      pocl_cache_kernel_cachedir_path(
          cachedir, kernel->program, cmd->program_device_i, kernel, "", cmd, 1);
      preread_images(cachedir, kd);
    } else {
      // if it doesn't exist, try specialized with local sizes 0-0-0
      // should pick either 0-0-0 or 0-0-0-goffs0
      _cl_command_node cmd_copy;
      memcpy(&cmd_copy, cmd, sizeof(_cl_command_node));
      cmd_copy.command.run.pc.local_size[0] = 0;
      cmd_copy.command.run.pc.local_size[1] = 0;
      cmd_copy.command.run.pc.local_size[2] = 0;

      pocl_cache_kernel_cachedir_path(img_file, kernel->program,
                                      cmd->program_device_i, kernel,
                                      "/parallel.img", &cmd_copy, 1);
      if (pocl_exists(img_file)) {
        pocl_cache_kernel_cachedir_path(cachedir, kernel->program,
                                        cmd->program_device_i, kernel, "",
                                        &cmd_copy, 1);
      } else {
        pocl_cache_kernel_cachedir_path(cachedir, kernel->program,
                                        cmd->program_device_i, kernel, "",
                                        &cmd_copy, 0);
      }
      POCL_MSG_PRINT_ALMAIF("Specialized kernel not found, using %s\n",
                           cachedir);
      preread_images(cachedir, kd);
    }
  }

  assert(kd->imem_img_size > 0);

  ControlMemory->Write32(ALMAIF_CONTROL_REG_COMMAND, ALMAIF_RESET_CMD);

  InstructionMemory->CopyToMMAP(InstructionMemory->PhysAddress, kd->imem_img,
                                kd->imem_img_size);
  POCL_MSG_PRINT_ALMAIF("IMEM image written: %zu / %zu B\n",
                       InstructionMemory->PhysAddress, kd->imem_img_size);

  ControlMemory->Write32(ALMAIF_CONTROL_REG_COMMAND, ALMAIF_CONTINUE_CMD);
  HwClockStart = pocl_gettimemono_ns();
}

void AlmaIFDevice::preread_images(const char *kernel_cachedir,
                                  almaif_kernel_data_s *kd) {
  POCL_MSG_PRINT_ALMAIF("Reading image files\n");
  uint64_t temp = 0;
  size_t size = 0;
  char *content = NULL;

  char module_fn[POCL_MAX_PATHNAME_LENGTH];
  snprintf(module_fn, POCL_MAX_PATHNAME_LENGTH, "%s/parallel.img",
           kernel_cachedir);

  if (pocl_exists(module_fn)) {
    int res = pocl_read_file(module_fn, &content, &temp);
    size = (size_t)temp;
    assert(res == 0);
    assert(size > 0);
    assert(size < InstructionMemory->Size);
    kd->imem_img = content;
    kd->imem_img_size = size;
    content = NULL;
  } else
    POCL_ABORT("ALMAIF: %s for this kernel does not exist.\n", module_fn);

  snprintf(module_fn, POCL_MAX_PATHNAME_LENGTH, "%s/kernel_address.txt",
           kernel_cachedir);
  if (pocl_exists(module_fn)) {
    int res = pocl_read_file(module_fn, &content, &temp);
    assert(res == 0);
    size = (size_t)temp;
    assert(size > 0);

    uint32_t kernel_address = 0;
    sscanf(content, "kernel address = %u", &kernel_address);
    assert(kernel_address != 0);
    kd->kernel_address = kernel_address;
    content = NULL;
  } else
    POCL_ABORT("ALMAIF: %s for this kernel does not exist.\n", module_fn);

  /*  snprintf(module_fn, POCL_MAX_PATHNAME_LENGTH, "%s/parallel_local.img",
             kernel_cachedir);
    if (pocl_exists(module_fn)) {
      int res = pocl_read_file(module_fn, &content, &temp);
      assert(res == 0);
      size = (size_t)temp;
      if (size == 0)
        POCL_MEM_FREE(content);
      kd->dmem_img = content;
      kd->dmem_img_size = size;

      uint32_t kernel_addr = 0;
      if (size) {
        void *p = content + size - 4;
        uint32_t *up = (uint32_t *)p;
        kernel_addr = *up;
     }
      POCL_MSG_PRINT_ALMAIF("Kernel address (%0x) found\n", kernel_addr);
      kd->kernel_address = kernel_addr;
      content = NULL;
    } else
      POCL_ABORT("ALMAIF: %s for this kernel does not exist.\n", module_fn);
  */
}

void AlmaIFDevice::printMemoryDump() {
  for (unsigned k = 0; k < CQMemory->Size; k += 4) {
    uint32_t value = CQMemory->Read32(k);
    std::cerr << "CQ at " << k << "=" << value << "\n";
  }

  for (unsigned k = 0; k < DataMemory->Size; k += 4) {
    uint32_t value = DataMemory->Read32(k);
    std::cerr << "Data at " << k << "=" << value << "\n";
  }
  std::cerr << std::endl;
}

void AlmaIFDevice::writeDataToDevice(size_t dst,
                                     const char *__restrict__ const src,
                                     size_t size) {
  if (DataMemory->isInRange(dst)) {
    POCL_MSG_PRINT_ALMAIF("almaif: Copying %zu bytes to 0x%zx\n", size, dst);
    DataMemory->CopyToMMAP(dst, src, size);
  } else if (ExternalMemory && ExternalMemory->isInRange(dst)) {
    POCL_MSG_PRINT_ALMAIF("almaif: Copying %zu bytes to external 0x%zx\n", size,
                         dst);
    ExternalMemory->CopyToMMAP(dst, src, size);
  } else {
    POCL_ABORT("Attempt to write data to outside the device memories\n");
  }
}

void AlmaIFDevice::readDataFromDevice(char *__restrict__ const dst, size_t src,
                                      size_t size) {
  if (DataMemory->isInRange(src)) {
    POCL_MSG_PRINT_ALMAIF("almaif: Copying %zu bytes from 0x%zx\n", size, src);
    DataMemory->CopyFromMMAP(dst, src, size);
  } else if (ExternalMemory && ExternalMemory->isInRange(src)) {
    POCL_MSG_PRINT_ALMAIF("almaif: Copying 0x%zu bytes from external 0x%zx\n",
                         size, src);
    ExternalMemory->CopyFromMMAP(dst, src, size);
  } else {
    POCL_ABORT("Attempt to write data to outside the device memories\n");
  }
}
