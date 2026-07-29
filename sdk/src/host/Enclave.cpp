//******************************************************************************
// Copyright (c) 2018, The Regents of the University of California (Regents).
// All Rights Reserved. See LICENSE for license details.
//------------------------------------------------------------------------------
#include "Enclave.hpp"
#include <cstdio>
#include <ctime>
#include <math.h>
#include <sys/mman.h>
#include <sys/stat.h>
extern "C" {
#include "common/sha3.h"
#include "shared/keystone_user.h"
}
#include "ElfFile.hpp"
#include "hash_util.hpp"

namespace Keystone {

static unsigned long long
monotonic_ns() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
  return static_cast<unsigned long long>(ts.tv_sec) * 1000000000ULL +
         static_cast<unsigned long long>(ts.tv_nsec);
}

static void
init_phase_marker(const char* phase, unsigned long long timestamp_ns) {
  fprintf(stderr, "PHASE,%s,%llu\n", phase, timestamp_ns);
  fflush(stderr);
}

Enclave::Enclave() {
}

Enclave::~Enclave() {
  destroy();
}

uint64_t
calculate_required_pages(ElfFile** elfFiles, size_t numElfFiles) {
  uint64_t req_pages = 0;

  for (int i = 0; i < numElfFiles; i++) {
    ElfFile* elfFile = elfFiles[i];
    req_pages += ceil(elfFile->getFileSize() / PAGE_SIZE);
  }
  /* FIXME: calculate the required number of pages for the page table.
   * We actually don't know how many page tables the enclave might need,
   * because the SDK never knows how its memory will be aligned.
   * Ideally, this should be managed by the driver.
   * For now, we naively allocate enough pages so that we can temporarily get
   * away from this problem.
   * 15 pages will be more than sufficient to cover several hundreds of
   * megabytes of enclave/runtime. */
  /* FIXME Part 2: now that loader does loading, .bss sections also eat up
   * space. Eapp dev must make FREEMEM big enough to fit this!!
   * Possible fix -- re-add exact .bss calculations?
   */

  /* Add one page each for bss segments of runtime and eapp */ 
  // TODO: add space for stack?
  req_pages += 16;
  return req_pages;
}

bool
Enclave::prepareEnclaveMemory(size_t requiredPages, uintptr_t alternatePhysAddr) {
  // FIXME: this will be deprecated with complete freemem support.
  // We just add freemem size for now.
  uint64_t minPages;
  minPages = ROUND_UP(params.getFreeMemSize(), PAGE_BITS) / PAGE_SIZE; 
  minPages += requiredPages;

  /* Call Enclave Driver */
  if (pDevice->create(minPages) != Error::Success) {
    return false;
  }

  /* We switch out the phys addr as needed */
  uintptr_t physAddr;
  if (alternatePhysAddr) {
    physAddr = alternatePhysAddr;
  } else {
    physAddr = pDevice->getPhysAddr();
  }

  pMemory->init(pDevice, physAddr, minPages);
  return true;
}

void
Enclave::copyFile(uintptr_t filePtr, size_t fileSize) {
	uintptr_t startOffset = pMemory->getCurrentOffset(); 
  size_t bytesRemaining = fileSize; 
	
	uintptr_t currOffset;
	while (bytesRemaining > 0) {
		currOffset = pMemory->getCurrentOffset(); 
    pMemory->incrementEPMFreeList();

		size_t bytesToWrite = (bytesRemaining > PAGE_SIZE) ? PAGE_SIZE : bytesRemaining;
		size_t bytesWritten = fileSize - bytesRemaining;

    // need 0 padding for hashes to be consistent,
    // and to keep code aligned to be able to map page-wise without copying.
    if (bytesToWrite < PAGE_SIZE) {
      char page[PAGE_SIZE];
      memset(page, 0, PAGE_SIZE);
      memcpy(page, (const void*) (filePtr + bytesWritten), (size_t)(bytesToWrite));
      pMemory->writeMem((uintptr_t) page, currOffset, PAGE_SIZE);
    } else {
		  pMemory->writeMem(filePtr + bytesWritten, currOffset, bytesToWrite);
    }
		bytesRemaining -= bytesToWrite;
	}
}

static void measureElfFile(hash_ctx_t* hash_ctx, ElfFile* file) {
  uintptr_t fptr = (uintptr_t) file->getPtr();
  uintptr_t fend = fptr + (uintptr_t) file->getFileSize();

  for (; fptr < fend; fptr += PAGE_SIZE) {
    if (fend - fptr < PAGE_SIZE) {
      char page[PAGE_SIZE];
      memset(page, 0, PAGE_SIZE);
      memcpy(page, (const void*) fptr, (size_t)(fend-fptr));
      hash_extend_page(hash_ctx, (void*) page);
    } else {
      hash_extend_page(hash_ctx, (void*) fptr);
    }
  }
}

