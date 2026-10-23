// ChargeLimit 配套 App：显示后台守护进程（chargelimitd）的状态，并一键跳到「设置 → 充电限制」。
// 真正的开关和参数在系统设置里（Settings.bundle），本 App 不限制充电，只负责展示。
#import <UIKit/UIKit.h>

static NSString *const kStatusText = @"statusText";
static NSString *const kStatusTime = @"statusTime";
// 超过这么久没有更新状态，就认为守护进程没在运行（守护进程至少每 60 秒刷新一次）
static const NSTimeInterval kStaleSeconds = 150;

@interface CLViewController : UIViewController
@end

@implementation CLViewController {
    UILabel *_statusLabel;
    UILabel *_detailLabel;
    NSTimer *_timer;
}

static UILabel *MakeLabel(CGFloat size, UIFontWeight weight, UIColor *color) {
    UILabel *l = [[UILabel alloc] init];
    l.font = [UIFont systemFontOfSize:size weight:weight];
    l.textColor = color;
    l.textAlignment = NSTextAlignmentCenter;
    l.numberOfLines = 0;
    return l;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = [UIColor groupTableViewBackgroundColor];

    UILabel *title = MakeLabel(34, UIFontWeightBold, [UIColor blackColor]);
    title.text = @"充电限制";

    _statusLabel = MakeLabel(24, UIFontWeightMedium, [UIColor darkTextColor]);
    _detailLabel = MakeLabel(16, UIFontWeightRegular, [UIColor grayColor]);

    UILabel *hint = MakeLabel(15, UIFontWeightRegular, [UIColor grayColor]);
    hint.text = @"开关和电量上限在「设置 → 充电限制」里调整。\n限制由后台守护进程执行，关闭本 App 不影响。";

    UIButton *button = [UIButton buttonWithType:UIButtonTypeSystem];
    [button setTitle:@"打开设置页" forState:UIControlStateNormal];
    button.titleLabel.font = [UIFont systemFontOfSize:20 weight:UIFontWeightMedium];
    [button addTarget:self action:@selector(openSettings) forControlEvents:UIControlEventTouchUpInside];

    UIStackView *stack = [[UIStackView alloc] initWithArrangedSubviews:@[title, _statusLabel, _detailLabel, hint, button]];
    stack.axis = UILayoutConstraintAxisVertical;
    stack.alignment = UIStackViewAlignmentCenter;
    stack.spacing = 22;
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    [self.view addSubview:stack];
    [NSLayoutConstraint activateConstraints:@[
        [stack.centerXAnchor constraintEqualToAnchor:self.view.centerXAnchor],
        [stack.centerYAnchor constraintEqualToAnchor:self.view.centerYAnchor constant:-30],
        [stack.widthAnchor constraintLessThanOrEqualToAnchor:self.view.widthAnchor constant:-80],
        [stack.widthAnchor constraintLessThanOrEqualToConstant:520],
    ]];
    [self refresh];
}

- (void)viewWillAppear:(BOOL)animated {
    [super viewWillAppear:animated];
    [self refresh];
    _timer = [NSTimer scheduledTimerWithTimeInterval:3 target:self selector:@selector(refresh) userInfo:nil repeats:YES];
}

- (void)viewDidDisappear:(BOOL)animated {
    [super viewDidDisappear:animated];
    [_timer invalidate];
    _timer = nil;
}

- (void)refresh {
    NSUserDefaults *d = [NSUserDefaults standardUserDefaults];
    [d synchronize];   // 守护进程是另一个进程在写，先把最新内容读进来
    NSString *text = [d stringForKey:kStatusText];
    double stamp = [d doubleForKey:kStatusTime];
    NSTimeInterval age = stamp > 0 ? [[NSDate date] timeIntervalSince1970] - stamp : -1;

    if (!text.length || age < 0) {
        _statusLabel.text = @"还没有收到守护进程的状态";
        _detailLabel.text = @"守护进程未安装，或还没有启动。\n重启 iPad 后需要重新越狱，守护进程才会运行。";
    } else if (age > kStaleSeconds) {
        _statusLabel.text = @"守护进程可能没有运行";
        _detailLabel.text = [NSString stringWithFormat:@"%.0f 分钟前最后一次状态：%@", age / 60, text];
    } else {
        _statusLabel.text = text;
        _detailLabel.text = [NSString stringWithFormat:@"%.0f 秒前更新", age];
    }
}

- (void)openSettings {
    NSURL *url = [NSURL URLWithString:UIApplicationOpenSettingsURLString];   // 直接打开本 App 的设置页
    [[UIApplication sharedApplication] openURL:url options:@{} completionHandler:nil];
}

@end

@interface CLAppDelegate : UIResponder <UIApplicationDelegate>
@property (nonatomic, strong) UIWindow *window;
@end

@implementation CLAppDelegate

- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:(NSDictionary *)launchOptions {
    // 只是让本进程里读到的默认值与守护进程一致；registerDefaults 不会写盘
    [[NSUserDefaults standardUserDefaults] registerDefaults:@{
        @"enabled": @NO, @"upper": @50, @"hysteresis": @5, @"drain": @NO,
    }];
    // 记下最近一次打开时间。同时用来验证本 App 的偏好是否落在 /var/mobile/Library/Preferences（与守护进程共用）
    [[NSUserDefaults standardUserDefaults] setDouble:[[NSDate date] timeIntervalSince1970] forKey:@"appLastOpened"];
    [[NSUserDefaults standardUserDefaults] synchronize];

    self.window = [[UIWindow alloc] initWithFrame:[UIScreen mainScreen].bounds];
    self.window.rootViewController = [[CLViewController alloc] init];
    [self.window makeKeyAndVisible];
    return YES;
}

@end

int main(int argc, char *argv[]) {
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass([CLAppDelegate class]));
    }
}
