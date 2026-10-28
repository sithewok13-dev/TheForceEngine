/*
 * The first screen, shown until the player's own Dark Forces files are in the
 * app's Documents folder (Files > On My iPhone > Dark Forces), and the search
 * that finds them there. Nothing of the game's is ever bundled.
 *
 * The files can be copied in directly or as the whole game folder, whatever
 * its name: the Dark Forces Remaster's (Steam: "STAR WARS Dark Forces
 * Remaster", GOG) or the original's (Steam: "dark forces" with its "Game"
 * folder inside, GOG, CD). The folder that holds DARK.GOB is the game; the
 * search ignores upper and lower case, as copies from Windows can be either.
 *
 * Compiled with ARC.
 */

#import <UIKit/UIKit.h>
#import <AVFoundation/AVFoundation.h>
#include <stdbool.h>
#include <string.h>

#include "iosApp.h"

// TFE's own folder in Documents (settings, saves, screenshots, the log)
static NSString * const kTFEFolder = @"TheForceEngine";

static NSString *DocumentsPath(void) {
	return NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES)[0];
}

const char *IOS_DocumentsPath(void) {
	static char path[1024];
	snprintf(path, sizeof(path), "%s/", DocumentsPath().fileSystemRepresentation);
	return path;
}

static BOOL IsDirectory(NSString *path) {
	BOOL dir = NO;
	return [[NSFileManager defaultManager] fileExistsAtPath:path isDirectory:&dir] && dir;
}

// true if dir holds a file called name, in any case, of more than one byte
static BOOL HasFile(NSString *dir, NSString *name) {
	NSFileManager *fm = [NSFileManager defaultManager];
	for (NSString *entry in [fm contentsOfDirectoryAtPath:dir error:nil] ?: @[]) {
		if ([entry caseInsensitiveCompare:name] == NSOrderedSame) {
			NSDictionary *attrs = [fm attributesOfItemAtPath:[dir stringByAppendingPathComponent:entry] error:nil];
			return attrs && [attrs fileSize] > 1;
		}
	}
	return NO;
}

// the folder with DARK.GOB: Documents itself, a folder in it, or a folder in
// one of those. A remaster folder (with DarkEX.kpf) is preferred when there
// are several.
static NSString *FindGameFolder(void) {
	NSFileManager *fm = [NSFileManager defaultManager];
	NSString *docs = DocumentsPath();
	if (HasFile(docs, @"DARK.GOB")) {
		return docs;
	}
	NSMutableArray<NSString *> *found = [NSMutableArray array];
	NSArray<NSString *> *top = [[fm contentsOfDirectoryAtPath:docs error:nil] ?: @[]
		sortedArrayUsingSelector:@selector(localizedStandardCompare:)];
	for (NSString *name in top) {
		if ([name hasPrefix:@"."] || [name isEqualToString:kTFEFolder]) continue;
		NSString *dir = [docs stringByAppendingPathComponent:name];
		if (!IsDirectory(dir)) continue;
		if (HasFile(dir, @"DARK.GOB")) {
			[found addObject:dir];
			continue;
		}
		NSArray<NSString *> *inner = [[fm contentsOfDirectoryAtPath:dir error:nil] ?: @[]
			sortedArrayUsingSelector:@selector(localizedStandardCompare:)];
		for (NSString *innerName in inner) {
			NSString *innerDir = [dir stringByAppendingPathComponent:innerName];
			if (![innerName hasPrefix:@"."] && IsDirectory(innerDir) && HasFile(innerDir, @"DARK.GOB")) {
				[found addObject:innerDir];
			}
		}
	}
	for (NSString *dir in found) {
		if (HasFile(dir, @"DarkEX.kpf")) return dir;
	}
	return found.firstObject;
}

bool IOS_FindGameFolder(char *outPath, size_t size) {
	@autoreleasepool {
		NSString *dir = FindGameFolder();
		if (!dir || size < 2) return false;
		snprintf(outPath, size, "%s/", dir.fileSystemRepresentation);
		return true;
	}
}

@interface IOSGameDataViewController : UIViewController
@property (nonatomic) BOOL ready;
@property (nonatomic, strong) UILabel *missingLabel;
@end

@implementation IOSGameDataViewController