Error
Enclave::measure(char* hash, const char* eapppath, const char* runtimepath, const char* loaderpath) {
  hash_ctx_t hash_ctx;
  hash_init(&hash_ctx);

  ElfFile* loader = new ElfFile(loaderpath);
  ElfFile* runtime = new ElfFile(runtimepath);
  ElfFile* eapp = new ElfFile(eapppath);

  uintptr_t sizes[3] = { PAGE_UP(loader->getFileSize()), PAGE_UP(runtime->getFileSize()),
                          PAGE_UP(eapp->getFileSize()) };
  hash_extend(&hash_ctx, (void*) sizes, sizeof(sizes));

  measureElfFile(&hash_ctx, loader);
  delete loader;
  measureElfFile(&hash_ctx, runtime);
  delete runtime;
  measureElfFile(&hash_ctx, eapp);
  delete eapp;

  hash_finalize(hash, &hash_ctx);

  return Error::Success;
}

Error
Enclave::init(const char* eapppath, const char* runtimepath, const char* loaderpath, Params _params) {
  return this->init(eapppath, runtimepath, loaderpath, _params, (uintptr_t)0);
}

Error
Enclave::init(
    const char* eapppath, const char* runtimepath, const char* loaderpath, Params _params,
    uintptr_t alternatePhysAddr) {
  const unsigned long long init_start_ns = monotonic_ns();
  params = _params;

  pMemory = new PhysicalEnclaveMemory();
  pDevice = new KeystoneDevice();

  ElfFile* enclaveFile = new ElfFile(eapppath);
  ElfFile* runtimeFile = new ElfFile(runtimepath);
  ElfFile* loaderFile = new ElfFile(loaderpath);

  if (!pDevice->initDevice(params)) {
    destroy();
    return Error::DeviceInitFailure;
  }

  ElfFile* elfFiles[3] = {enclaveFile, runtimeFile, loaderFile};
  size_t requiredPages = calculate_required_pages(elfFiles, 3);

  if (!prepareEnclaveMemory(requiredPages, alternatePhysAddr)) {
    destroy();
    return Error::DeviceError;
  }
  if (!pMemory->allocUtm(params.getUntrustedSize())) {
    ERROR("failed to init untrusted memory - ioctl() failed");
    destroy();
    return Error::DeviceError;
  }
	
  /* Copy loader into beginning of enclave memory */
  copyFile((uintptr_t) loaderFile->getPtr(), loaderFile->getFileSize());

  pMemory->startRuntimeMem();
  copyFile((uintptr_t) runtimeFile->getPtr(), runtimeFile->getFileSize());

  pMemory->startEappMem();
  copyFile((uintptr_t) enclaveFile->getPtr(), enclaveFile->getFileSize());

  pMemory->startFreeMem();

  const unsigned long long finalize_start_ns = monotonic_ns();
  init_phase_marker("finalize_ioctl_start", finalize_start_ns);
  Error finalize_error = pDevice->finalize(
          pMemory->getRuntimePhysAddr(), pMemory->getEappPhysAddr(),
          pMemory->getFreePhysAddr(), params.getFreeMemSize());
  const unsigned long long finalize_end_ns = monotonic_ns();
  init_phase_marker("finalize_ioctl_end", finalize_end_ns);
  if (finalize_error != Error::Success) {
    destroy();
    return Error::DeviceError;
  }
  if (!mapUntrusted(params.getUntrustedSize())) {
    ERROR(
        "failed to finalize enclave - cannot obtain the untrusted buffer "
        "pointer \n");
    destroy();
    return Error::DeviceMemoryMapError;
  }

  /* ELF files are no longer needed */
  delete enclaveFile;
  delete runtimeFile;
  delete loaderFile;

  const unsigned long long init_end_ns = monotonic_ns();
  const unsigned long long total_ns = init_end_ns - init_start_ns;
  const unsigned long long finalize_ioctl_ns = finalize_end_ns - finalize_start_ns;
  fprintf(
      stderr,
      "INIT_TIMING,total_ns,%llu,host_s_mode_ns,%llu,finalize_ioctl_ns,%llu\n",
      total_ns, total_ns - finalize_ioctl_ns, finalize_ioctl_ns);
  fflush(stderr);
  return Error::Success;
}

bool
Enclave::mapUntrusted(size_t size) {
  if (size == 0) {
    return true;
  }

  shared_buffer = pDevice->map(0, size);

  if (shared_buffer == NULL) {
    return false;
  }

  shared_buffer_size = size;

  return true;
}

Error
Enclave::destroy() {
  return pDevice->destroy();
}

Error
Enclave::run(uintptr_t* retval) {
  Error ret = pDevice->run(retval);
  while (ret == Error::EdgeCallHost || ret == Error::EnclaveInterrupted) {
    /* enclave is stopped in the middle. */
    if (ret == Error::EdgeCallHost && oFuncDispatch != NULL) {
      oFuncDispatch(getSharedBuffer());
    }
    ret = pDevice->resume(retval);
  }

  if (ret != Error::Success) {
    ERROR("failed to run enclave - ioctl() failed");
    destroy();
    return Error::DeviceError;
  }

  return Error::Success;
}

void*
Enclave::getSharedBuffer() {
  return shared_buffer;
}

size_t
Enclave::getSharedBufferSize() {
  return shared_buffer_size;
}

Memory*
Enclave::getMemory() {
  return pMemory;
}

Error
Enclave::registerOcallDispatch(OcallFunc func) {
  oFuncDispatch = func;
  return Error::Success;
}

}  // namespace Keystone
