// SPDX-License-Identifier: GPL-2.0+
/*
 * Freescale i.MX drm driver
 *
 * Copyright (C) 2011 Sascha Hauer, Pengutronix
 */

#include <linux/component.h>
#include <linux/device.h>
#include <linux/dma-buf.h>
#include <linux/module.h>
#include <linux/platform_device.h>

#include <video/imx-ipu-v3.h>
#include <video/imx-lcdif.h>
#include <video/imx-lcdifv3.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_fb_cma_helper.h>
#include <drm/drm_fb_helper.h>
#include <drm/drm_gem_cma_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_of.h>
#include <drm/drm_plane_helper.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_vblank.h>
#include <video/dpu.h>

#include "imx-drm.h"

static int legacyfb_depth = 16;
module_param(legacyfb_depth, int, 0444);

DEFINE_DRM_GEM_CMA_FOPS(imx_drm_driver_fops);

void imx_drm_connector_destroy(struct drm_connector *connector)
{
	drm_connector_unregister(connector);
	drm_connector_cleanup(connector);
}
EXPORT_SYMBOL_GPL(imx_drm_connector_destroy);

void imx_drm_encoder_destroy(struct drm_encoder *encoder)
{
	drm_encoder_cleanup(encoder);
}
EXPORT_SYMBOL_GPL(imx_drm_encoder_destroy);

int imx_drm_encoder_parse_of(struct drm_device *drm,
	struct drm_encoder *encoder, struct device_node *np)
{
	uint32_t crtc_mask = drm_of_find_possible_crtcs(drm, np);

	/*
	 * If we failed to find the CRTC(s) which this encoder is
	 * supposed to be connected to, it's because the CRTC has
	 * not been registered yet.  Defer probing, and hope that
	 * the required CRTC is added later.
	 */
	if (crtc_mask == 0)
		return -EPROBE_DEFER;

	encoder->possible_crtcs = crtc_mask;

	/* FIXME: this is the mask of outputs which can clone this output. */
	encoder->possible_clones = ~0;

	return 0;
}
EXPORT_SYMBOL_GPL(imx_drm_encoder_parse_of);

static const struct drm_ioctl_desc imx_drm_ioctls[] = {
	/* none so far */
};

static struct drm_driver imx_drm_driver = {
	.driver_features	= DRIVER_MODESET | DRIVER_GEM | DRIVER_ATOMIC,
	.gem_free_object_unlocked = drm_gem_cma_free_object,
	.gem_vm_ops		= &drm_gem_cma_vm_ops,
	.dumb_create		= drm_gem_cma_dumb_create,

	.prime_handle_to_fd	= drm_gem_prime_handle_to_fd,
	.prime_fd_to_handle	= drm_gem_prime_fd_to_handle,
	.gem_prime_get_sg_table	= drm_gem_cma_prime_get_sg_table,
	.gem_prime_import_sg_table = drm_gem_cma_prime_import_sg_table,
	.gem_prime_vmap		= drm_gem_cma_prime_vmap,
	.gem_prime_vunmap	= drm_gem_cma_prime_vunmap,
	.gem_prime_mmap		= drm_gem_cma_prime_mmap,
	.ioctls			= imx_drm_ioctls,
	.num_ioctls		= ARRAY_SIZE(imx_drm_ioctls),
	.fops			= &imx_drm_driver_fops,
	.name			= "imx-drm",
	.desc			= "i.MX DRM graphics",
	.date			= "20120507",
	.major			= 1,
	.minor			= 0,
	.patchlevel		= 0,
};

