/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-3-Clause) */
/*
 * DART IOMMU on Apple SoCs
 *
 * Copyright (C) 2020,2021 Corellium LLC
 */

#include <linux/err.h>
#include <linux/iommu.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/dma-iommu.h>
#include <linux/iommu-helper.h>
#include <linux/io.h>
#include <linux/interrupt.h>
#include <linux/slab.h>
#include <linux/sizes.h>
#include <linux/of.h>
#include <linux/of_iommu.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/clk.h>

#define DART_TLB_OP			0x0000
#define   DART_TLB_OP_FLUSH		0x00000002
#define   DART_TLB_OP_SID_SHIFT		8
#define   DART_TLB_OP_SID(sid4)		(1 << ((sid4) + 8))
#define   DART_TLB_OP_BUSY		(1 << 3)
#define DART_CONFIG			0x000C
#define   DART_CONFIG_TXEN(sid4)	(1 << ((sid4) * 8 + 7))
#define DART_ERROR_STATUS		0x0010
#define DART_ERROR_AXI_REQ0		0x0014
#define DART_ERROR_AXI_REQ1		0x0018
#define DART_ERROR_ADDRESS		0x001C
#define DART_DIAG_CONFIG		0x0020
#define DART_UNKNOWN_24			0x0024
#define DART_SID_REMAP			0x0028
#define DART_UNKNOWN_2C			0x002C
#define DART_FETCH_CONFIG		0x0030
#define DART_PERF_CONFIG		0x0078
#define DART_TLB_MISS			0x007C
#define DART_TLB_WAIT			0x0080
#define DART_TLB_HIT			0x0084
#define DART_ST_MISS			0x0088
#define DART_ST_WAIT			0x008C
#define DART_ST_HIT			0x0090
#define DART_TTBR(sid4,l1idx4)		(0x0040 + 16 * (sid4) + 4 * (l1idx4))
#define   DART_TTBR_VALID		(1 << 31)
#define   DART_TTBR_MASK		0x00FFFFFF
#define   DART_TTBR_SHIFT		12
#define DART_TLB_STATUS			0x1000
#define DART_TLB_UNKNOWN(idx)		(0x1004 + 4 * (idx))
#define DART_STT_PA_DATA(idx)		(0x2000 + 4 * (idx))
#define  DART_STT_PA_DATA_COUNT		1024
#define DART_SMMU_TLB_CFG		0x3000
#define DART_SMMU_TLB_DATA_RD		0x3100
#define  DART_SMMU_TLB_DATA_RD_COUNT	4
#define DART_DATA_DEBUG_IDX		0x3120
#define DART_DATA_DEBUG_CNTL		0x3124
#define  DART_DATA_DEBUG_CNTL_READ	(1 << 0)
#define  DART_DATA_DEBUG_CNTL_BUSY	(1 << 2)
#define DART_TLB_TAG(idx)		(0x3800 + 4 * (idx))
#define  DART_TLB_TAG_COUNT		128

#define DART1_TLB_OP			0x0020
#define   DART1_TLB_OP_OPMASK		(0xFFF << 20)
#define   DART1_TLB_OP_FLUSH		(1 << 20)
#define   DART1_TLB_OP_BUSY		(1 << 2)
#define DART1_TLB_OP_SIDMASK		0x0034
#define DART1_ERROR_STATUS		0x0040
#define   DART1_ERROR_FLAG		(1 << 31)
#define   DART1_ERROR_SID_MASK		(15 << 24)
#define   DART1_ERROR_APF_REJECT	(1 << 11)
#define   DART1_ERROR_UNKNOWN		(1 << 9)
#define   DART1_ERROR_CTRR_WRITE_PROT	(1 << 8)
#define   DART1_ERROR_REGION_PROT	(1 << 7)
#define   DART1_ERROR_AXI_SLV_ERR	(1 << 6)
#define   DART1_ERROR_AXI_SLV_DECODE	(1 << 5)
#define   DART1_ERROR_READ_PROT		(1 << 4)
#define   DART1_ERROR_WRITE_PROT	(1 << 3)
#define   DART1_ERROR_PTE_INVLD		(1 << 2)
#define   DART1_ERROR_L2E_INVLD		(1 << 1)
#define   DART1_ERROR_TTBR_INVLD	(1 << 0)
#define DART1_ERROR_ADDRESS_LO		0x0050
#define DART1_ERROR_ADDRESS_HI		0x0054
#define DART1_SID_REMAP(sid4)		(0x0080 + 4 * (sid4))
#define DART1_CONFIG(sid16)		(0x0100 + 4 * (sid16))
#define   DART1_CONFIG_TXEN		(1 << 7)
#define DART1_TTBR(sid16,l1idx4)	(0x0200 + 16 * (sid16) + 4 * (l1idx4))
#define   DART1_TTBR_VALID		(1 << 31)
#define   DART1_TTBR_MASK		0x0FFFFFFF
#define   DART1_TTBR_SHIFT		12