- (void)viewDidLoad {
	[super viewDidLoad];
	self.view.backgroundColor = UIColor.blackColor;

	UILabel *title = [UILabel new];
	title.text = @"Game files needed";
	title.font = [UIFont systemFontOfSize:28 weight:UIFontWeightBold];
	title.textColor = UIColor.whiteColor;

	NSString *device = UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPad ? @"iPad" : @"iPhone";
	UILabel *body = [UILabel new];
	body.text = [NSString stringWithFormat:
		@"Dark Forces plays the files from your own copy of the game.\n\n"
		@"Copy your game folder into this app's folder: in the Files app, On My %@ > Dark Forces. "
		@"From a computer, Finder (Mac) or iTunes (Windows) can copy it in too, under Files for this app.\n\n"
		@"Dark Forces Remaster (Steam or GOG): its whole folder, for example \"STAR WARS Dark Forces Remaster\". "
		@"The large \"movies\" folder inside it can be left out.\n\n"
		@"The original Dark Forces (Steam, GOG or CD): the folder with DARK.GOB in it, with its LFD folder.\n\n"
		@"The game starts by itself once DARK.GOB is found.", device];
	body.font = [UIFont systemFontOfSize:17];
	body.textColor = [UIColor colorWithWhite:0.85 alpha:1];
	body.numberOfLines = 0;

	self.missingLabel = [UILabel new];
	self.missingLabel.font = [UIFont systemFontOfSize:15 weight:UIFontWeightSemibold];
	self.missingLabel.textColor = [UIColor colorWithRed:1 green:0.35 blue:0.3 alpha:1];
	self.missingLabel.numberOfLines = 0;

	UIButton *check = [UIButton buttonWithType:UIButtonTypeSystem];
	[check setTitle:@"Check Again" forState:UIControlStateNormal];
	check.titleLabel.font = [UIFont systemFontOfSize:20 weight:UIFontWeightSemibold];
	[check setTitleColor:UIColor.blackColor forState:UIControlStateNormal];
	check.backgroundColor = [UIColor colorWithRed:0.95 green:0.75 blue:0.2 alpha:1];
	check.layer.cornerRadius = 10;
	[check addTarget:self action:@selector(checkAgain) forControlEvents:UIControlEventTouchUpInside];
	[check.widthAnchor constraintGreaterThanOrEqualToConstant:180].active = YES;
	[check.heightAnchor constraintEqualToConstant:48].active = YES;

	UIStackView *stack = [[UIStackView alloc] initWithArrangedSubviews:@[title, body, self.missingLabel, check]];
	stack.axis = UILayoutConstraintAxisVertical;
	stack.alignment = UIStackViewAlignmentLeading;
	stack.spacing = 16;
	stack.translatesAutoresizingMaskIntoConstraints = NO;

	UIScrollView *scroll = [UIScrollView new];
	scroll.translatesAutoresizingMaskIntoConstraints = NO;
	[scroll addSubview:stack];
	[self.view addSubview:scroll];

	UILayoutGuide *safe = self.view.safeAreaLayoutGuide;
	[NSLayoutConstraint activateConstraints:@[
		[scroll.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor constant:24],
		[scroll.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor constant:-24],
		[scroll.topAnchor constraintEqualToAnchor:safe.topAnchor constant:16],
		[scroll.bottomAnchor constraintEqualToAnchor:safe.bottomAnchor constant:-16],
		[stack.leadingAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.leadingAnchor],
		[stack.trailingAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.trailingAnchor],
		[stack.topAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.topAnchor],
		[stack.bottomAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.bottomAnchor],
		[stack.widthAnchor constraintEqualToAnchor:scroll.frameLayoutGuide.widthAnchor],
	]];

	// coming back from the Files app checks again by itself
	[[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(checkAgain)
												 name:UIApplicationDidBecomeActiveNotification object:nil];
	[self checkAgain];
}

- (void)dealloc {
	[[NSNotificationCenter defaultCenter] removeObserver:self];
}

- (BOOL)prefersStatusBarHidden {
	return YES;
}

- (UIInterfaceOrientationMask)supportedInterfaceOrientations {
	return UIInterfaceOrientationMaskLandscape;
}

- (void)checkAgain {
	if (FindGameFolder()) {
		self.ready = YES;
		return;
	}
	self.missingLabel.text = @"Not found yet: DARK.GOB";
}

@end

void IOS_AppInit(void) {
	@autoreleasepool {
		// The game's sound plays with the phone on silent, as in the owner's
		// other ports (iOS's default, Solo Ambient, follows the silent switch).
		// SDL sets the category again when it opens audio, from the
		// SDL_HINT_AUDIO_CATEGORY hint, which main() sets to "playback" too.
		[[AVAudioSession sharedInstance] setCategory:AVAudioSessionCategoryPlayback error:nil];
		[[AVAudioSession sharedInstance] setActive:YES error:nil];

		// TFE's own folder, so the app shows up in Files right away, and its
		// internal one in Application Support (which a new install lacks;
		// TFE only makes the last folder of a path)
		NSFileManager *fm = [NSFileManager defaultManager];
		[fm createDirectoryAtPath:[DocumentsPath() stringByAppendingPathComponent:kTFEFolder]
	  withIntermediateDirectories:YES attributes:nil error:nil];
		NSString *support = NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory, NSUserDomainMask, YES)[0];
		[fm createDirectoryAtPath:[support stringByAppendingPathComponent:kTFEFolder]
	  withIntermediateDirectories:YES attributes:nil error:nil];

		if (FindGameFolder()) {
			return;
		}

		UIWindowScene *scene = nil;
		for (UIScene *s in UIApplication.sharedApplication.connectedScenes) {
			if ([s isKindOfClass:UIWindowScene.class]) {
				scene = (UIWindowScene *)s;
				break;
			}
		}
		UIWindow *window = scene ? [[UIWindow alloc] initWithWindowScene:scene]
								 : [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
		IOSGameDataViewController *vc = [IOSGameDataViewController new];
		window.rootViewController = vc;
		window.windowLevel = UIWindowLevelNormal + 1;
		[window makeKeyAndVisible];

		while (!vc.ready) {
			@autoreleasepool {
				CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, true);
			}
		}

		window.hidden = YES;
		window.rootViewController = nil;
	}
}
