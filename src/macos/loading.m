#import "loading.h"
#import <QuartzCore/QuartzCore.h>
#include <ctype.h>
#include <math.h>
#include <xlocale.h>

// Deliberately the small absolute M/L/C/Z subset used by the bundled asset.
// Reject unrecognized geometry rather than silently changing the approved logo.
static CGPathRef logo_path(NSString *data) CF_RETURNS_RETAINED {
    const char *cursor=data.UTF8String;
    if (!cursor || data.length>65536) return NULL;
    locale_t locale=newlocale(LC_NUMERIC_MASK,"C",NULL);
    if (!locale) return NULL;
    CGMutablePathRef path=CGPathCreateMutable();
    BOOL valid=YES;unsigned segments=0;
    while (valid && *cursor) {
        while (isspace((unsigned char)*cursor) || *cursor==',') cursor++;
        if (!*cursor) break;
        char verb=*cursor++;
        unsigned count=verb=='C' ? 6 : verb=='M'||verb=='L' ? 2 : 0;
        if (++segments>512 || (!count && verb!='Z')) {valid=NO;break;}
        double n[6]={0};
        for (unsigned i=0;i<count;i++) {
            while (isspace((unsigned char)*cursor) || *cursor==',') cursor++;
            char *end=NULL;n[i]=strtod_l(cursor,&end,locale);
            if (end==cursor || !isfinite(n[i]) || fabs(n[i])>10000) {valid=NO;break;}
            cursor=end;
        }
        if (!valid) break;
        if (verb=='M') CGPathMoveToPoint(path,NULL,n[0],n[1]);
        else if (verb=='L') CGPathAddLineToPoint(path,NULL,n[0],n[1]);
        else if (verb=='C') CGPathAddCurveToPoint(path,NULL,n[0],n[1],n[2],n[3],n[4],n[5]);
        else CGPathCloseSubpath(path);
    }
    freelocale(locale);
    if (!valid || !segments) {CGPathRelease(path);return NULL;}
    return path;
}

@interface OTLoadingView () <NSXMLParserDelegate>
@property CALayer *drawing;
@property NSArray<CAShapeLayer *> *masks;
@property NSMutableArray<NSNumber *> *waves;
@property NSMutableArray<CAShapeLayer *> *blades;
@property CGAffineTransform groupTransform;
@property BOOL validAsset;
@property NSInteger currentBlade;
@property NSMutableArray<NSString *> *colours;
@property NSUInteger generation;
@property NSString *phase;
@property CFTimeInterval requestedAt, revealedAt, finishedAt;
@end