#define DART_PTE_STATE_MASK		3
#define   DART_PTE_STATE_INVALID	0
#define   DART_PTE_STATE_NEXT		3
#define   DART1_PTE_STATE_NEXT		11
#define   DART_PTE_STATE_VALID		3
#define DART_PTE_ADDR_MASK		0xFFFFFF000ull

#define DART_MAX_SID			16
#define DART_MAX_SUB			2

struct apple_dart_iommu {
	struct device *dev;
	struct clk_bulk_data *clks;
	int num_clks;
	struct iommu_device iommu;
	const struct apple_dart_version *version;
	void __iomem *base[DART_MAX_SUB];
	int is_init;
	int is_pcie;
	u32 page_bits;
	u32 sid_mask;
	u8 remap[DART_MAX_SID];
	u64 iova_offset;
	u64 **l2dma[DART_MAX_SID];
	u64 *l1dma[DART_MAX_SID];
	spinlock_t dart_lock;
};

#define DART_PAGE_SHIFT(im) ((im)->page_bits)
#define DART_PAGE_SIZE(im) (1ul << (im)->page_bits)
#define DART_PAGE_MASK(im) ((1ul << (im)->page_bits) - 1ul)

struct apple_dart_version {
	void (*irq)(struct apple_dart_iommu *im, void __iomem *base, unsigned subdart);
	void (*tlb_flush)(struct apple_dart_iommu *im, u32 sidmask,
			  void __iomem *base, unsigned subdart);
	void (*write_ttbr)(struct apple_dart_iommu *im, u32 sid, u64 phys,
			   void __iomem *base, unsigned subdart);
	void (*iommu_enable)(struct apple_dart_iommu *im, u32 sid,
			     void __iomem *base, unsigned subdart);
	void (*init)(struct apple_dart_iommu *im, void __iomem *base, unsigned subdart);

	unsigned min_sub, max_sub;	/* required subdevices */
	u64 pcie_iova_offset;	/* hardware offset between iova and translation table */
	u64 pcie_aperture[2];
	u64 system_aperture[2];
	u32 pte_next_flag;
};

struct apple_dart_iommu_domain {
	struct iommu_domain domain;
	struct list_head devices;
	spinlock_t list_lock;
	struct apple_dart_iommu *iommu;
	int sid;
};

struct apple_dart_iommu_domain_device {
	struct list_head list;
	struct device *dev;
};

struct apple_dart_iommu_devdata {
	struct apple_dart_iommu *iommu;
	u32 sid;
};

static const char * const apple_dart_subname[DART_MAX_SUB] = {
	"DART",
	"SMMU",
};

static void apple_dart_irq_v0(struct apple_dart_iommu *im, void __iomem *base, unsigned subdart)
{
	u32 status, axi_req[2], addr, tlbstat;

	status = readl(base + DART_ERROR_STATUS);
	tlbstat = readl(base + DART_TLB_STATUS);
	axi_req[0] = readl(base + DART_ERROR_AXI_REQ0);
	axi_req[1] = readl(base + DART_ERROR_AXI_REQ1);
	addr = readl(base + DART_ERROR_ADDRESS);

	writel(status, base + DART_ERROR_STATUS);
	writel(tlbstat, base + DART_TLB_STATUS);

	dev_err(im->dev, "STATUS %08x AXI_REQ %08x:%08x ADDR %08x TLBSTAT %08x\n",
		status, axi_req[0], axi_req[1], addr, tlbstat);
}

static void apple_dart_irq_v1(struct apple_dart_iommu *im, void __iomem *base, unsigned subdart)
{
	u32 status;
	u64 addr;

	status = readl(base + DART1_ERROR_STATUS);
	addr = readl(base + DART1_ERROR_ADDRESS_LO);
	addr |= (u64)readl(base + DART1_ERROR_ADDRESS_HI) << 32;

	writel(status, base + DART1_ERROR_STATUS);

	dev_err(im->dev, "%s STATUS %08x ADDR %08lx\n",
		apple_dart_subname[subdart], status, (unsigned long)addr);
}

