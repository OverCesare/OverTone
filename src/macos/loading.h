#import <AppKit/AppKit.h>

// The approved SVG's registered paths, rendered by the system animation server.
// No frame timer, raster sequence or independent loading/readiness state.
@interface OTLoadingView : NSView
@property(nonatomic) NSColor *backgroundColor;
@property CGFloat reservedHeight;
- (instancetype)initWithFrame:(NSRect)frame resource:(NSURL *)url;
// Arm feedback for a perceptible real wait; readiness always cancels immediately.
- (void)prepare;
- (void)play;
- (void)finishWithDuration:(NSTimeInterval)duration;
- (void)setProgress:(double)progress;
- (NSDictionary *)snapshot;
@end
