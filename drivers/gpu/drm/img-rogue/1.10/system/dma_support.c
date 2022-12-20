/*************************************************************************/ /*!
@File			dma_support.c
@Title          System DMA support
@Copyright      Copyright (c) Imagination Technologies Ltd. All Rights Reserved
@Description    This provides a contiguous memory allocator (i.e. DMA allocator);
				these APIs are used for allocation/ioremapping (DMA/PA <-> CPU/VA)
@License        Dual MIT/GPLv2

The contents of this file are subject to the MIT license as set out below.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

Alternatively, the contents of this file may be used under the terms of
the GNU General Public License Version 2 ("GPL") in which case the provisions
of GPL are applicable instead of those above.

If you wish to allow use of your version of this file only under the terms of
GPL, and not to allow others to use your version of this file under the terms
of the MIT license, indicate your decision by deleting the provisions above
and replace them with the notice and other provisions required by GPL as set
out in the file called "GPL-COPYING" included in this distribution. If you do
not delete the provisions above, a recipient may use your version of this file
under the terms of either the MIT license or GPL.

This License is also included in this distribution in the file called
"MIT-COPYING".

EXCEPT AS OTHERWISE STATED IN A NEGOTIATED AGREEMENT: (A) THE SOFTWARE IS
PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING
BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
PURPOSE AND NONINFRINGEMENT; AND (B) IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/ /**************************************************************************/
#if defined(LINUX)
#include <linux/mm.h>
#include <asm/page.h>
#include <linux/device.h>
#include <linux/highmem.h>
#include <linux/vmalloc.h>
#include <linux/dma-mapping.h>
#include <asm-generic/getorder.h>
#endif

#include "allocmem.h"
#include "system/dma_support.h"
#include "kernel_compatibility.h"

#define DMA_MAX_IOREMAP_ENTRIES 2
static IMG_BOOL gbEnableDmaIoRemapping = IMG_FALSE;
static DMA_ALLOC gsDmaIoRemapArray[DMA_MAX_IOREMAP_ENTRIES] = {{0}};

#if defined(LINUX)
static void* 
SysDmaAcquireKernelAddress(struct page *page, IMG_UINT64 ui64Size, void *pvOSDevice)
{
	IMG_UINT32 uiIdx;
	PVRSRV_ERROR eError;
	void *pvVirtAddr = NULL;
	IMG_UINT32 ui32PgCount = (IMG_UINT32)(ui64Size >> OSGetPageShift());
	PVRSRV_DEVICE_NODE *psDevNode = OSAllocZMemNoStats(sizeof(*psDevNode));
	PVRSRV_DEVICE_CONFIG *psDevConfig = OSAllocZMemNoStats(sizeof(*psDevConfig));
	struct page **pagearray = OSAllocZMemNoStats(ui32PgCount * sizeof(struct page *));
#if defined(CONFIG_ARM64)
	pgprot_t prot = pgprot_writecombine(PAGE_KERNEL);
#else
	pgprot_t prot = pgprot_noncached(PAGE_KERNEL);
#endif

	/* Validate all required dynamic tmp buffer allocations */
	if (psDevNode == NULL || psDevConfig == NULL || pagearray == NULL)
	{
		if (psDevNode)
		{
			OSFreeMem(psDevNode);
		}

		if (psDevConfig)
		{
			OSFreeMem(psDevConfig);
		}

		if (pagearray)
		{
			OSFreeMem(pagearray);
		}

		goto e0;
	}

	/* Fake psDevNode->psDevConfig->pvOSDevice */
	psDevConfig->pvOSDevice = pvOSDevice;
	psDevNode->psDevConfig = psDevConfig;

	/* Evict any page data contents from d-cache */
	eError = OSCPUOperation(PVRSRV_CACHE_OP_FLUSH);
	for (uiIdx = 0; uiIdx  < ui32PgCount; uiIdx++)
	{
		/* Prepare array required for vmap */
		pagearray[uiIdx] = &page[uiIdx];

		if (eError != PVRSRV_OK)
		{
#if defined(CONFIG_64BIT)
			void *pvVirtStart = kmap(&page[uiIdx]);
			void *pvVirtEnd = pvVirtStart + ui64Size;
			IMG_CPU_PHYADDR sCPUPhysStart = {page_to_phys(&page[uiIdx])};
			IMG_CPU_PHYADDR sCPUPhysEnd = {sCPUPhysStart.uiAddr + ui64Size};
			/* all pages have a kernel linear address, flush entire range */
#else
			void *pvVirtStart = kmap(&page[uiIdx]);
			void *pvVirtEnd = pvVirtStart + PAGE_SIZE;
			IMG_CPU_PHYADDR sCPUPhysStart = {page_to_phys(&page[uiIdx])};
			IMG_CPU_PHYADDR sCPUPhysEnd = {sCPUPhysStart.uiAddr + PAGE_SIZE};
			/* pages might be from HIGHMEM, need to kmap/flush per page */
#endif

			/* Fallback to range-based d-cache flush */
			OSCPUCacheInvalidateRangeKM(psDevNode,
										pvVirtStart, pvVirtEnd,
										sCPUPhysStart, sCPUPhysEnd);

#if defined(CONFIG_64BIT)
			eError = PVRSRV_OK;
#else
			kunmap(&page[uiIdx]);
#endif
		}
	}

	/* Remap pages into VMALLOC space */
#if !defined(CONFIG_64BIT) || defined(PVRSRV_FORCE_SLOWER_VMAP_ON_64BIT_BUILDS)
	pvVirtAddr = vmap(pagearray, ui32PgCount, VM_READ | VM_WRITE, prot);
#else
	pvVirtAddr = vm_map_ram(pagearray, ui32PgCount, VM_READ | VM_WRITE, prot);
#endif

	/* Clean-up tmp buffers */
	OSFreeMem(psDevConfig);
	OSFreeMem(psDevNode);
	OSFreeMem(pagearray);

e0:
	return pvVirtAddr;
}