static irqreturn_t apple_dart_irq(int irq, void *dev_id)
{
	struct apple_dart_iommu *im = dev_id;
	unsigned subdart;

	for(subdart=0; subdart<DART_MAX_SUB; subdart++)
		if(im->base[subdart])
			im->version->irq(im, im->base[subdart], subdart);

	return IRQ_HANDLED;
}

static void apple_dart_tlb_flush_v0(struct apple_dart_iommu *im, u32 sidmask,
				    void __iomem *base, unsigned subdart)
{
	u32 status;

	writel(DART_TLB_OP_FLUSH | (sidmask << DART_TLB_OP_SID_SHIFT), base + DART_TLB_OP);

	while(1) {
		status = readl(base + DART_TLB_OP);

		if(!(status & DART_TLB_OP_BUSY))
			break;
	}
}

static void apple_dart_tlb_flush_v1(struct apple_dart_iommu *im, u32 sidmask,
				    void __iomem *base, unsigned subdart)
{
	u32 status;

	writel(sidmask, base + DART1_TLB_OP_SIDMASK);
	writel(DART1_TLB_OP_FLUSH, base + DART1_TLB_OP);

	while(1) {
		status = readl(base + DART1_TLB_OP);

		if(!(status & DART1_TLB_OP_OPMASK))
			break;
		if(!(status & DART1_TLB_OP_BUSY))
			break;
	}
}

static void apple_dart_tlb_flush(struct apple_dart_iommu *im, u32 sidmask, int need_lock)
{
	unsigned long flags;
	unsigned subdart;

	if(need_lock)
		spin_lock_irqsave(&im->dart_lock, flags);

	for(subdart=0; subdart<DART_MAX_SUB; subdart++)
		if(im->base[subdart])
			im->version->tlb_flush(im, sidmask, im->base[subdart], subdart);

	if(need_lock)
		spin_unlock_irqrestore(&im->dart_lock, flags);
}

static void apple_dart_write_ttbr_v0(struct apple_dart_iommu *im, u32 sid, u64 phys,
				     void __iomem *base, u32 subdart)
{
	unsigned i;

	for(i=0; i<4; i++) {
		writel(((phys >> DART_TTBR_SHIFT) & DART_TTBR_MASK) | DART_TTBR_VALID,
		       base + DART_TTBR(sid, i));
		phys += DART_PAGE_SIZE(im);
	}
}

static void apple_dart_write_ttbr_v1(struct apple_dart_iommu *im, u32 sid, u64 phys,
				     void __iomem *base, u32 subdart)
{
	unsigned i;

	for(i=0; i<4; i++) {
		writel(((phys >> DART1_TTBR_SHIFT) & DART1_TTBR_MASK) | DART1_TTBR_VALID,
		       base + DART1_TTBR(sid, i));
		phys += DART_PAGE_SIZE(im);
	}
}