static int compare_of(struct device *dev, void *data)
{
	struct device_node *np = data;

	/* Special case for DI, dev->of_node may not be set yet */
	if (strcmp(dev->driver->name, "imx-ipuv3-crtc") == 0) {
		struct ipu_client_platformdata *pdata = dev->platform_data;

		return pdata->of_node == np;
	} else if (strcmp(dev->driver->name, "imx-dpu-crtc") == 0) {
		struct dpu_client_platformdata *pdata = dev->platform_data;

		return pdata->of_node == np;
	} else if (strcmp(dev->driver->name, "imx-lcdif-crtc") == 0 ||
		   strcmp(dev->driver->name, "imx-lcdifv3-crtc") == 0) {
		struct lcdif_client_platformdata *pdata = dev->platform_data;
#if IS_ENABLED(CONFIG_DRM_FBDEV_EMULATION)
		/* set legacyfb_depth to be 32 for lcdif, since
		 * default format of the connectors attached to
		 * lcdif is usually RGB888
		 */
		if (pdata->of_node == np)
			legacyfb_depth = 32;
#endif

		return pdata->of_node == np;
	}

	/* This is a special case for dpu bliteng. */
	if (strcmp(dev->driver->name, "imx-drm-dpu-bliteng") == 0) {
		struct dpu_client_platformdata *pdata = dev->platform_data;

		return pdata->of_node == np;
	}

	/* Special case for LDB, one device for two channels */
	if (of_node_name_eq(np, "lvds-channel")) {
#if IS_ENABLED(CONFIG_DRM_FBDEV_EMULATION)
		/* Overwrite the pixel depth for LDB */
		if (of_property_read_u32(np, "digi,bits-per-pixel", &legacyfb_depth))
			dev_info(dev,"Set pixel depth to %d bpp\n",legacyfb_depth);
#endif
		np = of_get_parent(np);
		of_node_put(np);
	}

	return dev->of_node == np;
}

static const char *const imx_drm_dpu_comp_parents[] = {
	"fsl,imx8qm-dpu",
	"fsl,imx8qxp-dpu",
};

static bool imx_drm_parent_is_compatible(struct device *dev,
					 const char *const comp_parents[],
					 int comp_parents_size)
{
	struct device_node *port, *parent;
	bool ret = false;
	int i;

	port = of_parse_phandle(dev->of_node, "ports", 0);
	if (!port)
		return ret;

	parent = of_get_parent(port);

	for (i = 0; i < comp_parents_size; i++) {
		if (of_device_is_compatible(parent, comp_parents[i])) {
			ret = true;
			break;
		}
	}

	of_node_put(parent);

	of_node_put(port);

	return ret;
}

static inline bool has_dpu(struct device *dev)
{
	return imx_drm_parent_is_compatible(dev, imx_drm_dpu_comp_parents,
					ARRAY_SIZE(imx_drm_dpu_comp_parents));
}

static void add_dpu_bliteng_components(struct device *dev,
				       struct component_match **matchptr)
{
	/*
	 * As there may be two dpu bliteng device,
	 * so need add something in compare data to distinguish.
	 * Use its parent dpu's of_node as the data here.
	 */
	struct device_node *port, *parent;
	/* assume max dpu number is 8 */
	struct device_node *dpu[8];
	int num_dpu = 0;
	int i, j;
	bool found = false;

	for (i = 0; ; i++) {
		port = of_parse_phandle(dev->of_node, "ports", i);
		if (!port)
			break;

		parent = of_get_parent(port);

		for (j = 0; j < num_dpu; j++) {
			if (dpu[j] == parent) {
				found = true;
				break;
			}
		}

		if (found) {
			found = false;
		} else {
			if (num_dpu >= ARRAY_SIZE(dpu)) {
				dev_err(dev, "The number of found dpu is greater than max [%ld].\n",
					ARRAY_SIZE(dpu));
				of_node_put(parent);
				of_node_put(port);
				break;
			}

			dpu[num_dpu] = parent;
			num_dpu++;

			component_match_add(dev, matchptr, compare_of, parent);
		}

		of_node_put(parent);
		of_node_put(port);
	}
}

