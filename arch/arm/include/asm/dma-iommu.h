/* SPDX-License-Identifier: GPL-2.0 */
#ifndef ASMARM_DMA_IOMMU_H
#define ASMARM_DMA_IOMMU_H

#ifdef __KERNEL__

#include <linux/mm_types.h>
#include <linux/scatterlist.h>
#include <linux/kref.h>

struct device;
struct iommu_domain;

struct dma_iommu_mapping {
	/* iommu specific data */
	struct iommu_domain	*domain;
	bool automatic; /* installed by the ARM kernel DMA setup path */

	unsigned long		**bitmaps;	/* array of bitmaps */
	unsigned int		nr_bitmaps;	/* nr of elements in array */
	unsigned int		extensions;
	size_t			bitmap_size;	/* size of a single bitmap */
	size_t			bits;		/* per bitmap */
	dma_addr_t		base;

	spinlock_t		lock;
	struct kref		kref;
};

struct dma_iommu_mapping *
arm_iommu_create_mapping(struct device *dev, dma_addr_t base, u64 size);

void arm_iommu_release_mapping(struct dma_iommu_mapping *mapping);

int arm_iommu_attach_device(struct device *dev,
					struct dma_iommu_mapping *mapping);
void arm_iommu_detach_device(struct device *dev);
bool arm_iommu_is_dma_domain(struct device *dev, struct iommu_domain *domain);

#endif /* __KERNEL__ */
#endif