static u64 *apple_dart_get_pte(struct apple_dart_iommu *im, u32 sid, u64 iova, int optional,
			       unsigned long *flags, gfp_t gfp)
{
	unsigned i, l1idx, l1base, l2idx, npgs, npg;
	u64 phys, **l1pt, *l1dma, *l2dma;
	void *dmava, *ptva;
	dma_addr_t dmah;
	unsigned subdart;

	if(im->is_pcie)
		sid = 0;
	if(!im->l1dma[sid]) {
		spin_unlock_irqrestore(&im->dart_lock, *flags);
		ptva = (void *)__get_free_pages(gfp | __GFP_ZERO,
						DART_PAGE_SHIFT(im) + 2 - PAGE_SHIFT);
		dmava = dma_alloc_attrs(im->dev, DART_PAGE_SIZE(im) * 4, &dmah,
					gfp | __GFP_ZERO, DMA_ATTR_WRITE_COMBINE);
		spin_lock_irqsave(&im->dart_lock, *flags);
		if(!im->l1dma[sid]) {
			if(!ptva || !dmava) {
				if(ptva)
					free_pages((unsigned long)ptva,
						   DART_PAGE_SHIFT(im) + 2 - PAGE_SHIFT);
				else
					dev_err(im->dev, "failed to allocate shadow L1 pagetable.\n");
				if(dmava)
					dma_free_attrs(im->dev, DART_PAGE_SIZE(im) * 4, dmava, dmah,
							DMA_ATTR_WRITE_COMBINE);
				else
					dev_err(im->dev, "failed to allocate uncached L1 pagetable.\n");
				return NULL;
			}
			im->l2dma[sid] = ptva;
			im->l1dma[sid] = dmava;
			phys = dmah;

			for(subdart=0; subdart<DART_MAX_SUB; subdart++)
				if(im->base[subdart])
					im->version->write_ttbr(im, sid, phys,
								im->base[subdart], subdart);
		} else {
			if(ptva)
				free_pages((unsigned long)ptva, DART_PAGE_SHIFT(im) + 2 - PAGE_SHIFT);
			if(dmava)
				dma_free_attrs(im->dev, DART_PAGE_SIZE(im) * 4, dmava, dmah,
					       DMA_ATTR_WRITE_COMBINE);
		}
	}

	l1pt = im->l2dma[sid];
	l1idx = (iova >> (2 * DART_PAGE_SHIFT(im) - 3)) & (DART_PAGE_MASK(im) >> 1);

	if(!l1pt[l1idx]) {
		if(optional)
			return NULL;
		if(DART_PAGE_SHIFT(im) < PAGE_SHIFT)
			npgs = PAGE_SHIFT - DART_PAGE_SHIFT(im);
		else
			npgs = 0;
		spin_unlock_irqrestore(&im->dart_lock, *flags);
		dmava = dma_alloc_attrs(im->dev, DART_PAGE_SIZE(im) << npgs, &dmah,
					gfp | __GFP_ZERO, DMA_ATTR_WRITE_COMBINE);
		spin_lock_irqsave(&im->dart_lock, *flags);
		if(!l1pt[l1idx]) {
			if(!dmava) {
				dev_err(im->dev, "failed to allocate uncached L2 pagetable.\n");
				return NULL;
			}
			npg = 1 << npgs;
			phys = dmah;
			l1dma = im->l1dma[sid];
			l1base = (l1idx >> npgs) << npgs;
			for(i=0; i<npg; i++) {
				l1pt[l1base + i] = dmava + (i << DART_PAGE_SHIFT(im));
				l1dma[l1base + i] =
					((phys + (i << DART_PAGE_SHIFT(im))) & DART_PTE_ADDR_MASK) |
					im->version->pte_next_flag;
			}
		} else
			if(dmava)
				dma_free_attrs(im->dev, DART_PAGE_SIZE(im) << npgs, dmava, dmah,
					       DMA_ATTR_WRITE_COMBINE);
	}

	l2dma = l1pt[l1idx];
	l2idx = (iova >> DART_PAGE_SHIFT(im)) & (DART_PAGE_MASK(im) >> 3);
	return &l2dma[l2idx];
}

static void apple_dart_enable_v0(struct apple_dart_iommu *im, u32 sid,
				 void __iomem *base, unsigned subdart)
{
	u32 val;

	val = readl(base + DART_CONFIG);
	if(val & DART_CONFIG_TXEN(sid))
		return;
	writel(val | DART_CONFIG_TXEN(sid), base + DART_CONFIG);
	if(!(readl(base + DART_CONFIG) & DART_CONFIG_TXEN(sid)))
		dev_err(im->dev, "failed to enable SID %d: 0x%08x.\n",
			sid, readl(base + DART_CONFIG));
}

static void apple_dart_enable_v1(struct apple_dart_iommu *im, u32 sid,
				 void __iomem *base, unsigned subdart)
{
	u32 val;

	val = readl(base + DART1_CONFIG(sid));
	if(val == DART1_CONFIG_TXEN)
		return;
	writel(DART1_CONFIG_TXEN, base + DART1_CONFIG(sid));
	if(readl(base + DART1_CONFIG(sid)) != DART1_CONFIG_TXEN)
		dev_err(im->dev, "failed to enable SID %d: 0x%08x.\n",
			sid, readl(base + DART1_CONFIG(sid)));
}

static void apple_dart_enable(struct apple_dart_iommu *im, u32 sid)
{
	unsigned subdart;

	for(subdart=0; subdart<DART_MAX_SUB; subdart++)
		if(im->base[subdart])
			im->version->iommu_enable(im, sid, im->base[subdart], subdart);
}

static bool apple_dart_iommu_capable(enum iommu_cap cap)
{
	switch (cap) {
	case IOMMU_CAP_CACHE_COHERENCY: return true;
	default: return false;
	}
}

static struct apple_dart_iommu_domain *to_apple_dart_iommu_domain(struct iommu_domain *dom)
{
	return container_of(dom, struct apple_dart_iommu_domain, domain);
}

static struct iommu_domain *apple_dart_iommu_domain_alloc(unsigned type)
{
	struct apple_dart_iommu_domain *idom;

	if(type != IOMMU_DOMAIN_UNMANAGED && type != IOMMU_DOMAIN_DMA)
		return NULL;