@implementation OTLoadingView
- (instancetype)initWithFrame:(NSRect)frame resource:(NSURL *)url {
    self=[super initWithFrame:frame];
    if (!self) return nil;
    NSData *data=[NSData dataWithContentsOfURL:url];
    if (!data || data.length>131072) return nil;
    self.wantsLayer=YES;
    self.drawing=[CALayer layer];self.drawing.anchorPoint=CGPointZero;
    self.drawing.bounds=CGRectMake(0,0,1024,1024);
    [self.layer addSublayer:self.drawing];
    NSMutableArray *masks=[NSMutableArray new];
    self.waves=[NSMutableArray new];self.blades=[NSMutableArray new];self.colours=[NSMutableArray new];
    for (unsigned i=0;i<6;i++) {
        [masks addObject:[CAShapeLayer layer]];
        [self.blades addObject:[CAShapeLayer layer]];
        [self.waves addObject:@0];[self.colours addObject:@""];
    }
    self.masks=masks;self.validAsset=YES;self.groupTransform=CGAffineTransformIdentity;
    NSURL *master=[[url URLByDeletingLastPathComponent] URLByAppendingPathComponent:@"OverTone.svg"];
    NSData *geometry=[NSData dataWithContentsOfURL:master];
    if (!geometry || geometry.length>131072) return nil;
    for (NSData *svg in @[data,geometry]) {
        self.currentBlade=-1;
        NSXMLParser *parser=[[NSXMLParser alloc] initWithData:svg];
        parser.shouldResolveExternalEntities=NO;parser.delegate=self;
        if (![parser parse] || !self.validAsset) return nil;
    }
    for (unsigned i=0;i<6;i++) {
        if (!self.masks[i].path || !self.blades[i].path || !self.blades[i].fillColor) return nil;
        self.blades[i].mask=self.masks[i];[self.drawing addSublayer:self.blades[i]];
    }
    self.hidden=YES;self.phase=@"idle";
    [NSWorkspace.sharedWorkspace.notificationCenter addObserver:self selector:@selector(motionChanged:)
        name:NSWorkspaceAccessibilityDisplayOptionsDidChangeNotification object:nil];
    return self;
}
- (void)dealloc {[NSWorkspace.sharedWorkspace.notificationCenter removeObserver:self];}
- (void)updateScale {
    CGFloat scale=self.window.backingScaleFactor;
    if (scale<=0) scale=NSScreen.mainScreen.backingScaleFactor;
    [CATransaction begin];[CATransaction setDisableActions:YES];
    self.drawing.contentsScale=scale;
    for (unsigned i=0;i<6;i++) {self.blades[i].contentsScale=scale;self.masks[i].contentsScale=scale;}
    [CATransaction commit];
}
- (void)viewDidMoveToWindow {[super viewDidMoveToWindow];[self updateScale];}
- (void)viewDidChangeBackingProperties {[super viewDidChangeBackingProperties];[self updateScale];}
- (BOOL)isFlipped {return YES;}
- (NSView *)hitTest:(NSPoint)point {(void)point;return nil;}
- (void)setBackgroundColor:(NSColor *)color {
    _backgroundColor=color;
    [CATransaction begin];[CATransaction setDisableActions:YES];
    self.layer.backgroundColor=color.CGColor;
    [CATransaction commit];
}
- (void)layout {
    [super layout];
    CGFloat side=MIN(MIN(320,MIN(self.bounds.size.width,self.bounds.size.height)*.6),MAX(0,self.bounds.size.height-self.reservedHeight)),scale=side/1024;
    [CATransaction begin];[CATransaction setDisableActions:YES];
    self.drawing.position=CGPointMake((self.bounds.size.width-side)/2,(self.bounds.size.height-side)/2);
    self.drawing.transform=CATransform3DMakeScale(scale,scale,1);
    [CATransaction commit];
}
- (BOOL)reducedMotion {return NSWorkspace.sharedWorkspace.accessibilityDisplayShouldReduceMotion;}
- (void)cancelMotion {
    self.generation++;
    [self.layer removeAllAnimations];
    [self.drawing removeAllAnimations];
    for (CAShapeLayer *mask in self.masks) [mask removeAllAnimations];
}
- (void)setProgress:(double)progress {
    [self cancelMotion];self.hidden=NO;
    [CATransaction begin];[CATransaction setDisableActions:YES];
    self.layer.opacity=1;self.drawing.opacity=1;self.drawing.hidden=NO;
    for (unsigned i=0;i<6;i++)
        self.masks[i].strokeEnd=(CGFloat)fmax(0,fmin(1,(progress*1.2-self.waves[i].doubleValue*.09)/1.02));
    [CATransaction commit];self.phase=progress==1 ? @"ready" : @"manual";
}
- (void)prepare {
    if (!self.hidden && ([@[@"waiting",@"playing",@"holding"] containsObject:self.phase])) return;
    [self cancelMotion];
    self.requestedAt=CACurrentMediaTime();self.revealedAt=self.finishedAt=0;
    self.phase=@"waiting";self.hidden=NO;
    [CATransaction begin];[CATransaction setDisableActions:YES];
    self.layer.opacity=1;self.drawing.hidden=YES;
    [CATransaction commit];
    NSUInteger generation=self.generation;
    __weak OTLoadingView *weak=self;
    // No minimum display time: this only defers feedback, never application readiness.
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW,NSEC_PER_SEC),dispatch_get_main_queue(),^{
        OTLoadingView *view=weak;
        if (view && view.generation==generation && [view.phase isEqual:@"waiting"]) [view play];
    });
}
- (void)play {
    if (!self.hidden && [self.phase isEqual:@"playing"]) return;
    if (![self.phase isEqual:@"waiting"]) {self.requestedAt=CACurrentMediaTime();self.finishedAt=0;}
    [self setProgress:0];self.revealedAt=CACurrentMediaTime();
    if (self.reducedMotion) {[self setProgress:1];self.phase=@"holding";return;}
    self.phase=@"playing";
    [CATransaction begin];[CATransaction setDisableActions:YES];
    CFTimeInterval now=[self.drawing convertTime:CACurrentMediaTime() fromLayer:nil];
    for (unsigned i=0;i<6;i++) {
        CAShapeLayer *mask=self.masks[i];mask.strokeEnd=1;
        CABasicAnimation *effect=[CABasicAnimation animationWithKeyPath:@"strokeEnd"];
        effect.fromValue=@0;effect.toValue=@1;effect.duration=1.02;
        effect.beginTime=self.waves[i].doubleValue*.09;
        effect.timingFunction=[CAMediaTimingFunction functionWithControlPoints:.25f :.7f :.3f :1];
        effect.fillMode=kCAFillModeBoth;
        // Reverse the entire staggered cycle together: no reset or partial master
        // at a loop boundary. Only real readiness cancels this server-owned motion.
        CAAnimationGroup *cycle=[CAAnimationGroup animation];
        cycle.animations=@[effect];cycle.duration=1.2;cycle.beginTime=now;
        cycle.autoreverses=YES;cycle.repeatCount=HUGE_VALF;
        [mask addAnimation:cycle forKey:@"reveal"];
    }
    [CATransaction commit];
}
- (void)finishWithDuration:(NSTimeInterval)duration {
    if ([self.phase isEqual:@"done"] && self.hidden) return;
    self.finishedAt=CACurrentMediaTime();
    BOOL revealed=!self.hidden && !self.drawing.hidden;
    if (!revealed || duration<=0 || self.reducedMotion) {
        [self cancelMotion];self.hidden=YES;self.phase=@"done";return;
    }
    [self setProgress:1];
    self.phase=@"finishing";NSUInteger generation=self.generation;
    __weak OTLoadingView *weak=self;
    [CATransaction begin];[CATransaction setDisableActions:YES];
    [CATransaction setCompletionBlock:^{
        OTLoadingView *view=weak;
        if (view && view.generation==generation) {view.hidden=YES;view.phase=@"done";}
    }];
    // Fade the mark on an opaque surface first. Only then reveal the workspace:
    // a completed loading animation must never run over an already visible app.
    self.layer.opacity=0;self.drawing.opacity=0;
    CABasicAnimation *mark=[CABasicAnimation animationWithKeyPath:@"opacity"];
    mark.fromValue=@1;mark.toValue=@0;mark.duration=duration*.5;
    mark.timingFunction=[CAMediaTimingFunction functionWithName:kCAMediaTimingFunctionEaseInEaseOut];
    [self.drawing addAnimation:mark forKey:@"exit"];
    CAKeyframeAnimation *cover=[CAKeyframeAnimation animationWithKeyPath:@"opacity"];
    cover.values=@[@1,@1,@0];cover.keyTimes=@[@0,@.5,@1];cover.duration=duration;
    cover.timingFunctions=@[[CAMediaTimingFunction functionWithName:kCAMediaTimingFunctionLinear],
                           [CAMediaTimingFunction functionWithName:kCAMediaTimingFunctionEaseInEaseOut]];
    [self.layer addAnimation:cover forKey:@"exit"];
    [CATransaction commit];
}
- (void)motionChanged:(NSNotification *)notification {
    (void)notification;
    if (!self.hidden && ![self.phase isEqual:@"waiting"] && self.reducedMotion) {
        if ([self.phase isEqual:@"finishing"]) [self finishWithDuration:0];
        else [self setProgress:1];
    }
}
- (NSDictionary *)snapshot {
    NSRect box=self.drawing.frame;
    NSMutableArray *progress=[NSMutableArray new];unsigned finite=(unsigned)(self.layer.animationKeys.count+self.drawing.animationKeys.count),animations=finite;
    for (CAShapeLayer *mask in self.masks) {
        CAShapeLayer *presentation=(CAShapeLayer *)mask.presentationLayer;
        [progress addObject:@(presentation ? presentation.strokeEnd : mask.strokeEnd)];
        animations+=(unsigned)mask.animationKeys.count;
    }
    return @{@"visible":@(!self.hidden),@"state":self.phase,@"animations":@(animations),@"finiteAnimations":@(finite),@"logoVisible":@(!self.hidden && !self.drawing.hidden),
        @"waitMs":@(self.requestedAt>0 ? ((self.finishedAt>0 ? self.finishedAt : CACurrentMediaTime())-self.requestedAt)*1000 : 0),
        @"revealed":@(self.revealedAt>0),@"progress":progress,@"colours":self.colours,
        @"x":@(box.origin.x),@"y":@(box.origin.y),@"width":@(box.size.width),@"height":@(box.size.height)};
}
- (void)parser:(NSXMLParser *)parser didStartElement:(NSString *)element namespaceURI:(NSString *)uri
        qualifiedName:(NSString *)qualified attributes:(NSDictionary<NSString *,NSString *> *)attributes {
    (void)uri;(void)qualified;
    if ([element isEqual:@"svg"]) {
        if (![attributes[@"viewBox"] isEqual:@"0 0 1024 1024"]) self.validAsset=NO;
    } else if ([element isEqual:@"g"]) {
        NSString *identity=attributes[@"id"];
        self.currentBlade=[identity hasPrefix:@"blade-0"] && identity.length==8 && [identity characterAtIndex:7]>='1' &&
            [identity characterAtIndex:7]<='6' ? [identity characterAtIndex:7]-'1' : -1;
        NSString *transform=attributes[@"transform"];
        NSScanner *scanner=[NSScanner scannerWithString:transform ?: @""];
        scanner.locale=[NSLocale localeWithLocaleIdentifier:@"en_US_POSIX"];
        double x=0,y=0,scale=1;
        if (transform && (![scanner scanString:@"translate(" intoString:NULL] || ![scanner scanDouble:&x] ||
            ![scanner scanDouble:&y] || ![scanner scanString:@")" intoString:NULL] ||
            ![scanner scanString:@"scale(" intoString:NULL] || ![scanner scanDouble:&scale] ||
            ![scanner scanString:@")" intoString:NULL] || !scanner.isAtEnd || !isfinite(x) || !isfinite(y) ||
            !isfinite(scale) || scale<=0 || scale>4 || fabs(x)>10000 || fabs(y)>10000)) self.validAsset=NO;
        self.groupTransform=CGAffineTransformMake(scale,0,0,scale,x,y);
    } else if ([element isEqual:@"path"]) {
        NSString *index=attributes[@"data-cl-trace"];
        BOOL trace=index!=nil;
        if (trace && (index.length!=1 || [index characterAtIndex:0]<'0' || [index characterAtIndex:0]>'5')) {self.validAsset=NO;return;}
        NSInteger ordinal=trace ? [index characterAtIndex:0]-'0' : self.currentBlade;
        if (ordinal<0 || ordinal>5) {self.validAsset=NO;return;}
        NSUInteger i=(NSUInteger)ordinal;
        CAShapeLayer *layer=trace ? self.masks[i] : self.blades[i];
        CGPathRef raw=logo_path(attributes[@"d"]);
        if (!raw || layer.path) {if (raw) CGPathRelease(raw);self.validAsset=NO;return;}
        if (trace) {
            layer.path=raw;layer.fillColor=NULL;layer.strokeColor=NSColor.whiteColor.CGColor;
            double width=attributes[@"stroke-width"].doubleValue,wave=attributes[@"data-cl-wave"].doubleValue;
            if (!isfinite(width) || width<=0 || width>512 || !isfinite(wave) || wave<0 || wave>2 || floor(wave)!=wave) self.validAsset=NO;
            layer.lineWidth=(CGFloat)width;layer.lineCap=kCALineCapButt;layer.lineJoin=kCALineJoinRound;
            self.waves[i]=@(wave);
        } else {
            CGAffineTransform t=self.groupTransform;
            CGPathRef path=CGPathCreateCopyByTransformingPath(raw,&t);layer.path=path;CGPathRelease(path);
            if (!self.masks[i].path) self.validAsset=NO;
            else {
                path=CGPathCreateCopyByTransformingPath(self.masks[i].path,&t);self.masks[i].path=path;CGPathRelease(path);
                self.masks[i].lineWidth*=t.a;
            }
            NSString *fill=attributes[@"fill"];unsigned rgb=0;
            NSScanner *scanner=[NSScanner scannerWithString:fill.length==7 ? [fill substringFromIndex:1] : @""];
            if (![fill hasPrefix:@"#"] || ![scanner scanHexInt:&rgb] || !scanner.isAtEnd) self.validAsset=NO;
            else {self.colours[i]=fill;layer.fillColor=[NSColor colorWithSRGBRed:((rgb>>16)&255)/255.0 green:((rgb>>8)&255)/255.0 blue:(rgb&255)/255.0 alpha:1].CGColor;}
        }
        CGPathRelease(raw);
    }
    if (!self.validAsset) [parser abortParsing];
}
@end
