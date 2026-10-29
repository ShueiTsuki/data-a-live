/*
 * PandoraPlayer: a UIKit app that plays an audio file with AVAudioPlayer, for listening tests: "外放" with the
 * playback category (mediaserverd -> the default output, the speakers), "听筒" with the play-and-record category,
 * which iOS routes to the receiver. It has no bundle of
 * its own: installed inside an app bundle that is already on the home screen (Camera.app, unusable on this phone)
 * and started in place of that app's executable (boot-arg sweetexecswap=Camera:/Applications/Camera.app/
 * PandoraPlayer). The scene configuration comes from the app delegate, so the host bundle's scene manifest and
 * storyboards are not used. Prints "PLAYER:" lines to /dev/console.
 */
#import <UIKit/UIKit.h>
#import <AVFoundation/AVFoundation.h>
#include <fcntl.h>
#include <unistd.h>

static NSString *const kFile = @"/private/var/mobile/Media/audplay.m4a";

static void
say(NSString *s)
{
	static int fd = -1;
	if (fd < 0) {
		fd = open("/dev/console", O_WRONLY);
	}
	NSData *d = [[NSString stringWithFormat:@"PLAYER: %@\n", s] dataUsingEncoding:NSUTF8StringEncoding];
	if (fd >= 0) {
		(void)write(fd, d.bytes, d.length);
	}
}

@interface PlayerController : UIViewController <AVAudioPlayerDelegate>
@property (nonatomic, strong) AVAudioPlayer *player;
@property (nonatomic, strong) UIButton *button;
@property (nonatomic, strong) UIButton *ear;
@property (nonatomic, strong) UILabel *status;
@end

@implementation PlayerController

- (void)viewDidLoad
{
	[super viewDidLoad];
	self.view.backgroundColor = UIColor.blackColor;
	UILabel *title = [[UILabel alloc] init];
	title.text = @"魔王猜你想听";
	title.textColor = UIColor.whiteColor;
	title.font = [UIFont boldSystemFontOfSize:28];
	title.textAlignment = NSTextAlignmentCenter;
	title.translatesAutoresizingMaskIntoConstraints = NO;
	[self.view addSubview:title];

	self.button = [UIButton buttonWithType:UIButtonTypeSystem];
	self.button.titleLabel.font = [UIFont boldSystemFontOfSize:40];
	[self.button setTitle:@"外放" forState:UIControlStateNormal];
	[self.button setTitleColor:UIColor.whiteColor forState:UIControlStateNormal];
	self.button.backgroundColor = [UIColor colorWithRed:0.20 green:0.45 blue:0.95 alpha:1];
	self.button.layer.cornerRadius = 20;
	self.button.translatesAutoresizingMaskIntoConstraints = NO;
	[self.button addTarget:self action:@selector(toggle) forControlEvents:UIControlEventTouchUpInside];
	[self.view addSubview:self.button];

	self.ear = [UIButton buttonWithType:UIButtonTypeSystem];
	self.ear.titleLabel.font = [UIFont boldSystemFontOfSize:40];
	[self.ear setTitle:@"听筒" forState:UIControlStateNormal];
	[self.ear setTitleColor:UIColor.whiteColor forState:UIControlStateNormal];
	self.ear.backgroundColor = [UIColor colorWithRed:0.15 green:0.65 blue:0.35 alpha:1];
	self.ear.layer.cornerRadius = 20;
	self.ear.translatesAutoresizingMaskIntoConstraints = NO;
	[self.ear addTarget:self action:@selector(earpiece) forControlEvents:UIControlEventTouchUpInside];
	[self.view addSubview:self.ear];

	self.status = [[UILabel alloc] init];
	self.status.textColor = UIColor.lightGrayColor;
	self.status.font = [UIFont systemFontOfSize:18];
	self.status.textAlignment = NSTextAlignmentCenter;
	self.status.numberOfLines = 0;
	self.status.translatesAutoresizingMaskIntoConstraints = NO;
	[self.view addSubview:self.status];

	[NSLayoutConstraint activateConstraints:@[
		[title.centerXAnchor constraintEqualToAnchor:self.view.centerXAnchor],
		[title.bottomAnchor constraintEqualToAnchor:self.button.topAnchor constant:-40],
		[self.button.centerXAnchor constraintEqualToAnchor:self.view.centerXAnchor],
		[self.button.centerYAnchor constraintEqualToAnchor:self.view.centerYAnchor],
		[self.button.widthAnchor constraintEqualToConstant:220],
		[self.button.heightAnchor constraintEqualToConstant:100],
		[self.ear.centerXAnchor constraintEqualToAnchor:self.view.centerXAnchor],
		[self.ear.topAnchor constraintEqualToAnchor:self.button.bottomAnchor constant:30],
		[self.ear.widthAnchor constraintEqualToConstant:220],
		[self.ear.heightAnchor constraintEqualToConstant:100],
		[self.status.topAnchor constraintEqualToAnchor:self.ear.bottomAnchor constant:30],
		[self.status.leadingAnchor constraintEqualToAnchor:self.view.leadingAnchor constant:20],
		[self.status.trailingAnchor constraintEqualToAnchor:self.view.trailingAnchor constant:-20],
	]];

	NSError *err = nil;
	AVAudioSession *session = AVAudioSession.sharedInstance;
	if (![session setCategory:AVAudioSessionCategoryPlayback error:&err]) {
		say([NSString stringWithFormat:@"setCategory: %@", err]);
	}
	self.player = [[AVAudioPlayer alloc] initWithContentsOfURL:[NSURL fileURLWithPath:kFile] error:&err];
	if (self.player == nil) {
		self.status.text = [NSString stringWithFormat:@"打不开文件：%@", err.localizedDescription];
		say([NSString stringWithFormat:@"cannot open %@: %@", kFile, err]);
		self.button.enabled = NO;
		return;
	}
	self.player.delegate = self;
	[self.player prepareToPlay];
	self.status.text = [NSString stringWithFormat:@"%.1f 秒", self.player.duration];
	say([NSString stringWithFormat:@"loaded %@, %.2f s, %lu channels", kFile, self.player.duration,
	    (unsigned long)self.player.numberOfChannels]);
}