	idom = kzalloc(sizeof(*idom), GFP_KERNEL);
	if(!idom)
		return NULL;

	if(type == IOMMU_DOMAIN_DMA && iommu_get_dma_cookie(&idom->domain)) {
		kfree(idom);
		return NULL;
	}

	INIT_LIST_HEAD(&idom->devices);

	idom->sid = -1;

	return &idom->domain;
}

static void apple_dart_iommu_domain_free(struct iommu_domain *domain)
{
	struct apple_dart_iommu_domain *idom = to_apple_dart_iommu_domain(domain);

	if(domain->type == IOMMU_DOMAIN_DMA)
		iommu_put_dma_cookie(&idom->domain);

	kfree(idom);
}

static void apple_dart_init_v0(struct apple_dart_iommu *im, void __iomem *base, unsigned subdart)
{
	u32 i, j;

	writel(0x0020FFFC, im->base + DART_UNKNOWN_24);
	writel(0x00000000, im->base + DART_UNKNOWN_2C);
	for(i=0; i<4; i++)
		for(j=0; j<4; j++)
			writel(0x00000000, im->base + DART_TTBR(i, j));
	writel(0x000E0303, im->base + DART_FETCH_CONFIG);
	writel(0x00000100, im->base + DART_DIAG_CONFIG);
	for(i=0; i<6; i++)
		writel(0x00000000, im->base + DART_TLB_UNKNOWN(i));
	writel(0x03F3FFFF, im->base + DART_TLB_STATUS);

	apple_dart_tlb_flush(im, 15, 0);
}

static void apple_dart_init_v1(struct apple_dart_iommu *im, void __iomem *base, unsigned subdart)
{
	u32 sid, i;
	u32 remap[4];

	if(!subdart) {
		for(sid=0; sid<DART_MAX_SID/4; sid++)
			remap[sid] = readl(base + DART1_SID_REMAP(sid));
		for(sid=0; sid<DART_MAX_SID; sid++)
			if(im->remap[sid] < DART_MAX_SID) {
				remap[sid >> 2] &= ~(0xFF << (sid << 3));
				remap[sid >> 2] |= im->remap[sid] << (sid << 3);
			}
	}

	for(sid=0; sid<DART_MAX_SID/4; sid++)
		writel(remap[sid], base + DART1_SID_REMAP(sid));

	for(sid=0; sid<DART_MAX_SID; sid++)
		if(im->sid_mask & (1u << sid))
			for(i=0; i<4; i++)
				writel(0x00000000, base + DART1_TTBR(sid, i));

	apple_dart_tlb_flush_v1(im, 0xffffffff, base, subdart);

	for(sid=0; sid<DART_MAX_SID; sid++)
		writel(DART1_CONFIG_TXEN, base + DART1_CONFIG(sid));
}

static int apple_dart_iommu_attach_device(struct iommu_domain *domain, struct device *dev)
{
	struct apple_dart_iommu_domain *idom = to_apple_dart_iommu_domain(domain);
	struct apple_dart_iommu_domain_device *domain_device;
	struct apple_dart_iommu_devdata *idd;
	struct apple_dart_iommu *im;
	unsigned long flags;
	unsigned subdart;
	u32 sid;

	idd = dev_iommu_priv_get(dev);
	if(!idd)
		return -ENODEV;
	im = idd->iommu;

	if(idom->iommu && idom->iommu != im) {
		dev_err(dev, "different DART already assigned to IOMMU domain.\n");
		return -EINVAL;
	}

	if(!idom->iommu) {
		idom->iommu = im;
		if(im->is_pcie) {
			idom->domain.geometry.aperture_start = im->version->pcie_aperture[0];
			idom->domain.geometry.aperture_end   = im->version->pcie_aperture[1];
		} else {
			idom->domain.geometry.aperture_start = im->version->system_aperture[0];
			idom->domain.geometry.aperture_end   = im->version->system_aperture[1];
		}
		idom->domain.geometry.force_aperture = true;
	}

	sid = im->is_pcie ? 0 : idd->sid;
	if(idom->sid >= 0 && idom->sid != sid) {
		dev_err(dev, "multiple SIDs mapped to the same IOMMU domain.\n");
		return -EEXIST;
	}
	idom->sid = sid;

	spin_lock_irqsave(&im->dart_lock, flags);

	if(!im->is_init) {
		im->is_init = 1;

		for(subdart=0; subdart<DART_MAX_SUB; subdart++)
			if(im->base[subdart])
				im->version->init(im, im->base[subdart], subdart);
	}
	apple_dart_enable(im, sid);

	spin_unlock_irqrestore(&im->dart_lock, flags);

	domain_device = kzalloc(sizeof(*domain_device), GFP_KERNEL);
	if(!domain_device)
		return -ENOMEM;

	domain_device->dev = dev;

	spin_lock_irqsave(&idom->list_lock, flags);
	list_add(&domain_device->list, &idom->devices);
	spin_unlock_irqrestore(&idom->list_lock, flags);

	dev_err(dev, "attached to %s:0x%x\n", dev_name(im->dev), idd->sid);

	return 0;
}

