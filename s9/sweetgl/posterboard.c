/*
 * PosterBoard without Metal (Galaxy S9: no Metal driver for the Mali).
 *
 * -[PBFHomeScreenPreviewImageBuilder _imageForView:] renders the home
 * screen preview with CARenderer into an MTLTexture (MTLCreateSystemDefault
 * Device, newCommandQueue, +[CARenderer rendererWithMTLTexture:options:]);
 * with no Metal device the command queue is nil, +[NSDictionary
 * dictionaryWithObjects:forKeys:count:] throws and PosterBoard aborts, so
 * the wallpaper never gets set up. Replaced by a CPU render of the view's
 * layer (-[CALayer renderInContext:]) into a bitmap, returned the same way
 * (+[UIImage imageWithCGImage:]).
 *
 * Loaded into PosterBoard by sweetenv (DYLD_INSERT_LIBRARIES); installed
 * when the class appears (PosterBoard.framework may load after us).
 */
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <mach-o/dyld.h>
#include <math.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdbool.h>

#include "sweet_drm.h"

static SEL S(const char *n) { return sel_registerName(n); }

static bool pb_installed;

static id
pb_image_for_view(id self, SEL cmd, id view)
{
	(void)self; (void)cmd;
	static int logged;
	id layer = view ? ((id (*)(id, SEL))objc_msgSend)(view, S("layer")) : nil;
	CGRect b = layer ? ((CGRect (*)(id, SEL))objc_msgSend)(layer, S("bounds")) : CGRectZero;
	CGFloat scale = layer ? ((CGFloat (*)(id, SEL))objc_msgSend)(layer, S("contentsScale")) : 1;
	if (!(scale > 0)) {
		scale = 1;
	}
	size_t w = (size_t)ceil(b.size.width * scale), h = (size_t)ceil(b.size.height * scale);
	if (w == 0 || h == 0 || w > 8192 || h > 8192) {
		w = h = 1;
	}
	CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
	CGContextRef ctx = CGBitmapContextCreate(NULL, w, h, 8, 0, cs,
	    kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
	CGColorSpaceRelease(cs);
	id image = nil;
	if (ctx != NULL) {
		/* UIKit-style coordinates: origin top left, points */
		CGContextTranslateCTM(ctx, 0, (CGFloat)h);
		CGContextScaleCTM(ctx, scale, -scale);
		CGContextTranslateCTM(ctx, -b.origin.x, -b.origin.y);
		if (layer != nil) {
			((void (*)(id, SEL, CGContextRef))objc_msgSend)(layer, S("renderInContext:"), ctx);
		}
		CGImageRef cg = CGBitmapContextCreateImage(ctx);
		CGContextRelease(ctx);
		if (cg != NULL) {
			image = ((id (*)(id, SEL, CGImageRef))objc_msgSend)((id)objc_getClass("UIImage"), S("imageWithCGImage:"),
			    cg);
			CGImageRelease(cg);
		}
	}
	if (logged++ < 10) {
		sweet_drm_log("sweetgl: PosterBoard preview image %zux%zu (scale %.1f) on the CPU: %s", w, h, (double)scale,
		    image ? "ok" : "FAILED");
	}
	return image;
}

static void
pb_try_install(void)
{
	if (pb_installed) {
		return;
	}
	Class c = objc_getClass("PBFHomeScreenPreviewImageBuilder");
	Method m = c ? class_getInstanceMethod(c, S("_imageForView:")) : NULL;
	if (m == NULL) {
		return;
	}
	pb_installed = true;
	method_setImplementation(m, (IMP)pb_image_for_view);
	sweet_drm_log("sweetgl: PosterBoard preview images rendered on the CPU (no Metal)");
}

static void
pb_image_added(const struct mach_header *mh, intptr_t slide)
{
	(void)mh; (void)slide;
	pb_try_install();
}

void
sweetgl_posterboard_install(void)
{
	pb_try_install();
	if (!pb_installed) {
		sweet_drm_log("sweetgl: PosterBoard: preview builder class not loaded yet, waiting");
		_dyld_register_func_for_add_image(pb_image_added);
	}
}