/* Stops what plays, sets CATEGORY and plays from the start; the route iOS picked goes to the status line. */
- (void)playWith:(AVAudioSessionCategory)category name:(NSString *)name
{
	NSError *err = nil;
	AVAudioSession *session = AVAudioSession.sharedInstance;
	[self.player stop];
	self.player.currentTime = 0;
	if (![session setCategory:category error:&err]) {
		say([NSString stringWithFormat:@"setCategory %@: %@", category, err]);
	}
	if (![session setActive:YES error:&err]) {
		say([NSString stringWithFormat:@"setActive: %@", err]);
	}
	BOOL ok = [self.player play];
	NSMutableArray *outs = [NSMutableArray array];
	for (AVAudioSessionPortDescription *port in session.currentRoute.outputs) {
		[outs addObject:[NSString stringWithFormat:@"%@ (%@)", port.portName, port.portType]];
	}
	NSString *route = [outs componentsJoinedByString:@", "];
	self.status.text = [NSString stringWithFormat:@"%@：%@", name, route];
	say([NSString stringWithFormat:@"%@ play: %d, category %@, route %@", name, ok, category, route]);
}

- (void)toggle
{
	if (self.player.playing) {
		[self.player stop];
		say(@"stopped");
		self.status.text = @"停";
		return;
	}
	[self playWith:AVAudioSessionCategoryPlayback name:@"外放"];
}

- (void)earpiece
{
	if (self.player.playing) {
		[self.player stop];
		say(@"stopped");
		self.status.text = @"停";
		return;
	}
	[self playWith:AVAudioSessionCategoryPlayAndRecord name:@"听筒"];
}

- (void)audioPlayerDidFinishPlaying:(AVAudioPlayer *)player successfully:(BOOL)flag
{
	player.currentTime = 0;
	say([NSString stringWithFormat:@"finished: %d", flag]);
}

@end

@interface PlayerSceneDelegate : UIResponder <UIWindowSceneDelegate>
@property (nonatomic, strong) UIWindow *window;
@end

@implementation PlayerSceneDelegate

- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session
	options:(UISceneConnectionOptions *)options
{
	self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene];
	self.window.rootViewController = [[PlayerController alloc] init];
	[self.window makeKeyAndVisible];
	say(@"scene connected");
}

@end

@interface PlayerAppDelegate : UIResponder <UIApplicationDelegate>
@end

@implementation PlayerAppDelegate

- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:(NSDictionary *)options
{
	say(@"launched");
	return YES;
}

- (UISceneConfiguration *)application:(UIApplication *)application
	configurationForConnectingSceneSession:(UISceneSession *)session options:(UISceneConnectionOptions *)options
{
	UISceneConfiguration *c = [[UISceneConfiguration alloc] initWithName:@"Player" sessionRole:session.role];
	c.delegateClass = PlayerSceneDelegate.class;
	c.storyboard = nil;
	return c;
}

@end

int
main(int argc, char *argv[])
{
	@autoreleasepool {
		say(@"started");
		return UIApplicationMain(argc, argv, nil, NSStringFromClass(PlayerAppDelegate.class));
	}
}