static void apple_dart_iommu_detach_device(struct iommu_domain *domain, struct device *dev)
{
	struct apple_dart_iommu_domain *idom = to_apple_dart_iommu_domain(domain);
	struct apple_dart_iommu_domain_device *domain_device, *tmp;
	unsigned long flags;

	spin_lock_irqsave(&idom->list_lock, flags);
	list_for_each_entry_safe(domain_device, tmp, &idom->devices, list) {
		if(domain_device->dev == dev) {
			list_del(&domain_device->list);
			kfree(domain_device);
			break;
		}
	}
	spin_unlock_irqrestore(&idom->list_lock, flags);
}

static struct iommu_device *apple_dart_iommu_probe_device(struct device *dev)
{
	struct apple_dart_iommu_devdata *idd;

	idd = dev_iommu_priv_get(dev);
	if(!idd)
		return ERR_PTR(-ENODEV);

	return &idd->iommu->iommu;
}

static void apple_dart_iommu_release_device(struct device *dev)
{
}

static int apple_dart_iommu_map(struct iommu_domain *domain, unsigned long iova,
				phys_addr_t paddr, size_t size, int prot, gfp_t gfp)
{
	struct apple_dart_iommu_domain *idom = to_apple_dart_iommu_domain(domain);
	struct apple_dart_iommu *im = idom->iommu;
	unsigned i, npg = (size + DART_PAGE_MASK(im)) >> DART_PAGE_SHIFT(im);
	unsigned long flags;
	u64 *ptep;
	int ret = 0;

	if(idom->sid < 0)
		return 0;

	if(iova < im->iova_offset)
		return 0;
	iova -= im->iova_offset;

	spin_lock_irqsave(&im->dart_lock, flags);
	for(i=0; i<npg; i++) {
		ptep = apple_dart_get_pte(im, idom->sid, iova, 0, &flags, gfp);
		if(!ptep) {
			ret = -ENOMEM;
			break;
		}
		*ptep = (paddr & DART_PTE_ADDR_MASK) | DART_PTE_STATE_VALID;
		iova += DART_PAGE_SIZE(im);
		paddr += DART_PAGE_SIZE(im);
	}
	spin_unlock_irqrestore(&im->dart_lock, flags);

	return ret;
}

static phys_addr_t apple_dart_iommu_iova_to_phys(struct iommu_domain *domain, dma_addr_t iova)
{
	struct apple_dart_iommu_domain *idom = to_apple_dart_iommu_domain(domain);
	struct apple_dart_iommu *im = idom->iommu;
	unsigned long flags;
	u64 *ptep, result = 0;

	if(idom->sid < 0)
		return 0;

	if(iova < im->iova_offset)
		return 0;
	iova -= im->iova_offset;

	spin_lock_irqsave(&im->dart_lock, flags);
	ptep = apple_dart_get_pte(im, idom->sid, iova, 1, &flags, GFP_KERNEL);
	if(ptep)
		result = *ptep;
	spin_unlock_irqrestore(&im->dart_lock, flags);

	if(result & DART_PTE_STATE_MASK)
		result = (result & DART_PTE_ADDR_MASK) | (iova & DART_PAGE_MASK(im));
	return result;
}

static size_t apple_dart_iommu_unmap(struct iommu_domain *domain, unsigned long iova, size_t size,
				     struct iommu_iotlb_gather *gather)
{
	struct apple_dart_iommu_domain *idom = to_apple_dart_iommu_domain(domain);
	struct apple_dart_iommu *im = idom->iommu;
	unsigned i, npg = (size + DART_PAGE_MASK(im)) >> DART_PAGE_SHIFT(im);
	unsigned long flags;
	u64 *ptep;

	if(idom->sid < 0)
		return 0;

	if(iova < im->iova_offset)
		return 0;
	iova -= im->iova_offset;

	spin_lock_irqsave(&im->dart_lock, flags);
	for(i=0; i<npg; i++) {
		ptep = apple_dart_get_pte(im, idom->sid, iova, 1, &flags, GFP_KERNEL);
		if(ptep)
			*ptep = 0;
		iova += DART_PAGE_SIZE(im);
	}
	spin_unlock_irqrestore(&im->dart_lock, flags);