static int imx_drm_bind(struct device *dev)
{
	struct drm_device *drm;
	int ret;

	if (has_dpu(dev))
		imx_drm_driver.driver_features |= DRIVER_RENDER;

	drm = drm_dev_alloc(&imx_drm_driver, dev);
	if (IS_ERR(drm))
		return PTR_ERR(drm);

	/*
	 * enable drm irq mode.
	 * - with irq_enabled = true, we can use the vblank feature.
	 *
	 * P.S. note that we wouldn't use drm irq handler but
	 *      just specific driver own one instead because
	 *      drm framework supports only one irq handler and
	 *      drivers can well take care of their interrupts
	 */
	drm->irq_enabled = true;

	/*
	 * set max width and height as default value(4096x4096).
	 * this value would be used to check framebuffer size limitation
	 * at drm_mode_addfb().
	 */
	drm->mode_config.min_width = 1;
	drm->mode_config.min_height = 1;
	drm->mode_config.max_width = 4096;
	drm->mode_config.max_height = 4096;
	drm->mode_config.normalize_zpos = true;

	drm_mode_config_init(drm);

	ret = drm_vblank_init(drm, MAX_CRTC);
	if (ret)
		goto err_kms;

	/* Now try and bind all our sub-components */
	ret = component_bind_all(dev, drm);
	if (ret)
		goto err_kms;

	drm_mode_config_reset(drm);

	/*
	 * All components are now initialised, so setup the fb helper.
	 * The fb helper takes copies of key hardware information, so the
	 * crtcs/connectors/encoders must not change after this point.
	 */
	if (legacyfb_depth != 16 && legacyfb_depth != 32) {
		dev_warn(dev, "Invalid legacyfb_depth.  Defaulting to 16bpp\n");
		legacyfb_depth = 16;
	}

	drm_kms_helper_poll_init(drm);

	ret = drm_dev_register(drm, 0);
	if (ret)
		goto err_poll_fini;

	drm_fbdev_generic_setup(drm, legacyfb_depth);

	dev_set_drvdata(dev, drm);

	return 0;

err_poll_fini:
	drm_kms_helper_poll_fini(drm);
	component_unbind_all(drm->dev, drm);
err_kms:
	drm_mode_config_cleanup(drm);
	drm_dev_put(drm);

	return ret;
}

static void imx_drm_unbind(struct device *dev)
{
	struct drm_device *drm = dev_get_drvdata(dev);

	if (has_dpu(dev))
		imx_drm_driver.driver_features &= ~DRIVER_RENDER;

	drm_dev_unregister(drm);

	drm_kms_helper_poll_fini(drm);

	component_unbind_all(drm->dev, drm);

	drm_mode_config_cleanup(drm);

	dev_set_drvdata(dev, NULL);

	drm_dev_put(drm);
}

static const struct component_master_ops imx_drm_ops = {
	.bind = imx_drm_bind,
	.unbind = imx_drm_unbind,
};

static int imx_drm_platform_probe(struct platform_device *pdev)
{
	struct component_match *match = NULL;
	int ret;

	if (has_dpu(&pdev->dev))
		add_dpu_bliteng_components(&pdev->dev, &match);

	ret = drm_of_component_probe_with_match(&pdev->dev, match, compare_of,
						&imx_drm_ops);

	if (!ret)
		ret = dma_set_coherent_mask(&pdev->dev, DMA_BIT_MASK(32));

	return ret;
}

static int imx_drm_platform_remove(struct platform_device *pdev)
{
	component_master_del(&pdev->dev, &imx_drm_ops);
	return 0;
}

#ifdef CONFIG_PM_SLEEP
static int imx_drm_suspend(struct device *dev)
{
	struct drm_device *drm_dev = dev_get_drvdata(dev);

	return drm_mode_config_helper_suspend(drm_dev);
}

static int imx_drm_resume(struct device *dev)
{
	struct drm_device *drm_dev = dev_get_drvdata(dev);

	return drm_mode_config_helper_resume(drm_dev);
}
#endif

static SIMPLE_DEV_PM_OPS(imx_drm_pm_ops, imx_drm_suspend, imx_drm_resume);

static const struct of_device_id imx_drm_dt_ids[] = {
	{ .compatible = "fsl,imx-display-subsystem", },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, imx_drm_dt_ids);

static struct platform_driver imx_drm_pdrv = {
	.probe		= imx_drm_platform_probe,
	.remove		= imx_drm_platform_remove,
	.driver		= {
		.name	= "imx-drm",
		.pm	= &imx_drm_pm_ops,
		.of_match_table = imx_drm_dt_ids,
	},
};
module_platform_driver(imx_drm_pdrv);

MODULE_AUTHOR("Sascha Hauer <s.hauer@pengutronix.de>");
MODULE_DESCRIPTION("i.MX drm driver core");
MODULE_LICENSE("GPL");
