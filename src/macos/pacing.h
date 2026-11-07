#import <AppKit/AppKit.h>

// Bounded, opt-in measurement of this process's own window. No screen permission,
// audio or other apps. Video is saved locally only by an explicit record command.
@interface OTMotionTrace : NSObject
@property(readonly) NSDictionary *state;
- (void)startForWindow:(NSWindow *)window seconds:(unsigned)seconds
             vertical:(BOOL)vertical position:(NSInteger)position range:(NSRange)range
            recording:(BOOL)recording path:(NSString *)path;
+ (void)captureWindow:(NSWindow *)window completion:(void (^)(CGImageRef, NSError *))completion;
- (void)cancel;
@end