	return size;
}

static void apple_dart_iommu_flush_iotlb_all(struct iommu_domain *domain)
{
	struct apple_dart_iommu_domain *idom = to_apple_dart_iommu_domain(domain);

	if(!idom->iommu)
		return;

	if(idom->sid >= 0)
		apple_dart_tlb_flush(idom->iommu, 1u << idom->sid, 1);
}

static void apple_dart_iommu_iotlb_sync(struct iommu_domain *domain,
					struct iommu_iotlb_gather *gather)
{
	struct apple_dart_iommu_domain *idom = to_apple_dart_iommu_domain(domain);

	if(!idom->iommu)
		return;

	if(idom->sid >= 0)
		apple_dart_tlb_flush(idom->iommu, 1u << idom->sid, 1);
}

static void apple_dart_iommu_iotlb_sync_map(struct iommu_domain *domain)
{
	struct apple_dart_iommu_domain *idom = to_apple_dart_iommu_domain(domain);

	if(!idom->iommu)
		return;

	if(idom->sid >= 0)
		apple_dart_tlb_flush(idom->iommu, 1u << idom->sid, 1);
}

static const struct iommu_ops apple_dart_iommu_ops;

static int apple_dart_iommu_of_xlate(struct device *dev, struct of_phandle_args *args)
{
	struct platform_device *iommu_dev;
	struct apple_dart_iommu_devdata *data;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if(!data)
		return -ENOMEM;

	iommu_dev = of_find_device_by_node(args->np);

	data->iommu = platform_get_drvdata(iommu_dev);
	data->sid = args->args[0];
	dev_iommu_priv_set(dev, data);

	if(dev->bus->iommu_ops != &apple_dart_iommu_ops)
		bus_set_iommu(dev->bus, &apple_dart_iommu_ops);

	platform_device_put(iommu_dev);

	return 0;
}

static const struct iommu_ops apple_dart_iommu_ops = {
	.capable = apple_dart_iommu_capable,
	.of_xlate = apple_dart_iommu_of_xlate,
	.domain_alloc = apple_dart_iommu_domain_alloc,
	.domain_free = apple_dart_iommu_domain_free,
	.attach_dev = apple_dart_iommu_attach_device,
	.detach_dev = apple_dart_iommu_detach_device,
	.map = apple_dart_iommu_map,
	.unmap = apple_dart_iommu_unmap,
	.iova_to_phys = apple_dart_iommu_iova_to_phys,
	.flush_iotlb_all = apple_dart_iommu_flush_iotlb_all,
	.iotlb_sync = apple_dart_iommu_iotlb_sync,
	.iotlb_sync_map = apple_dart_iommu_iotlb_sync_map,
	.probe_device = apple_dart_iommu_probe_device,
	.release_device = apple_dart_iommu_release_device,
	.device_group = generic_device_group,
	.pgsize_bitmap = ~0x3FFFul,
};

static const struct apple_dart_version apple_dart_version_a10 = {
	.irq =			apple_dart_irq_v0,
	.tlb_flush =		apple_dart_tlb_flush_v0,
	.write_ttbr =		apple_dart_write_ttbr_v0,
	.iommu_enable =		apple_dart_enable_v0,
	.init =			apple_dart_init_v0,

	.min_sub =		1,
	.max_sub =		1,
	.pcie_iova_offset =	0x80000000,
	.pcie_aperture =	{ 0x80000000, 0xBBFFFFFF },
	.system_aperture =	{ 0x00004000, 0xFFFFFFFF },
	.pte_next_flag =	DART_PTE_STATE_NEXT,
};

static const struct apple_dart_version apple_dart_version_m1 = {
	.irq =			apple_dart_irq_v1,
	.tlb_flush =		apple_dart_tlb_flush_v1,
	.write_ttbr =		apple_dart_write_ttbr_v1,
	.iommu_enable =		apple_dart_enable_v1,
	.init =			apple_dart_init_v1,

	.min_sub =		1,
	.max_sub =		2,
	.pcie_iova_offset =	0,
	.pcie_aperture =	{ 0x00100000, 0x3FEFFFFF },
	.system_aperture =	{ 0x00004000, 0xFFFFFFFF },
	.pte_next_flag =	DART1_PTE_STATE_NEXT,
};

