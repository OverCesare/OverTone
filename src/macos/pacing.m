#import "pacing.h"
#import <ScreenCaptureKit/ScreenCaptureKit.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#include <mach/mach_time.h>
#include <unistd.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <errno.h>

enum { trace_capacity = 2560, trace_width = 4096, trace_height = 8 };
typedef struct {
    uint64_t display;
    uint64_t arrival;
    uint64_t hash;
    uint32_t changed;
    uint32_t edge;
    uint32_t texture;
    NSInteger status;
} TraceFrame;

@interface OTMotionTrace () <SCStreamOutput, SCStreamDelegate> {
    TraceFrame frames[trace_capacity];
    uint32_t previous[trace_width * trace_height];
    size_t count, previousWidth;
    size_t capturedWidth, capturedHeight;
    mach_timebase_info_data_t timebase;
    atomic_bool accepting;
}
@property SCStream *stream;
@property dispatch_queue_t samples;
@property dispatch_source_t deadline;
@property NSString *path;
@property NSString *phase;
@property NSString *failure;
@property NSNumber *started;
@property BOOL vertical;
@property NSUInteger position;
@property NSRange edgeRange;
@property NSDictionary *probe;
@property BOOL outputAttached;
@property id recordingOutput;
@property NSString *videoPath;
@property BOOL recordingPending;
@property BOOL videoStarted;
@property BOOL streamStopped;
@end

API_AVAILABLE(macos(15.0))
@interface OTMotionTrace (Recording) <SCRecordingOutputDelegate>
@end

@implementation OTMotionTrace
+ (void)captureWindow:(NSWindow *)window completion:(void (^)(CGImageRef, NSError *))completion {
    if (@available(macOS 14.4, *)) {
        CGWindowID identifier=(CGWindowID)window.windowNumber;
        [SCShareableContent getCurrentProcessShareableContentWithCompletionHandler:^(SCShareableContent *content,NSError *error) {
            SCWindow *own=nil;
            for (SCWindow *candidate in content.windows)
                if (candidate.windowID==identifier && candidate.owningApplication.processID==getpid()) {own=candidate;break;}
            if (error || !own) {completion(NULL,error);return;}
            SCStreamConfiguration *configuration=[SCStreamConfiguration new];
            configuration.width=(size_t)own.frame.size.width;configuration.height=(size_t)own.frame.size.height;
            configuration.showsCursor=NO;configuration.ignoreShadowsSingleWindow=YES;
            [SCScreenshotManager captureImageWithFilter:[[SCContentFilter alloc] initWithDesktopIndependentWindow:own]
                configuration:configuration completionHandler:completion];
        }];
    } else completion(NULL,nil);
}