static void SysDmaReleaseKernelAddress(void *pvVirtAddr, IMG_UINT64 ui64Size)
{
#if !defined(CONFIG_64BIT) || defined(PVRSRV_FORCE_SLOWER_VMAP_ON_64BIT_BUILDS)
	vunmap(pvVirtAddr);
#else
	vm_unmap_ram(pvVirtAddr, ui64Size >> OSGetPageShift());
#endif
}
#endif

/*!
******************************************************************************
 @Function			SysDmaAllocMem

 @Description 		Allocates physically contiguous memory

 @Return			PVRSRV_ERROR	PVRSRV_OK on success. Otherwise, a PVRSRV_
									error code
 ******************************************************************************/
PVRSRV_ERROR SysDmaAllocMem(DMA_ALLOC *psDmaAlloc)
{
	PVRSRV_ERROR eError = PVRSRV_OK;
	struct page *page;

	if (psDmaAlloc == NULL ||
		psDmaAlloc->hHandle ||
		psDmaAlloc->pvVirtAddr ||
		psDmaAlloc->ui64Size == 0 ||
		psDmaAlloc->sBusAddr.uiAddr ||
		psDmaAlloc->pvOSDevice == NULL)
	{
		PVR_LOG_IF_FALSE((IMG_FALSE), "Invalid parameter");
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

#if defined(LINUX)
	psDmaAlloc->hHandle =
#if defined(CONFIG_L4) || defined(PVR_LINUX_PHYSMEM_SUPPRESS_DMA_AC)
		NULL;
#else
		dma_alloc_coherent((struct device *)psDmaAlloc->pvOSDevice,
						   (size_t) psDmaAlloc->ui64Size,
						   (dma_addr_t *)&psDmaAlloc->sBusAddr.uiAddr,
						   GFP_KERNEL);
#endif
	if (psDmaAlloc->hHandle)
	{
#if !defined(CONFIG_ARM)
#if defined(CONFIG_L4)
		page = pfn_to_page((unsigned long)l4x_phys_to_virt(psDmaAlloc->sBusAddr.uiAddr) >> PAGE_SHIFT);
#else
		page = pfn_to_page(psDmaAlloc->sBusAddr.uiAddr >> PAGE_SHIFT);
#endif
#else /* !defined(CONFIG_ARM) */
		page = pfn_to_page(dma_to_pfn((struct device *)psDmaAlloc->pvOSDevice, psDmaAlloc->sBusAddr.uiAddr));
#endif

		psDmaAlloc->pvVirtAddr = SysDmaAcquireKernelAddress(page,
													 		psDmaAlloc->ui64Size,
													  		psDmaAlloc->pvOSDevice);
		if (! psDmaAlloc->pvVirtAddr)
		{
			/* Not always safe due to possibility of DMA_ATTR_NO_KERNEL_MAPPING */
			PVR_LOG(("Cannot remap DMA buffer, using cookie VA as buffer VA"));
			psDmaAlloc->pvVirtAddr = psDmaAlloc->hHandle;
		}

		PVR_DPF((PVR_DBG_MESSAGE,
				"Allocated DMA buffer V:0x%llx P:0x%llx S:%llx",
				(IMG_UINT64)(uintptr_t)psDmaAlloc->pvVirtAddr,
				psDmaAlloc->sBusAddr.uiAddr,
				psDmaAlloc->ui64Size));
	}
	else if ((page = alloc_pages(GFP_KERNEL, get_order(psDmaAlloc->ui64Size))))
	{
		psDmaAlloc->pvVirtAddr = SysDmaAcquireKernelAddress(page,
													  		psDmaAlloc->ui64Size,
													  		psDmaAlloc->pvOSDevice);
		if (! psDmaAlloc->pvVirtAddr)
		{
			__free_pages(page, get_order(psDmaAlloc->ui64Size));
			goto e0;
		}

		psDmaAlloc->sBusAddr.uiAddr = page_to_phys(page);

		PVR_DPF((PVR_DBG_MESSAGE,
				"Allocated contiguous buffer V:0x%llx P:0x%llx S:%llx",
				(IMG_UINT64)(uintptr_t)psDmaAlloc->pvVirtAddr,
				psDmaAlloc->sBusAddr.uiAddr,
				psDmaAlloc->ui64Size));
	}
	else
	{
		eError = PVRSRV_ERROR_FAILED_TO_ALLOC_PAGES;
	}
#else
	#error "Provide OS implementation of DMA allocation";
#endif

e0:
	PVR_LOGR_IF_FALSE((psDmaAlloc->pvVirtAddr), "DMA/CMA allocation failed", PVRSRV_ERROR_FAILED_TO_ALLOC_PAGES);
	return eError;
}

/*!
******************************************************************************
 @Function			SysDmaFreeMem

 @Description 		Free physically contiguous memory

 @Return			void
 ******************************************************************************/
void SysDmaFreeMem(DMA_ALLOC *psDmaAlloc)
{
	if (psDmaAlloc == NULL ||
		psDmaAlloc->ui64Size == 0 ||
		psDmaAlloc->pvOSDevice == NULL ||
		psDmaAlloc->pvVirtAddr == NULL ||
		psDmaAlloc->sBusAddr.uiAddr == 0)
	{
		PVR_LOG_IF_FALSE((IMG_FALSE), "Invalid parameter");
		return;
	}

#if defined(LINUX)
	if (psDmaAlloc->pvVirtAddr != psDmaAlloc->hHandle)
	{
		SysDmaReleaseKernelAddress(psDmaAlloc->pvVirtAddr, psDmaAlloc->ui64Size);
	}

	if (! psDmaAlloc->hHandle)
	{
#if !defined(CONFIG_ARM)
		struct page *page = pfn_to_page(psDmaAlloc->sBusAddr.uiAddr >> PAGE_SHIFT);
#else
		struct page *page = pfn_to_page(dma_to_pfn((struct device *)psDmaAlloc->pvOSDevice, psDmaAlloc->sBusAddr.uiAddr));
#endif

		__free_pages(page, get_order(psDmaAlloc->ui64Size));
		return;
	}

	dma_free_coherent((struct device *)psDmaAlloc->pvOSDevice,
					  (size_t) psDmaAlloc->ui64Size,
					  psDmaAlloc->hHandle,
					  (dma_addr_t )psDmaAlloc->sBusAddr.uiAddr);
#else
	#error "Provide OS implementation of DMA deallocation";
#endif
}

/*!
******************************************************************************
 @Function			SysDmaRegisterForIoRemapping

 @Description 		Registers DMA_ALLOC for manual I/O remapping

 @Return			PVRSRV_ERROR	PVRSRV_OK on success. Otherwise, a PVRSRV_
									error code
 ******************************************************************************/
PVRSRV_ERROR SysDmaRegisterForIoRemapping(DMA_ALLOC *psDmaAlloc)
{
	IMG_UINT32 ui32Idx;
	IMG_BOOL bTabEntryFound = IMG_TRUE;
	PVRSRV_ERROR eError = PVRSRV_ERROR_TOO_FEW_BUFFERS;

	if (psDmaAlloc == NULL ||
		psDmaAlloc->ui64Size == 0 ||
		psDmaAlloc->pvOSDevice == NULL ||
		psDmaAlloc->pvVirtAddr == NULL ||
		psDmaAlloc->sBusAddr.uiAddr == 0)
	{
		PVR_LOG_IF_FALSE((IMG_FALSE), "Invalid parameter");
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

	for (ui32Idx = 0; ui32Idx < DMA_MAX_IOREMAP_ENTRIES; ++ui32Idx)
	{
		/* Check if an I/O remap entry exists for remapping */
		if (gsDmaIoRemapArray[ui32Idx].pvVirtAddr == NULL)
		{
			PVR_ASSERT(gsDmaIoRemapArray[ui32Idx].sBusAddr.uiAddr == 0);
			PVR_ASSERT(gsDmaIoRemapArray[ui32Idx].ui64Size == 0);
			break;
		}
	}

	if (ui32Idx >= DMA_MAX_IOREMAP_ENTRIES)
	{
		bTabEntryFound = IMG_FALSE;
	}

	if (bTabEntryFound)
	{
		IMG_BOOL bSameVAddr, bSamePAddr, bSameSize;

		bSamePAddr = gsDmaIoRemapArray[ui32Idx].sBusAddr.uiAddr == psDmaAlloc->sBusAddr.uiAddr;
		bSameVAddr = gsDmaIoRemapArray[ui32Idx].pvVirtAddr == psDmaAlloc->pvVirtAddr;
		bSameSize = gsDmaIoRemapArray[ui32Idx].ui64Size == psDmaAlloc->ui64Size;

		if (bSameVAddr)
		{
			if (bSamePAddr && bSameSize)
			{
				eError = PVRSRV_OK;
			}
			else
			{
				eError = PVRSRV_ERROR_ALREADY_EXISTS;
			}
		}
		else
		{
			PVR_ASSERT(bSamePAddr == IMG_FALSE);

			gsDmaIoRemapArray[ui32Idx].ui64Size = psDmaAlloc->ui64Size;
			gsDmaIoRemapArray[ui32Idx].sBusAddr = psDmaAlloc->sBusAddr;
			gsDmaIoRemapArray[ui32Idx].pvVirtAddr = psDmaAlloc->pvVirtAddr;

			PVR_DPF((PVR_DBG_MESSAGE,
					"DMA: register I/O remap: "\
					"VA: 0x%p, PA: 0x%llx, Size: 0x%llx",
					psDmaAlloc->pvVirtAddr, 
					psDmaAlloc->sBusAddr.uiAddr, 
					psDmaAlloc->ui64Size));

			gbEnableDmaIoRemapping = IMG_TRUE;
			eError = PVRSRV_OK;
		}
	}

	return eError;
}

/*!
******************************************************************************
 @Function			SysDmaDeregisterForIoRemapping

 @Description 		Deregisters DMA_ALLOC from manual I/O remapping

 @Return			void
 ******************************************************************************/
void SysDmaDeregisterForIoRemapping(DMA_ALLOC *psDmaAlloc)
{
	IMG_UINT32 ui32Idx;

	if (psDmaAlloc == NULL ||
		psDmaAlloc->ui64Size == 0 ||
		psDmaAlloc->pvOSDevice == NULL ||
		psDmaAlloc->pvVirtAddr == NULL ||
		psDmaAlloc->sBusAddr.uiAddr == 0)
	{
		PVR_LOG_IF_FALSE((IMG_FALSE), "Invalid parameter");
		return;
	}

	/* Remove specified entries from list of I/O remap entries */
	for (ui32Idx = 0; ui32Idx < DMA_MAX_IOREMAP_ENTRIES; ++ui32Idx)
	{
		if (gsDmaIoRemapArray[ui32Idx].pvVirtAddr == psDmaAlloc->pvVirtAddr)
		{
			gsDmaIoRemapArray[ui32Idx].sBusAddr.uiAddr = 0;
			gsDmaIoRemapArray[ui32Idx].pvVirtAddr = NULL;
			gsDmaIoRemapArray[ui32Idx].ui64Size =  0;

			PVR_DPF((PVR_DBG_MESSAGE,
					"DMA: deregister I/O remap: "\
					"VA: 0x%p, PA: 0x%llx, Size: 0x%llx",
					psDmaAlloc->pvVirtAddr, 
					psDmaAlloc->sBusAddr.uiAddr, 
					psDmaAlloc->ui64Size));

			break;
		}
	}

	/* Check if no other I/O remap entries exists for remapping */
	for (ui32Idx = 0; ui32Idx < DMA_MAX_IOREMAP_ENTRIES; ++ui32Idx)
	{
		if (gsDmaIoRemapArray[ui32Idx].pvVirtAddr != NULL)
		{
			break;
		}
	}

	if (ui32Idx == DMA_MAX_IOREMAP_ENTRIES)
	{
		/* No entries found so disable remapping */
		gbEnableDmaIoRemapping = IMG_FALSE;
	}
}

/*!
******************************************************************************
 @Function			SysDmaDevPAddrToCpuVAddr

 @Description 		Maps a DMA_ALLOC physical address to CPU virtual address

 @Return			IMG_CPU_VIRTADDR on success. Otherwise, a NULL
 ******************************************************************************/
IMG_CPU_VIRTADDR SysDmaDevPAddrToCpuVAddr(IMG_UINT64 uiAddr, IMG_UINT64 ui64Size)
{
	IMG_CPU_VIRTADDR pvDMAVirtAddr = NULL;
	DMA_ALLOC *psHeapDmaAlloc;
	IMG_UINT32 ui32Idx;

	if (gbEnableDmaIoRemapping == IMG_FALSE)
	{
		return pvDMAVirtAddr;
	}

	for (ui32Idx = 0; ui32Idx < DMA_MAX_IOREMAP_ENTRIES; ++ui32Idx)
	{
		psHeapDmaAlloc = &gsDmaIoRemapArray[ui32Idx];
		if (psHeapDmaAlloc->sBusAddr.uiAddr && uiAddr >= psHeapDmaAlloc->sBusAddr.uiAddr)
		{
			IMG_UINT64 uiSpan = psHeapDmaAlloc->ui64Size;
			IMG_UINT64 uiOffset = uiAddr - psHeapDmaAlloc->sBusAddr.uiAddr;

			if (uiOffset < uiSpan)
			{
				PVR_ASSERT((uiOffset+ui64Size-1) < uiSpan);
				pvDMAVirtAddr = psHeapDmaAlloc->pvVirtAddr + uiOffset;

				PVR_DPF((PVR_DBG_MESSAGE,
					"DMA: remap: PA: 0x%llx => VA: 0x%p",
					uiAddr, pvDMAVirtAddr));

				break;
			}
		}
	}

	return pvDMAVirtAddr;
}

/*!
******************************************************************************
 @Function			SysDmaCpuVAddrToDevPAddr

 @Description 		Maps a DMA_ALLOC CPU virtual address to physical address

 @Return			Non-zero value on success. Otherwise, a 0
 ******************************************************************************/
IMG_UINT64 SysDmaCpuVAddrToDevPAddr(IMG_CPU_VIRTADDR pvDMAVirtAddr)
{
	IMG_UINT64 uiAddr = 0;
	DMA_ALLOC *psHeapDmaAlloc;
	IMG_UINT32 ui32Idx;

	if (gbEnableDmaIoRemapping == IMG_FALSE)
	{
		return uiAddr;
	}

	for (ui32Idx = 0; ui32Idx < DMA_MAX_IOREMAP_ENTRIES; ++ui32Idx)
	{
		psHeapDmaAlloc = &gsDmaIoRemapArray[ui32Idx];
		if (psHeapDmaAlloc->pvVirtAddr && pvDMAVirtAddr >= psHeapDmaAlloc->pvVirtAddr)
		{
			IMG_UINT64 uiSpan = psHeapDmaAlloc->ui64Size;
			IMG_UINT64 uiOffset = pvDMAVirtAddr - psHeapDmaAlloc->pvVirtAddr;

			if (uiOffset < uiSpan)
			{
				uiAddr = psHeapDmaAlloc->sBusAddr.uiAddr + uiOffset;

				PVR_DPF((PVR_DBG_MESSAGE,
					"DMA: remap: VA: 0x%p => PA: 0x%llx",
					pvDMAVirtAddr, uiAddr));

				break;
			}
		}
	}

	return uiAddr;
}

/******************************************************************************
 End of file (dma_support.c)
******************************************************************************/