static const struct of_device_id apple_dart_iommu_match[] = {
	{ .compatible = "apple,dart-a10", .data = &apple_dart_version_a10 },
	{ .compatible = "apple,dart-m1",  .data = &apple_dart_version_m1  },
	{ .compatible = "apple,dart",     .data = &apple_dart_version_m1  },
	{ },
};
MODULE_DEVICE_TABLE(of, apple_dart_iommu_match);

static int apple_dart_iommu_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *node = dev->of_node;
	struct fwnode_handle *fwnode = &node->fwnode;
	const struct of_device_id *ofdev;
	struct apple_dart_iommu *im;
	struct resource *r;
	int ret = 0, irq, idx, len;
	u32 sid, tgt;

	ofdev = of_match_device(apple_dart_iommu_match, dev);
	if(!ofdev)
		return -ENODEV;

	im = devm_kzalloc(dev, sizeof(struct apple_dart_iommu), GFP_KERNEL);
	if(!im)
		return -ENOMEM;

	im->dev = &pdev->dev;
	platform_set_drvdata(pdev, im);

	im->version = ofdev->data;

	spin_lock_init(&im->dart_lock);

	dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));

	if(of_property_read_u32(pdev->dev.of_node, "page-bits", &im->page_bits) < 0)
		im->page_bits = 12;
	if(of_property_read_u32(pdev->dev.of_node, "sid-mask", &im->sid_mask) < 0)
		im->sid_mask = 15;

	if(of_property_read_bool(pdev->dev.of_node, "pcie-dart")) {
		im->is_pcie = 1;
		im->iova_offset = im->version->pcie_iova_offset;
	}

	memset(im->remap, 0xFF, sizeof(im->remap));
	len = of_property_count_elems_of_size(pdev->dev.of_node, "sid-remap", sizeof(u32));
	for(idx=0; idx<len; idx+=2) {
		if(of_property_read_u32_index(pdev->dev.of_node, "sid-remap", idx, &sid) < 0 ||
		   of_property_read_u32_index(pdev->dev.of_node, "sid-remap", idx + 1, &tgt) < 0 ||
		   sid >= DART_MAX_SID || tgt >= DART_MAX_SID) {
			dev_err(dev, "invalid sid-remap property.\n");
			return -EINVAL;
		}
		im->remap[sid] = tgt;
	}

	for(idx=0; idx<im->version->max_sub; idx++) {
		r = platform_get_resource(pdev, IORESOURCE_MEM, idx);
		if(!r) {
			if(idx < im->version->min_sub) {
				dev_err(dev, "missing mmio resource %d.\n", idx);
				return -EINVAL;
			}
			im->base[idx] = NULL;
			continue;
		}
		im->base[idx] = devm_ioremap_resource(&pdev->dev, r);
		if(IS_ERR(im->base[idx]))
			return PTR_ERR(im->base[idx]);
	}

	ret = clk_bulk_get_all(im->dev, &im->clks);
	if(ret < 0) {
		dev_err(dev, "clk_bulk_get_all failed.\n");
		return ret;
	}
	im->num_clks = ret;

	ret = clk_bulk_prepare_enable(im->num_clks, im->clks);
	if(ret) {
		dev_err(dev, "clk_bulk_prepare_enable failed.\n");
		return ret;
	}

	irq = platform_get_irq(pdev, 0);
	if(irq < 0)
		return irq;

	ret = devm_request_irq(&pdev->dev, irq, apple_dart_irq, 0, dev_name(&pdev->dev), im);
	if(ret < 0)
		return ret;

	ret = iommu_device_sysfs_add(&im->iommu, dev, NULL, node->name);
	if(ret)
		return ret;

	iommu_device_set_ops(&im->iommu, &apple_dart_iommu_ops);
	iommu_device_set_fwnode(&im->iommu, fwnode);
	ret = iommu_device_register(&im->iommu);
	if(ret)
		return ret;

	if(dev->bus->iommu_ops != &apple_dart_iommu_ops) {
		ret = bus_set_iommu(dev->bus, &apple_dart_iommu_ops);
		if(ret)
			dev_err(dev, "failed to register bus iommu driver.\n");
	}

	return ret;
}

static int apple_dart_iommu_remove(struct platform_device *pdev)
{
	return 0;
}

static struct platform_driver apple_dart_iommu_driver = {
	.probe   = apple_dart_iommu_probe,
	.remove  = apple_dart_iommu_remove,
	.driver  = {
		.name  = "apple-dart",
		.of_match_table = apple_dart_iommu_match,
	},
};
module_platform_driver(apple_dart_iommu_driver);

MODULE_DESCRIPTION("Apple SoC DART IOMMU driver");
MODULE_LICENSE("GPL v2");
