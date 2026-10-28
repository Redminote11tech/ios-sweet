// Adapted from syncmodz/ios-s9 646570b, MIT; see LICENSE.
// Home background inserted beneath icons; device validation required.
/*
 * S9Wallpaper: a static lock + home screen wallpaper for the Galaxy S9 iOS port.
 *
 * On iOS 16 the wallpaper is a PosterBoard "poster", and this root's
 * first-boot LaunchServices database has none of the system poster provider
 * plug-ins (they live inside private frameworks), so PosterBoard has nothing
 * to show and both screens stay black. This tweak (ElleKit, SpringBoard only)
 * puts an image on top of SpringBoard's wallpaper window(s). With no poster
 * the only one is _SBWallpaperSecureWindow, so today this is the lock screen
 * background; the home screen stays black (SBHomeScreenWindow: future work).
 *
 * Defensive by design (a tweak that blocks while SpringBoard loads leaves the
 * phone on the Apple logo): nothing runs in the constructor except one
 * notification observer; all UIKit work waits until SpringBoard has finished
 * launching, the JPEG is decoded off the main thread, and the file
 * /var/mobile/Library/S9Wallpaper/disabled turns the tweak off.
 *
 * The image is /var/mobile/Library/S9Wallpaper/wallpaper.jpg (replace it and
 * respring). It is decoded once into an opaque bitmap of exactly the screen's
 * pixel size, so CoreAnimation's CPU renderer copies it 1:1 (no scaling, no
 * blending) and skips whatever is below it. What it did goes to log.txt there.
 */
#import <UIKit/UIKit.h>
#include <unistd.h>

#define DIR		"/var/mobile/Library/S9Wallpaper/"
static const NSInteger kTag = 0x53395750;	/* 'S9WP' */
static UIImage *gImage;
static BOOL gLoading;

static void wlog(NSString *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	NSString *s = [[NSString alloc] initWithFormat:fmt arguments:ap];
	va_end(ap);
	FILE *f = fopen("/dev/null", "a");
	if (f != NULL) {
		fprintf(f, "S9Wallpaper: %s\n", s.UTF8String);
		fclose(f);
	}
}

static void attach(void)
{
    NSMutableSet<UIWindow *> *windows = [NSMutableSet setWithArray:UIApplication.sharedApplication.windows];
    for (UIScene *scene in UIApplication.sharedApplication.connectedScenes)
        if ([scene isKindOfClass:UIWindowScene.class])
            [windows addObjectsFromArray:((UIWindowScene *)scene).windows];
    for (UIWindow *w in windows) {
        NSString *name = NSStringFromClass(w.class);
        BOOL home = [name isEqualToString:@"SBHomeScreenWindow"];
        BOOL wallpaper = [name rangeOfString:@"Wallpaper"].location != NSNotFound;
        if (!home && !wallpaper) continue;
        UIView *host = home ? w.rootViewController.view : w;
        if (!host) continue;
        UIImageView *iv = (UIImageView *)[host viewWithTag:kTag];
        if (!iv) {
            iv = [[UIImageView alloc] initWithImage:gImage];
            iv.tag = kTag;
            iv.frame = host.bounds;
            iv.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
            iv.contentMode = UIViewContentModeScaleAspectFill;
            iv.userInteractionEnabled = NO;
            iv.isAccessibilityElement = NO;
            iv.opaque = YES;
            if (home) {
                w.backgroundColor = UIColor.clearColor;
                host.backgroundColor = UIColor.clearColor;
                [host insertSubview:iv atIndex:0];
            } else {
                [host addSubview:iv];
            }
            wlog(@"attached %@ wallpaper to %@ root %@", home ? @"home" : @"lock", name, NSStringFromClass(host.class));
        }
        if (home) [host sendSubviewToBack:iv];
        else [host bringSubviewToFront:iv];
    }
}

static void apply(void)
{
	if (UIApplication.sharedApplication == nil)
		return;
	if (gImage != nil) {
		attach();
		return;
	}
	if (gLoading)
		return;
	gLoading = YES;
	UIScreen *screen = UIScreen.mainScreen;
	CGSize pts = screen.bounds.size;
	CGFloat scale = screen.scale;
	dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
		UIImage *src = [UIImage imageWithContentsOfFile:@DIR "wallpaper.jpg"];
		UIImage *img = nil;
		if (src != nil && src.size.width > 0 && src.size.height > 0) {
			UIGraphicsImageRendererFormat *fmt = [UIGraphicsImageRendererFormat new];
			fmt.scale = scale;
			fmt.opaque = YES;
			UIGraphicsImageRenderer *r = [[UIGraphicsImageRenderer alloc] initWithSize:pts format:fmt];
			img = [r imageWithActions:^(UIGraphicsImageRendererContext *ctx) {
				/* aspect fill, centered */
				CGSize s = src.size;
				CGFloat k = MAX(pts.width / s.width, pts.height / s.height);
				CGSize d = CGSizeMake(s.width * k, s.height * k);
				[src drawInRect:CGRectMake((pts.width - d.width) / 2, (pts.height - d.height) / 2, d.width, d.height)];
			}];
		}
		dispatch_async(dispatch_get_main_queue(), ^{
			gLoading = NO;
			if (img == nil) {
				wlog(@"no image at " DIR "wallpaper.jpg");
				return;
			}
			gImage = img;
			wlog(@"image %.0fx%.0f @%.0fx", pts.width, pts.height, scale);
			attach();
		});
	});
}

static void apply_after(double seconds)
{
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(seconds * NSEC_PER_SEC)),
	    dispatch_get_main_queue(), ^{ apply(); });
}

__attribute__((constructor)) static void init(void)
{
	if (strcmp(getprogname(), "SpringBoard") != 0 || access(DIR "disabled", F_OK) == 0)
		return;
	/* String names: no UIKit symbol is touched while SpringBoard is loading. */
	[NSNotificationCenter.defaultCenter addObserverForName:@"UIApplicationDidFinishLaunchingNotification"
	    object:nil queue:nil usingBlock:^(NSNotification *n) {
		wlog(@"SpringBoard pid %d finished launching", getpid());
		[NSNotificationCenter.defaultCenter addObserverForName:@"UIWindowDidBecomeVisibleNotification"
		    object:nil queue:nil usingBlock:^(NSNotification *m) { apply_after(0.5); }];
		for (double t = 2; t <= 64; t *= 2)
			apply_after(t);
	}];
}
