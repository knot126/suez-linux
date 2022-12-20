/*
 * Copyright 2014 Canonical
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors: Andreas Pokorny
 */

#include <drm/drm_drv.h>
#include <drm/drm_prime.h>
#include <linux/virtio_dma_buf.h>
#include <linux/dma-buf.h>
#include "virtgpu_drv.h"

static int virtgpu_virtio_get_uuid(struct dma_buf *buf,
				  uuid_t *uuid)
{
	struct drm_gem_object *obj = buf->priv;
	struct virtio_gpu_object *bo = gem_to_virtio_gpu_obj(obj);
	struct virtio_gpu_device *vgdev = obj->dev->dev_private;

	wait_event(vgdev->resp_wq, bo->uuid_state != UUID_INITIALIZING);
	if (bo->uuid_state != UUID_INITIALIZED)
		return -ENODEV;

	uuid_copy(uuid, &bo->uuid);

	return 0;
}

const struct virtio_dma_buf_ops virtgpu_dmabuf_ops =  {
	.ops = {
		.cache_sgt_mapping = true,
		.attach = virtio_dma_buf_attach,
		.detach = drm_gem_map_detach,
		.map_dma_buf = drm_gem_map_dma_buf,
		.unmap_dma_buf = drm_gem_unmap_dma_buf,
		.release = drm_gem_dmabuf_release,
		.mmap = drm_gem_dmabuf_mmap,
		.vmap = drm_gem_dmabuf_vmap,
		.vunmap = drm_gem_dmabuf_vunmap,
	},
	.device_attach = drm_gem_map_attach,
	.get_uuid = virtgpu_virtio_get_uuid,
};

struct dma_buf *virtgpu_gem_prime_export(struct drm_gem_object *obj,
					int flags)
{
	struct dma_buf *buf;
	struct drm_device *dev = obj->dev;
	struct virtio_gpu_device *vgdev = dev->dev_private;
	struct virtio_gpu_object *bo = gem_to_virtio_gpu_obj(obj);
	int ret = 0;
	DEFINE_DMA_BUF_EXPORT_INFO(exp_info);

	if (vgdev->has_resource_assign_uuid) {
		ret = virtio_gpu_cmd_resource_assign_uuid(vgdev, bo);
		if (ret)
			return ERR_PTR(ret);
	} else {
		bo->uuid_state = UUID_INITIALIZATION_FAILED;
	}

	exp_info.ops = &virtgpu_dmabuf_ops.ops;
	exp_info.size = obj->size;
	exp_info.flags = flags;
	exp_info.priv = obj;
	exp_info.resv = obj->resv;

	buf = virtio_dma_buf_export(&exp_info);
	if (IS_ERR(buf))
		return buf;

	drm_dev_get(dev);
	drm_gem_object_get(obj);

	return buf;
}

struct drm_gem_object *virtgpu_gem_prime_import(struct drm_device *dev,
						struct dma_buf *buf)
{
	struct drm_gem_object *obj;

	if (buf->ops == &virtgpu_dmabuf_ops.ops) {
		obj = buf->priv;
		if (obj->dev == dev) {
			/*
			 * Importing dmabuf exported from our own gem increases
			 * refcount on gem itself instead of f_count of dmabuf.
			 */
			drm_gem_object_get(obj);
			return obj;
		}
	}

	return drm_gem_prime_import(dev, buf);
}

struct sg_table *virtgpu_gem_prime_get_sg_table(struct drm_gem_object *obj)
{
	struct virtio_gpu_object *bo = gem_to_virtio_gpu_obj(obj);

	if (!bo->tbo.ttm->pages || !bo->tbo.ttm->num_pages)
		/* should not happen */
		return ERR_PTR(-EINVAL);

	return drm_prime_pages_to_sg(bo->tbo.ttm->pages,
				     bo->tbo.ttm->num_pages);
}

struct drm_gem_object *virtgpu_gem_prime_import_sg_table(
	struct drm_device *dev, struct dma_buf_attachment *attach,
	struct sg_table *table)
{
	return ERR_PTR(-ENODEV);
}

void *virtgpu_gem_prime_vmap(struct drm_gem_object *obj)
{
	struct virtio_gpu_object *bo = gem_to_virtio_gpu_obj(obj);
	int ret;

	ret = virtio_gpu_object_kmap(bo);
	if (ret)
		return NULL;
	return bo->vmap;
}

void virtgpu_gem_prime_vunmap(struct drm_gem_object *obj, void *vaddr)
{
	virtio_gpu_object_kunmap(gem_to_virtio_gpu_obj(obj));
}

int virtgpu_gem_prime_mmap(struct drm_gem_object *obj,
			   struct vm_area_struct *vma)
{
	return drm_gem_prime_mmap(obj, vma);
}