- (instancetype)init {
    if ((self = [super init])) {
        _samples = dispatch_queue_create("com.overtone.motion-trace", DISPATCH_QUEUE_SERIAL);
        _phase = @"idle";
        mach_timebase_info(&timebase);
        atomic_init(&accepting,false);
    }
    return self;
}
- (NSDictionary *)state {
    NSAssert(NSThread.isMainThread, @"Trace lifetime belongs to the main queue");
    return @{@"phase":self.phase, @"path":self.path ?: @"", @"started":self.started ?: @0,
             @"videoPath":self.videoPath ?: @"", @"videoStarted":@(self.videoStarted),
             @"error":self.failure ?: @""};
}
- (void)clearDeadline {
    if (self.deadline) dispatch_source_cancel(self.deadline);
    self.deadline = nil;
}
- (void)armDeadline:(unsigned)seconds {
    [self clearDeadline];
    self.deadline = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER,0,0,dispatch_get_main_queue());
    dispatch_source_set_timer(self.deadline,
        dispatch_time(DISPATCH_TIME_NOW,(int64_t)seconds * (int64_t)NSEC_PER_SEC),
        DISPATCH_TIME_FOREVER, NSEC_PER_MSEC);
    __weak OTMotionTrace *weak = self;
    dispatch_source_set_event_handler(self.deadline, ^{
      OTMotionTrace *trace = weak;
      if ([trace.phase isEqual:@"stopping"]) {
          trace.failure = @"trace_stop_timeout";
          [trace collect];
      } else {
          if ([trace.phase isEqual:@"starting"]) trace.failure = @"trace_start_timeout";
          [trace finish];
      }
    });
    dispatch_resume(self.deadline);
}
- (void)startForWindow:(NSWindow *)window seconds:(unsigned)seconds
             vertical:(BOOL)vertical position:(NSInteger)position range:(NSRange)range
            recording:(BOOL)recording path:(NSString *)path {
    NSAssert(NSThread.isMainThread, @"Trace lifetime belongs to the main queue");
    if (![self.phase isEqual:@"idle"] || !seconds || seconds > 10) return;
    self.path = path;
    self.phase = @"starting";
    self.vertical = vertical;
    self.started = @([NSDate.date timeIntervalSince1970]);
    if (recording) {
        if (@available(macOS 15.0, *)) {}
        else { self.failure = @"requires_macos_15"; [self finish]; return; }
    }
    if (@available(macOS 14.4, *)) {
        CGWindowID identifier = (CGWindowID)window.windowNumber;
        [self armDeadline:3];
        __weak OTMotionTrace *weak = self;
        // Never replace this call with the permissioned, system-wide enumeration.
        [SCShareableContent getCurrentProcessShareableContentWithCompletionHandler:
         ^(SCShareableContent *content, NSError *error) {
          dispatch_async(dispatch_get_main_queue(), ^{
            OTMotionTrace *trace = weak;
            if (!trace || ![trace.phase isEqual:@"starting"]) return;
            SCWindow *own = nil;
            for (SCWindow *candidate in content.windows)
                if (candidate.windowID == identifier &&
                    candidate.owningApplication.processID == getpid()) { own = candidate; break; }
            if (error || !own) {
                trace.failure = @"own_window_unavailable";
                [trace finish];
                return;
            }
            SCContentFilter *filter = [[SCContentFilter alloc] initWithDesktopIndependentWindow:own];
            SCStreamConfiguration *config = [SCStreamConfiguration new];
            double width = own.frame.size.width, height = own.frame.size.height,
                   scale = fmin(1,(double)trace_width/fmax(width,height)),
                   cross = vertical ? width : height,
                   at = position < 0 ? floor(cross/2) : (double)position;
            at = fmax(0,fmin(at,cross-(double)trace_height));
            // sourceRect is ignored by desktop-independent window capture.
            // Capture nominal window pixels, then inspect only the requested
            // narrow strip. Scaling a whole window into 8px aliases the motion.
            config.width = (size_t)fmax(1,floor(width*scale));
            config.height = (size_t)fmax(1,floor(height*scale));
            trace.position = (NSUInteger)floor(at*scale);
            NSUInteger length = vertical ? config.height : config.width;
            NSUInteger start = range.location == NSNotFound ? (vertical ? length*2/3 : length > 400 ? length-400 : 0)
                                    : MIN(length,(NSUInteger)floor((double)range.location*scale)),
                       end = range.location == NSNotFound ? length
                                    : MIN(length,(NSUInteger)ceil((double)NSMaxRange(range)*scale));
            trace.edgeRange = NSMakeRange(start,end-start);
            trace.probe = @{@"axis":vertical ? @"vertical" : @"horizontal",@"position":@(at),
                            @"windowWidth":@(width),@"windowHeight":@(height),
                            @"captureWidth":@(config.width),@"captureHeight":@(config.height),
                            @"edgeStart":@(start),@"edgeEnd":@(end),
                            @"scale":@(scale)};
            config.minimumFrameInterval = kCMTimeZero; // Display's rate, not a timer or FPS claim.
            config.queueDepth = 3;
            config.pixelFormat = kCVPixelFormatType_32BGRA;
            config.showsCursor = NO;
            config.capturesAudio = NO;
            config.ignoreShadowsSingleWindow = YES;
            config.captureResolution = SCCaptureResolutionNominal;
            trace.stream = [[SCStream alloc] initWithFilter:filter configuration:config delegate:trace];
            NSError *outputError = nil;
            if (![trace.stream addStreamOutput:trace type:SCStreamOutputTypeScreen
                             sampleHandlerQueue:trace.samples error:&outputError]) {
                trace.failure = @"trace_output_unavailable";
                [trace finish];
                return;
            }
            trace.outputAttached = YES;
            if (recording) {
                if (@available(macOS 15.0, *)) {
                    trace.videoPath = [[path stringByDeletingPathExtension] stringByAppendingPathExtension:@"mp4"];
                    SCRecordingOutputConfiguration *record = [SCRecordingOutputConfiguration new];
                    record.outputURL = [NSURL fileURLWithPath:trace.videoPath];
                    // The fixed diagnostic file is replaced only by an explicit recording.
                    // Never recursively remove a directory or follow a symbolic link here.
                    struct stat destination;
                    const char *file = trace.videoPath.fileSystemRepresentation;
                    BOOL safe = lstat(file,&destination) == 0
                        ? S_ISREG(destination.st_mode) && destination.st_uid == getuid() && unlink(file) == 0
                        : errno == ENOENT;
                    if (!safe) {
                        trace.failure = @"trace_recording_path_unavailable";
                        [trace finish];
                        return;
                    }
                    trace.recordingOutput = [[SCRecordingOutput alloc] initWithConfiguration:record delegate:trace];
                    if (![trace.stream addRecordingOutput:trace.recordingOutput error:NULL]) {
                        trace.failure = @"trace_recording_unavailable";
                        [trace finish];
                        return;
                    }
                    trace.recordingPending = YES;
                }
            }
            atomic_store_explicit(&trace->accepting,true,memory_order_relaxed);
            [trace.stream startCaptureWithCompletionHandler:^(NSError *startError) {
              dispatch_async(dispatch_get_main_queue(), ^{
                OTMotionTrace *active = weak;
                if (!active || ![active.phase isEqual:@"starting"]) return;
                if (startError) { active.failure = @"trace_start_failed"; [active finish]; }
                else {
                    active.phase = @"recording";
                    [active armDeadline:seconds];
                }
              });
            }];
          });
        }];
    } else {
        self.failure = @"requires_macos_14_4";
        [self finish];
    }
}
- (void)stream:(SCStream *)stream didOutputSampleBuffer:(CMSampleBufferRef)buffer
        ofType:(SCStreamOutputType)type {
    (void)stream;
    if (type != SCStreamOutputTypeScreen || !atomic_load_explicit(&accepting,memory_order_relaxed) ||
        count == trace_capacity) return;
    CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(buffer,false);
    if (!attachments || !CFArrayGetCount(attachments)) return;
    NSDictionary *info = (__bridge NSDictionary *)CFArrayGetValueAtIndex(attachments,0);
    TraceFrame frame = {.display=[info[SCStreamFrameInfoDisplayTime] unsignedLongLongValue],
                        .arrival=mach_absolute_time(), .status=[info[SCStreamFrameInfoStatus] integerValue]};
    CVPixelBufferRef image = CMSampleBufferGetImageBuffer(buffer);
    if (image && frame.status == SCFrameStatusComplete &&
        CVPixelBufferGetPixelFormatType(image) == kCVPixelFormatType_32BGRA &&
        CVPixelBufferLockBaseAddress(image,kCVPixelBufferLock_ReadOnly) == kCVReturnSuccess) {
        BOOL vertical = self.vertical;
        size_t width = CVPixelBufferGetWidth(image), height = CVPixelBufferGetHeight(image),
               stride = CVPixelBufferGetBytesPerRow(image),
               length = MIN(trace_width,vertical ? height : width),
               step = vertical ? stride : sizeof(uint32_t);
        size_t across = MIN(trace_height,vertical ? width : height);
        size_t at = MIN(self.position,(vertical ? width : height)-across);
        const uint8_t *raw = CVPixelBufferGetBaseAddress(image),
                      *base = raw ? raw + (vertical ? at*sizeof(uint32_t) : at*stride) : NULL,
                      *scan = base && width && height ? base +
                          (vertical ? (across/2)*sizeof(uint32_t) : (across/2)*stride) : NULL;
        capturedWidth = width;
        capturedHeight = height;
        uint32_t largest = 0, lastPixel = 0;
        frame.hash = scan ? UINT64_C(14695981039346656037) : 0;
        for (size_t x = 0; scan && x < length; x++) {
            // Hash the whole narrow strip: one scanline can remain unchanged
            // between glyph rows even while the text scrolls. Padding is excluded.
            for (size_t cross = 0; cross < across; cross++) {
                const uint8_t *p = base + (vertical ? x*stride + cross*sizeof(uint32_t)
                                                                   : cross*stride + x*sizeof(uint32_t));
                uint32_t pixel = *(const uint32_t *)p;
                if (!cross) {
                    frame.texture += (uint32_t)(x && pixel != lastPixel);
                    lastPixel = pixel;
                }
                size_t index = x*trace_height+cross;
                frame.hash = (frame.hash ^ pixel) * UINT64_C(1099511628211);
                frame.changed += (uint32_t)(previousWidth != length || previous[index] != pixel);
                previous[index] = pixel;
            }
            // Only hashes and geometry leave this callback. The previous small
            // strip is overwritten in memory and cleared when the trace stops.
            if (x && NSLocationInRange(x,self.edgeRange)) {
                uint32_t pixel = *(const uint32_t *)(scan+x*step);
                uint32_t before = *(const uint32_t *)(scan+(x-1)*step);
                uint32_t delta = (uint32_t)(abs((int)(pixel&255)-(int)(before&255)) +
                                  abs((int)((pixel>>8)&255)-(int)((before>>8)&255)) +
                                  abs((int)((pixel>>16)&255)-(int)((before>>16)&255)));
                if (delta > largest) { largest = delta; frame.edge = (uint32_t)x; }
            }
        }
        previousWidth = scan ? length : 0;
        CVPixelBufferUnlockBaseAddress(image,kCVPixelBufferLock_ReadOnly);
    }
    frames[count++] = frame;
}
- (void)stream:(SCStream *)stream didStopWithError:(NSError *)error {
    (void)stream; (void)error;
    dispatch_async(dispatch_get_main_queue(), ^{
      if (![self.phase isEqual:@"starting"] && ![self.phase isEqual:@"recording"]) return;
      self.failure = @"trace_stream_stopped";
      [self finish];
    });
}
- (void)finish {
    if (![self.phase isEqual:@"starting"] && ![self.phase isEqual:@"recording"]) return;
    atomic_store_explicit(&accepting,false,memory_order_relaxed);
    self.phase = @"stopping";
    [self armDeadline:2];
    SCStream *stream = self.stream;
    __weak OTMotionTrace *weak = self;
    void (^complete)(NSError *) = ^(NSError *error) {
      dispatch_async(dispatch_get_main_queue(), ^{
        OTMotionTrace *trace = weak;
        if (error && !trace.failure) trace.failure = @"trace_stop_failed";
        trace.streamStopped = YES;
        if (!trace.recordingPending) [trace collect];
      });
    };
    if (stream) [stream stopCaptureWithCompletionHandler:complete];
    else complete(nil);
}
- (void)collect {
    if (![self.phase isEqual:@"stopping"]) return;
    self.phase = @"writing";
    [self clearDeadline];
    if (self.outputAttached)
        [self.stream removeStreamOutput:self type:SCStreamOutputTypeScreen error:NULL];
    self.outputAttached = NO;
    if (self.recordingPending && self.recordingOutput) {
        if (@available(macOS 15.0, *))
            [self.stream removeRecordingOutput:self.recordingOutput error:NULL];
    }
    self.recordingOutput = nil;
    self.stream = nil;
    // This barrier drains samples before reading their storage. JSON encoding
    // and atomic I/O stay off the UI thread, outside the measured movements.
      dispatch_async(self.samples, ^{ @autoreleasepool {
        NSMutableArray *values = [NSMutableArray arrayWithCapacity:self->count];
        uint64_t origin = self->count ? self->frames[0].arrival : 0;
        double ms = (double)self->timebase.numer / self->timebase.denom / 1e6;
        for (size_t i=0; i<self->count; i++) {
            TraceFrame f = self->frames[i];
            [values addObject:@{@"displayMs":@((double)f.display*ms),
                                @"arrivalMs":@((double)(f.arrival-origin)*ms),
                                @"status":@(f.status),@"hash":[NSString stringWithFormat:@"%llx",f.hash],
                                @"changed":@(f.changed),@"edge":@(f.edge),@"texture":@(f.texture)}];
        }
        NSDictionary *result = @{@"started":self.started,@"error":self.failure ?: @"",
                                  @"videoPath":self.videoPath ?: @"",
                                  @"sampleLimit":@(trace_capacity),@"probe":self.probe ?: @{},
                                  @"capturedWidth":@(self->capturedWidth),
                                  @"capturedHeight":@(self->capturedHeight),
                                  @"truncated":@(self->count == trace_capacity),@"frames":values};
        NSData *data = [NSJSONSerialization dataWithJSONObject:result options:0 error:NULL];
        BOOL saved = data && [data writeToFile:self.path options:NSDataWritingAtomic error:NULL];
        memset(self->previous,0,sizeof self->previous);
        dispatch_async(dispatch_get_main_queue(), ^{
          self.samples = nil;
          if (!saved && !self.failure) self.failure = @"trace_write_failed";
          self.phase = self.failure ? @"failed" : @"done";
        });
      }});
}
- (void)cancel {
    if ([self.phase isEqual:@"starting"] || [self.phase isEqual:@"recording"]) {
        self.failure = @"trace_cancelled";
        [self finish];
    }
}
@end

@implementation OTMotionTrace (Recording)
- (void)recordingOutputDidStartRecording:(SCRecordingOutput *)output API_AVAILABLE(macos(15.0)) {
    dispatch_async(dispatch_get_main_queue(), ^{
      if (output == self.recordingOutput &&
          ([self.phase isEqual:@"starting"] || [self.phase isEqual:@"recording"]))
          self.videoStarted = YES;
    });
}
- (void)recordingOutputDidFinishRecording:(SCRecordingOutput *)output API_AVAILABLE(macos(15.0)) {
    dispatch_async(dispatch_get_main_queue(), ^{
      if (output != self.recordingOutput) return;
      self.recordingPending = NO;
      if (self.streamStopped) [self collect];
    });
}
- (void)recordingOutput:(SCRecordingOutput *)output didFailWithError:(NSError *)error API_AVAILABLE(macos(15.0)) {
    (void)error;
    dispatch_async(dispatch_get_main_queue(), ^{
      if (output != self.recordingOutput) return;
      self.recordingPending = NO;
      if (!self.failure) self.failure = @"trace_recording_failed";
      if (self.streamStopped) [self collect];
      else [self finish];
    });
}
@end
