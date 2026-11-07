#import <AppKit/AppKit.h>
#import <WebKit/WebKit.h>
#import <NaturalLanguage/NaturalLanguage.h>
#include "core.h"
#include "store.h"
#include "engine.h"
#include "library.h"
#include "diagnostics.h"
#include "location.h"
#include "package.h"
#import "pacing.h"
#import "loading.h"
#import "connection.h"
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <math.h>

static WKWebViewConfiguration *private_renderer(void) {
    WKWebViewConfiguration *configuration = [WKWebViewConfiguration new];
    configuration.websiteDataStore = WKWebsiteDataStore.nonPersistentDataStore;
    return configuration;
}

static char appearance_context;
static NSString *system_theme(void) {
    return [[NSApp.effectiveAppearance bestMatchFromAppearancesWithNames:
                     @[NSAppearanceNameAqua, NSAppearanceNameDarkAqua]] isEqual:NSAppearanceNameDarkAqua]
               ? @"dark" : @"light";
}

/* Only empty header space is a window background. Controls keep their WebKit hit targets. */
@interface ChromeBackground : NSView
@property NSArray<NSValue *> *controls;
@property CGFloat leading;
@property CGFloat trailing;
@property unsigned dragEvents;
@property BOOL draggingAllowed;
@end
@implementation ChromeBackground
- (BOOL)isFlipped {
    return YES;
}
- (BOOL)mouseDownCanMoveWindow {
    return self.draggingAllowed;
}
- (void)mouseDown:(NSEvent *)event {
    if (!self.draggingAllowed)
        return;
    self.dragEvents++;
    if (event.clickCount == 2)
        [self.window performZoom:self];
    else
        [self.window performWindowDragWithEvent:event];
}
- (void)resetCursorRects {
    [super resetCursorRects];
    if (!self.draggingAllowed)
        return;
    NSRect area = NSMakeRect(self.leading, 0, MAX(0, self.bounds.size.width - self.leading - self.trailing),
                             self.bounds.size.height);
    // Only uncovered rectangles receive a hand cursor; buttons keep their own cursor.
    NSMutableArray<NSValue *> *spaces = [NSMutableArray arrayWithObject:[NSValue valueWithRect:area]];
    for (NSValue *value in self.controls) {
        NSMutableArray<NSValue *> *next = [NSMutableArray new];
        for (NSValue *space in spaces) {
            NSRect r = space.rectValue, cut = NSIntersectionRect(r, value.rectValue);
            if (NSIsEmptyRect(cut)) {
                [next addObject:space];
                continue;
            }
            NSRect pieces[] = {
                NSMakeRect(r.origin.x, r.origin.y, cut.origin.x - r.origin.x, r.size.height),
                NSMakeRect(NSMaxX(cut), r.origin.y, NSMaxX(r) - NSMaxX(cut), r.size.height),
                NSMakeRect(cut.origin.x, r.origin.y, cut.size.width, cut.origin.y - r.origin.y),
                NSMakeRect(cut.origin.x, NSMaxY(cut), cut.size.width, NSMaxY(r) - NSMaxY(cut))};
            for (unsigned i = 0; i < 4; i++)
                if (!NSIsEmptyRect(pieces[i]))
                    [next addObject:[NSValue valueWithRect:pieces[i]]];
        }
        spaces = next;
    }
    for (NSValue *space in spaces)
        [self addCursorRect:space.rectValue cursor:NSCursor.openHandCursor];
}
- (NSView *)hitTest:(NSPoint)point {
    NSPoint p = [self convertPoint:point fromView:self.superview];
    if (!self.draggingAllowed || self.hidden || !NSPointInRect(p, self.bounds) || p.x < self.leading ||
        p.x > self.bounds.size.width - self.trailing)
        return nil;
    for (NSValue *control in self.controls)
        if (NSPointInRect(p, control.rectValue))
            return nil;
    return self;
}
@end

/* Finish the small chrome movement before AppKit snapshots/resizes the full-screen window. */
@interface WorkspaceWindow : NSWindow
@property(copy) void (^prepareFullScreen)(BOOL, dispatch_block_t);
@property BOOL preparingFullScreen;
@property NSUInteger fullScreenPreparation;
@end
@implementation WorkspaceWindow
- (void)beginSystemFullScreen:(id)sender {
    [super toggleFullScreen:sender];
}
- (void)toggleFullScreen:(id)sender {
    if (self.preparingFullScreen)
        return;
    if (!self.prepareFullScreen) {
        [super toggleFullScreen:sender];
        return;
    }
    self.preparingFullScreen = YES;
    NSUInteger preparation = ++self.fullScreenPreparation;
    __weak WorkspaceWindow *weak = self;
    dispatch_block_t finish = ^{
      WorkspaceWindow *window = weak;
      if (!window || !window.preparingFullScreen || window.fullScreenPreparation != preparation)
          return;
      window.preparingFullScreen = NO;
      if (window.visible)
          [window beginSystemFullScreen:sender];
    };
    self.prepareFullScreen(!(self.styleMask & NSWindowStyleMaskFullScreen), finish);
    /* A renderer that is busy or unavailable must never trap the native command. */
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 450 * NSEC_PER_MSEC), dispatch_get_main_queue(), finish);
}
@end

/* Quit is a native operation even when WebKit cannot process keyboard events. */
@interface WorkspaceView : WKWebView
@end
@implementation WorkspaceView
- (BOOL)performKeyEquivalent:(NSEvent *)event {
    NSEventModifierFlags modifiers =
        event.modifierFlags & (NSEventModifierFlagCommand | NSEventModifierFlagControl |
                               NSEventModifierFlagOption | NSEventModifierFlagShift);
    if (modifiers == NSEventModifierFlagCommand &&
        [event.charactersIgnoringModifiers.lowercaseString isEqual:@"q"]) {
        [NSApp terminate:self];
        return YES;
    }
    return [super performKeyEquivalent:event];
}
@end

@interface Desktop : NSObject <NSApplicationDelegate, WKScriptMessageHandler, WKNavigationDelegate,
                               WKUIDelegate, NSURLSessionDataDelegate, NSWindowDelegate, NSMenuItemValidation> {
  @public
    ot_app *core;
    ot_library *library;
    dispatch_queue_t owner;
    ot_location location;
    atomic_bool locationCancel;
    uint64_t networkAccepted[5], networkBlocked[5];
}
@property NSWindow *window;
@property WKWebView *web;
@property ChromeBackground *chrome;
@property OTLoadingView *loading;
@property NSURLSession *session;
@property NSURLSession *localSession;
@property NSOperationQueue *networkCallbacks;
@property NSURLSessionDataTask *task;
@property NSString *profile;
@property BOOL showSetup;
@property NSMutableDictionary<NSNumber *, NSURLSessionDataTask *> *libraryTasks;
@property uint64_t shownLibraryRevision;
@property uint64_t shownMemoryRevision;
@property BOOL memoryRendering;
@property BOOL terminating;
@property BOOL opening;
@property dispatch_source_t observationTimer;
@property id<NSObject> observationActivity;
@property OTMotionTrace *motionTrace;
@property BOOL fileBusy;
@property NSDictionary<NSString *, NSString *> *sessionCredential;
@property OTConnection *connectionManager;
@property BOOL connectionRestoring;
@property BOOL connectionEditing;
@property NSString *transportError;
@property uint64_t operation;
@property uint64_t shownRevision;
@property uint64_t shownStreamRevision;
@property BOOL rendering;
@property BOOL renderRequested;
@property uint64_t renderInterval;
@property uint64_t rendererEpoch;
@property NSTimeInterval rendererRecoveryTime;
@property dispatch_source_t timer;
@property int lock;
@property int locationLock;
@property BOOL locationBusy;
@property BOOL restarting;
@property BOOL servicesStopped;
@property ot_diagnostics *diagnostics;
@property NSString *uiStatus;
@property NSDictionary *rendererFault;
@property NSDictionary *windowLayout;
@property NSDictionary *readingMetrics;
@property NSDictionary *renderMetrics;
@property NSDictionary *motionMetrics;
@property NSMutableArray<NSDictionary *> *networkMetrics;
@property NSInteger chromePublishedTransition;
@property NSTimeInterval resourceSampleTime;
@property BOOL streamRenderScheduled;
@property dispatch_group_t fileWork;
@property NSString *language;
@property NSDictionary<NSString *, NSString *> *translations;
@property BOOL chromeFullScreen;
@property BOOL chromePublished;
@property BOOL observingAppearance;
@property NSMutableDictionary<NSString *, NSMenuItem *> *workspaceMenuItems;
@property NSSet<NSString *> *enabledWorkspaceActions;
@property NSSet<NSString *> *checkedWorkspaceActions;
@property NSInteger chromeTransition;
- (void)applyLanguage:(NSString *)language;
- (BOOL)validateMenuItem:(NSMenuItem *)item;
- (void)performWorkspaceAction:(NSMenuItem *)item;
- (NSString *)text:(NSString *)italian english:(NSString *)english;
- (void)layoutChrome;
- (void)setTheme:(NSString *)theme;
- (void)createWorkspaceWithTheme:(NSString *)theme accent:(NSString *)accent;
- (void)scheduleStreamRender;
- (void)requestRender;
- (void)finishRender;
- (void)openWorkspace;
- (void)sendLocation;
- (void)locationAction:(NSString *)action;
- (void)stopServices;
- (void)stopObservation;
- (void)recoverWindow:(void (^)(BOOL))completion;
- (void)checkWindowVisibilityUntil:(NSTimeInterval)deadline completion:(void (^)(BOOL))completion;
@end

static NSString *session_key(Desktop *desktop, NSString *server) {
    NSDictionary *credential = desktop.sessionCredential;
    return [credential[@"server"] isEqual:server] ? credential[@"key"] : @"";
}

typedef struct {
    __unsafe_unretained NSTextField *label;
    __unsafe_unretained NSProgressIndicator *bar;
    __unsafe_unretained NSButton *cancel;
    __unsafe_unretained NSString *finishing, *verifying, *copying;
    NSTimeInterval emitted;
} TransferPresentation;
static void transferProgress(void *context, const char *stage, uint64_t bytes, uint64_t total) {
    TransferPresentation *p = context;
    NSTimeInterval now = NSProcessInfo.processInfo.systemUptime;
    BOOL committing = !strcmp(stage, "registering");
    if (!committing && now - p->emitted < .1)
        return;
    p->emitted = now;
    NSString *label = committing ? p->finishing
                      : !strcmp(stage, "verifying") ? p->verifying : p->copying;
    NSTextField *field = p->label;
    NSProgressIndicator *bar = p->bar;
    NSButton *cancelButton = p->cancel;
    dispatch_async(dispatch_get_main_queue(), ^{
      field.stringValue = label;
      bar.indeterminate = total == 0;
      bar.maxValue = (double)(total ? total : 1);
      bar.doubleValue = (double)bytes;
      cancelButton.enabled = !committing;
    });
}
static void request(void *context, uint64_t operation, const char *body);
static NSDictionary *regional_environment(void) {
    NSLocale *locale = NSLocale.currentLocale;
    NSString *pattern = [NSDateFormatter dateFormatFromTemplate:@"j" options:0 locale:locale];
    BOOL twelve = [pattern containsString:@"h"] || [pattern containsString:@"K"];
    NSMutableArray<NSString *> *parts = [NSMutableArray arrayWithObject:locale.languageCode ?: @"en"];
    if (locale.scriptCode.length)
        [parts addObject:locale.scriptCode];
    if (locale.countryCode.length)
        [parts addObject:locale.countryCode];
    return @{@"locale" : [parts componentsJoinedByString:@"-"],
             @"hourCycle" : twelve ? @"h12" : @"h23"};
}
static void prompt_language(void *context, const char *prompt, char tag[16]) {
    (void)context;
    NLLanguageRecognizer *recognizer = [NLLanguageRecognizer new];
    [recognizer processString:@(prompt)];
    NSDictionary<NLLanguage, NSNumber *> *hypotheses = [recognizer languageHypothesesWithMaximum:1];
    NLLanguage language = recognizer.dominantLanguage;
    if (language && hypotheses[language].doubleValue >= .7 &&
        [language lengthOfBytesUsingEncoding:NSUTF8StringEncoding] < 16)
        snprintf(tag, 16, "%s", language.UTF8String);
}
static void cancel(void *context, uint64_t operation) {
    Desktop *d = (__bridge Desktop *)context;
    if (d.operation == operation)
        [d.task cancel];
}
static char *snapshot_on_owner(Desktop *d) {
    char *health = ot_health(d->core);
    if (!health) {
        return NULL;
    }
    NSData *data = [NSData dataWithBytes:health length:strlen(health)];
    free(health);
    id decoded = [NSJSONSerialization JSONObjectWithData:data options:0 error:NULL];
    if (![decoded isKindOfClass:NSDictionary.class]) return NULL;
    NSMutableDictionary *value = [decoded mutableCopy];
    value[@"ui"] = d.uiStatus ?: @"starting";
    value[@"connectionDiscovery"] = @{@"editing":@(d.connectionEditing),@"restoring":@(d.connectionRestoring)};
    const ot_preferences *network = ot_get_preferences(d->core);
    value[@"networkPolicy"] = @{@"enabled":@(network->internet),@"permissions":@(network->network),
        @"activeRequests":@(d.libraryTasks.count + (d.task ? 1 : 0)),
        @"accepted":@[@(d->networkAccepted[0]),@(d->networkAccepted[1]),@(d->networkAccepted[2]),@(d->networkAccepted[3]),@(d->networkAccepted[4])],
        @"blocked":@[@(d->networkBlocked[0]),@(d->networkBlocked[1]),@(d->networkBlocked[2]),@(d->networkBlocked[3]),@(d->networkBlocked[4])]};
    if (d.rendererFault)
        value[@"rendererFault"] = d.rendererFault;
    if (ot_get_preferences(d->core)->developer && d.networkMetrics.count)
        value[@"network"] = [d.networkMetrics copy];
    if (d.windowLayout)
        value[@"window"] = d.windowLayout;
    if (d.readingMetrics)
        value[@"reading"] = d.readingMetrics;
    if (d.renderMetrics)
        value[@"rendering"] = d.renderMetrics;
    if (d.motionMetrics)
        value[@"motion"] = d.motionMetrics;
    char *resources = ot_resources_view(d->core);
    if (resources) {
        NSData *r = [NSData dataWithBytes:resources length:strlen(resources)];
        value[@"resources"] = [NSJSONSerialization JSONObjectWithData:r options:0 error:NULL];
        free(resources);
    }
    char *library = ot_library_health(d->library);
    if (library) {
        NSData *r = [NSData dataWithBytes:library length:strlen(library)];
        value[@"library"] = [NSJSONSerialization JSONObjectWithData:r options:0 error:NULL];
        free(library);
    }
    NSData *json = [NSJSONSerialization dataWithJSONObject:value options:0 error:NULL];
    NSString *line = [[[NSString alloc] initWithData:json
                                            encoding:NSUTF8StringEncoding] stringByAppendingString:@"\n"];
    return line ? strdup(line.UTF8String) : NULL;
}
static char *snapshot(void *context) {
    Desktop *d = (__bridge Desktop *)context;
    __block char *result;
    dispatch_sync(d->owner, ^{ @autoreleasepool { result = snapshot_on_owner(d); } });
    return result;
}
/* The socket thread never owns UI state or blocks shutdown on the main queue.
 * Only bounded named UI/window controls are accepted, only in developer mode. */
static char *local_request_impl(void *context, const char *request) {
    if (!strcmp(request, "inspect"))
        return snapshot(context);
    Desktop *d = (__bridge Desktop *)context;
    __block BOOL allowed = NO;
    dispatch_sync(d->owner, ^{ allowed = ot_get_preferences(d->core)->developer; });
    if (!allowed || strncmp(request, "control ", 8))
        return strdup("{\"ok\":false,\"error\":\"control_unavailable\"}\n");
    NSString *command = @(request + 8);
    NSLock *lock = [NSLock new];
    dispatch_semaphore_t ready = dispatch_semaphore_create(0);
    __block NSString *reply = nil;
    __block BOOL expired = NO;
    void (^finish)(NSDictionary *) = ^(NSDictionary *value) {
      NSData *json = [NSJSONSerialization dataWithJSONObject:value options:0 error:NULL];
      [lock lock];
      if (!expired && !reply)
          reply = [[[NSString alloc] initWithData:json encoding:NSUTF8StringEncoding] stringByAppendingString:@"\n"];
      [lock unlock];
      dispatch_semaphore_signal(ready);
    };
    dispatch_async(dispatch_get_main_queue(), ^{
      [lock lock];
      BOOL cancelled = expired;
      [lock unlock];
      if (cancelled) return;
      if (d.terminating || !d.window || !d.web) {
          finish(@{@"ok":@NO, @"error":@"window_unavailable"});
          return;
      }
      if ((d.fileBusy || d.locationBusy || d.window.attachedSheet || NSApp.modalWindow) &&
          ![command isEqual:@"show"] && ![command isEqual:@"state"] &&
          ![command isEqual:@"observe 0"] && ![command isEqual:@"trace-state"]) {
          finish(@{@"ok":@NO, @"error":@"control_busy"});
          return;
      }
      if ([command isEqual:@"show"]) {
          [d recoverWindow:^(BOOL visible) {
            [d layoutChrome];
            NSMutableDictionary *value = [@{@"ok":@(visible), @"pid":@(getpid()),
                                             @"window":d.windowLayout ?: @{},
                                             @"windowNumber":@(d.window.windowNumber),
                                             @"workspaceWindows":@([NSApp.windows filteredArrayUsingPredicate:
                                                 [NSPredicate predicateWithBlock:^BOOL(NSWindow *w, NSDictionary *bindings) {
                                                     (void)bindings; return [w isKindOfClass:WorkspaceWindow.class];
                                                 }]].count)} mutableCopy];
            if (!visible) value[@"error"] = @"window_not_active";
            finish(value);
          }];
      } else if ([command isEqual:@"hide"]) {
          [NSApp hide:nil];
          finish(@{@"ok":@YES,@"hidden":@(NSApp.hidden)});
      } else if ([command hasPrefix:@"observe "]) {
          unsigned seconds = 0;
          int end = 0;
          if (sscanf(command.UTF8String,"observe %u%n",&seconds,&end) != 1 ||
              command.UTF8String[end] || seconds > 30) {
              finish(@{@"ok":@NO, @"error":@"invalid_observation"});
              return;
          }
          if (d.observationTimer) dispatch_source_cancel(d.observationTimer);
          d.observationTimer = nil;
          if (!seconds) { [d stopObservation]; finish(@{@"ok":@YES}); return; }
          if (!d.observationActivity)
              d.observationActivity = [NSProcessInfo.processInfo
                  beginActivityWithOptions:NSActivityUserInitiatedAllowingIdleSystemSleep |
                                           NSActivityIdleDisplaySleepDisabled
                                    reason:@"Bounded OverTone motion measurement"];
          d.observationTimer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER,0,0,dispatch_get_main_queue());
          dispatch_source_set_timer(d.observationTimer,
              dispatch_time(DISPATCH_TIME_NOW,(int64_t)seconds * (int64_t)NSEC_PER_SEC),
              DISPATCH_TIME_FOREVER,NSEC_PER_MSEC);
          __weak Desktop *weak = d;
          dispatch_source_set_event_handler(d.observationTimer, ^{ [weak stopObservation]; });
          dispatch_resume(d.observationTimer);
          [d layoutChrome];
          finish(@{@"ok":@YES,
                     @"visibilityReady":@((BOOL)(d.window.isOnActiveSpace && d.window.isVisible &&
                                  (d.window.occlusionState & NSWindowOcclusionStateVisible))),
                     @"pid":@(getpid()),@"window":d.windowLayout ?: @{}});
      } else if ([command isEqual:@"trace-state"]) {
          finish(@{@"ok":@YES,@"trace":d.motionTrace.state ?: @{@"phase":@"idle"}});
      } else if ([command hasPrefix:@"trace "] || [command hasPrefix:@"record "]) {
          BOOL recording = [command hasPrefix:@"record "];
          NSString *traceRequest = recording ? [@"trace " stringByAppendingString:[command substringFromIndex:7]] : command;
          unsigned seconds = 0;
          unsigned position = 0, first = 0, last = 0;
          char axis = 0;
          int end = 0, values = sscanf(traceRequest.UTF8String,"trace %u %c:%u:%u:%u%n",&seconds,&axis,&position,&first,&last,&end);
          if (values == 3) sscanf(traceRequest.UTF8String,"trace %u %c:%u%n",&seconds,&axis,&position,&end);
          if (values == 1) sscanf(traceRequest.UTF8String,"trace %u%n",&seconds,&end);
          if ((values != 1 && values != 3 && values != 5) || traceRequest.UTF8String[end] || !seconds || seconds > 10 ||
              (values >= 3 && (position > 65535 || (axis != 'h' && axis != 'v'))) ||
              (values == 5 && (last <= first || last > 65535))) {
              finish(@{@"ok":@NO,@"error":@"invalid_trace"});
              return;
          }
          NSString *phase = d.motionTrace.state[@"phase"];
          if ([phase isEqual:@"starting"] || [phase isEqual:@"recording"] ||
              [phase isEqual:@"stopping"] || [phase isEqual:@"writing"]) {
              finish(@{@"ok":@NO,@"error":@"trace_busy"});
              return;
          }
          d.motionTrace = [OTMotionTrace new];
          [d.motionTrace startForWindow:d.window seconds:seconds
                              vertical:axis == 'v' position:values >= 3 ? (NSInteger)position : -1
                                 range:values == 5 ? NSMakeRange(first,last-first) : NSMakeRange(NSNotFound,0)
                             recording:recording
                                  path:[d.profile stringByAppendingPathComponent:@"diagnostic-motion.json"]];
          finish(@{@"ok":@YES,@"trace":d.motionTrace.state});
      } else if ([command hasPrefix:@"resize "]) {
          unsigned width = 0, height = 0;
          int end = 0;
          NSRect screen = (d.window.screen ?: NSScreen.mainScreen).visibleFrame;
          if (sscanf(command.UTF8String, "resize %u %u%n", &width, &height, &end) != 2 ||
              command.UTF8String[end] || width < 640 || height < 480 ||
              width > screen.size.width || height > screen.size.height ||
              (d.window.styleMask & NSWindowStyleMaskFullScreen)) {
              finish(@{@"ok":@NO, @"error":@"invalid_window_size"});
              return;
          }
          NSRect frame = NSMakeRect(NSMidX(screen) - width / 2., NSMidY(screen) - height / 2., width, height);
          [d.window setFrame:frame display:YES];
          [d layoutChrome];
          finish(@{@"ok":@YES, @"window":d.windowLayout ?: @{}});
      } else if ([command isEqual:@"menu"] || [command hasPrefix:@"menu "] || [command hasPrefix:@"shortcut "]) {
          BOOL listing = [command isEqual:@"menu"];
          NSString *identifier = listing ? @"" : [command substringFromIndex:[command hasPrefix:@"menu "] ? 5 : 9];
          NSMenuItem *item = d.workspaceMenuItems[identifier];
          if (!listing && (!item || ![d validateMenuItem:item])) {
              finish(@{@"ok":@NO,@"error":item ? @"control_not_available" : @"unknown_control"}); return;
          }
          if (item) {
              if ([command hasPrefix:@"shortcut "]) {
                  if (!item.keyEquivalent.length) {finish(@{@"ok":@NO,@"error":@"shortcut_unavailable"});return;}
                  NSEvent *event = [NSEvent keyEventWithType:NSEventTypeKeyDown location:NSZeroPoint
                      modifierFlags:item.keyEquivalentModifierMask timestamp:NSProcessInfo.processInfo.systemUptime
                      windowNumber:d.window.windowNumber context:nil characters:item.keyEquivalent
                      charactersIgnoringModifiers:item.keyEquivalent isARepeat:NO keyCode:0];
                  finish(@{@"ok":@([NSApp.mainMenu performKeyEquivalent:event])});
              } else { [d performWorkspaceAction:item]; finish(@{@"ok":@YES}); }
              return;
          }
          NSMutableArray *menus = [NSMutableArray new];
          for (NSMenuItem *top in NSApp.mainMenu.itemArray) {
              [top.submenu update];
              NSMutableArray *items = [NSMutableArray new];
              for (NSMenuItem *row in top.submenu.itemArray) {
                  if (row.separatorItem) continue;
                  [items addObject:@{@"title":row.title,@"key":row.keyEquivalent,
                      @"modifiers":@(row.keyEquivalentModifierMask),@"selected":@(row.state == NSControlStateValueOn),
                      @"enabled":@((BOOL)(row.target == d ? [d validateMenuItem:row] : row.enabled)),
                      @"command":row.target == d && [row.representedObject isKindOfClass:NSString.class] ? row.representedObject : @""}];
              }
              [menus addObject:@{@"title":top.title,@"items":items}];
          }
          finish(@{@"ok":@YES,@"menus":menus});
      } else if ([command isEqual:@"capture"]) {
          void (^save)(CGImageRef,NSError *)=^(CGImageRef pixels,NSError *error) {
            if (error || !pixels) { finish(@{@"ok":@NO, @"error":@"capture_failed"}); return; }
            NSString *path = [d.profile stringByAppendingPathComponent:@"diagnostic-ui.png"];
            CGImageRetain(pixels);
            dispatch_group_async(d.fileWork, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
              @autoreleasepool {
                  NSBitmapImageRep *bitmap = [[NSBitmapImageRep alloc] initWithCGImage:pixels];
                  CGImageRelease(pixels);
                  NSData *png = [bitmap representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
                  BOOL saved = png && [png writeToFile:path options:NSDataWritingAtomic error:NULL];
                  finish(saved ? @{@"ok":@YES, @"path":path} : @{@"ok":@NO, @"error":@"capture_failed"});
              }
            });
          };
          if (d.loading && !d.loading.hidden) [OTMotionTrace captureWindow:d.window completion:save];
          else {
              WKSnapshotConfiguration *configuration=[WKSnapshotConfiguration new];configuration.rect=d.web.bounds;
              [d.web takeSnapshotWithConfiguration:configuration completionHandler:^(NSImage *image,NSError *error) {
                  save(image ? [image CGImageForProposedRect:NULL context:nil hints:nil] : NULL,error);
              }];
          }
      } else if ([command isEqual:@"diagnostics off"]) {
          // A local test handoff can remove its own privilege without closing the
          // first-run dialog or changing the user's other saved preferences.
          dispatch_async(d->owner, ^{
              ot_preferences preferences = *ot_get_preferences(d->core);
              preferences.developer = false;
              yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
              char *json = NULL;
              if (doc) {
                  yyjson_mut_doc_set_root(doc,ot_preferences_json(doc,&preferences));
                  json = yyjson_mut_write(doc,0,NULL);
              }
              BOOL saved = json && ot_set_preferences(d->core,json);
              free(json); yyjson_mut_doc_free(doc);
              dispatch_async(dispatch_get_main_queue(), ^{
                  if (saved) { [d stopObservation]; [d.motionTrace cancel]; [d requestRender]; }
                  finish(saved ? @{@"ok":@YES} : @{@"ok":@NO,@"error":@"storage_write"});
              });
          });
      } else if ([command isEqual:@"quit"]) {
          finish(@{@"ok":@YES});
          /* AppKit may enter its termination run loop before returning. Invoke
           * outside a dispatch item so deferred draft callbacks can drain main. */
          [NSApp performSelector:@selector(terminate:) withObject:nil afterDelay:0];
      } else if ([command isEqual:@"reload"]) {
          [d.web reload];finish(@{@"ok":@YES});
      } else if ([command isEqual:@"fullscreen"]) {
          if (d.chromeTransition) finish(@{@"ok":@NO, @"error":@"transition_in_progress"});
          else { [d stopObservation]; [d.window toggleFullScreen:nil]; finish(@{@"ok":@YES}); }
      } else if ([command hasPrefix:@"press "] || [command hasPrefix:@"fill "] || [command hasPrefix:@"focus "] || [command hasPrefix:@"key "] || [command isEqual:@"context-menu"] || [command isEqual:@"state"]) {
          [d.web callAsyncJavaScript:@"return window.overtoneControl?.(actions) || {ok:false,error:'renderer_unavailable'}"
                         arguments:@{@"actions":[command hasPrefix:@"press "] ? [command substringFromIndex:6] : command}
                           inFrame:nil inContentWorld:WKContentWorld.pageWorld
                 completionHandler:^(id value, NSError *error) {
                   if (error || ![value isKindOfClass:NSDictionary.class]) {
                       finish(@{@"ok":@NO,@"error":@"renderer_unavailable"});return;
                   }
                   NSMutableDictionary *result=[value mutableCopy];
                   if ([command isEqual:@"state"] && d.loading) {
                       NSDictionary *native=d.loading.snapshot;
                       NSMutableDictionary *startup=[result[@"startup"] mutableCopy] ?: [NSMutableDictionary new];
                       [startup addEntriesFromDictionary:native];startup[@"native"]=@YES;result[@"startup"]=startup;
                       result[@"finiteAnimations"]=@([result[@"finiteAnimations"] unsignedIntegerValue]+[native[@"finiteAnimations"] unsignedIntegerValue]);
                       NSMutableDictionary *brand=[result[@"branding"] mutableCopy];brand[@"state"]=native[@"state"];brand[@"colours"]=native[@"colours"];result[@"branding"]=brand;
                   }
                   finish(result);
                 }];
      } else finish(@{@"ok":@NO, @"error":@"unknown_control"});
    });
    dispatch_semaphore_wait(ready, dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC));
    [lock lock];
    expired = YES;
    char *result = strdup(reply ? reply.UTF8String : "{\"ok\":false,\"error\":\"control_timeout\"}\n");
    [lock unlock];
    return result;
}
static char *local_request(void *context, const char *request) {
    @autoreleasepool { return local_request_impl(context,request); }
}
/* URLSession supplies TLS, redirects and backpressure; the C owner sees only
 * response bytes and a terminal callback, always on the same serial executor. */
static NSURL *destination(NSString *base, NSString *protocol, BOOL remote) {
    return OTServerChat(base,[protocol isEqual:@"ollama"],remote);
}
static BOOL network_allowed(Desktop *d, NSURL *url, ot_network_scope scope) {
    if (!url || !d->core) return NO;
    BOOL local = OTServerLocal(url);
    if (scope == OT_NETWORK_LOCAL) return local;
    if (scope == OT_NETWORK_SERVERS && local) return YES;
    return ot_network_allowed(ot_get_preferences(d->core), scope);
}
/* Counts describe admitted attempts, not bytes, domains or OS background traffic. */
static BOOL network_admit(Desktop *d, NSURL *url, ot_network_scope scope) {
    unsigned index = scope == OT_NETWORK_CATALOGUES ? 1 : scope == OT_NETWORK_MODELS ? 2 :
                     scope == OT_NETWORK_ENGINES ? 3 : scope == OT_NETWORK_SERVERS ? 4 : 0;
    if (scope == OT_NETWORK_SERVERS && OTServerLocal(url)) index = 0;
    BOOL allowed = network_allowed(d,url,scope);
    if (allowed) d->networkAccepted[index]++;else d->networkBlocked[index]++;
    return allowed;
}
static NSURLSession *network_session(Desktop *d, NSURL *url) {
    if (OTServerLocal(url)) return d.localSession;
    // Called only after admission; no remote session/proxy setup before opt-in.
    if (!d.session) {
        NSURLSessionConfiguration *c = OTNetworkConfiguration(NO);
        c.timeoutIntervalForRequest = 60; c.timeoutIntervalForResource = 86400;
        d.session = [NSURLSession sessionWithConfiguration:c delegate:d delegateQueue:d.networkCallbacks];
    }
    return d.session;
}
static ot_network_scope task_scope(NSURLSessionTask *task) {
    NSArray<NSString *> *parts = [task.taskDescription componentsSeparatedByString:@":"];
    return parts.count == 2 ? (ot_network_scope)parts.lastObject.intValue : OT_NETWORK_SERVERS;
}
static void request(void *context, uint64_t operation, const char *body) {
    Desktop *d = (__bridge Desktop *)context;
    const ot_connection *connection = ot_configuration(d->core);
    NSURL *url =
        destination(@(connection->endpoint), connection->ollama ? @"ollama" : @"openai", connection->remote);
    d.operation = operation;
    d.transportError = nil;
    if (!url || !network_admit(d, url, OT_NETWORK_SERVERS)) {
        dispatch_async(d->owner, ^{
          if (d->core) ot_end(d->core, operation, url ? "internet_disabled" : "invalid_input");
        });
        return;
    }
    NSMutableURLRequest *request = [NSMutableURLRequest requestWithURL:url];
    request.HTTPMethod = @"POST";
    request.HTTPBody = [NSData dataWithBytes:body length:strlen(body)];
    [request setValue:@"application/json" forHTTPHeaderField:@"Content-Type"];
    NSString *key = session_key(d,OTServerBase(@(connection->endpoint),[@(connection->endpoint) hasPrefix:@"https:"]));
    if (key.length)
        [request setValue:[@"Bearer " stringByAppendingString:key] forHTTPHeaderField:@"Authorization"];
    d.task = [network_session(d,url) dataTaskWithRequest:request];
    [d.task resume];
}

static BOOL libraryURL(NSURL *url) {
    NSURLComponents *c = [NSURLComponents componentsWithURL:url resolvingAgainstBaseURL:NO];
    BOOL local = [@[ @"localhost", @"127.0.0.1", @"[::1]", @"::1" ] containsObject:c.host.lowercaseString];
    return c.host.length && !c.user && !c.password && !c.fragment &&
           ([c.scheme.lowercaseString isEqual:@"https"] ||
            (local && [c.scheme.lowercaseString isEqual:@"http"]));
}
static void library_request(void *context, uint64_t id, const char *method, const char *url, const char *body,
                            const char *etag, uint64_t offset, ot_network_scope scope) {
    Desktop *d = (__bridge Desktop *)context;
    NSURL *target = [NSURL URLWithString:@(url)];
    if (!libraryURL(target) || !network_admit(d, target, scope)) {
        dispatch_async(d->owner, ^{
          if (d->library)
              ot_library_end(d->library, id, libraryURL(target) ? "internet_disabled" : "invalid_url");
        });
        return;
    }
    NSMutableURLRequest *request = [NSMutableURLRequest requestWithURL:target];
    request.HTTPMethod = @(method);
    request.timeoutInterval =
        [target.path hasSuffix:@"/health"] || [target.path hasSuffix:@"/api/tags"] ? 3 : 60;
    if (body) {
        request.HTTPBody = [NSData dataWithBytes:body length:strlen(body)];
        [request setValue:@"application/json" forHTTPHeaderField:@"Content-Type"];
    }
    // Resumable files must keep byte offsets/content lengths on the wire. JSON
    // metadata can use URLSession's negotiated compression and automatic decoding.
    if (etag || !strcmp(method, "HEAD"))
        [request setValue:@"identity" forHTTPHeaderField:@"Accept-Encoding"];
    if (offset) {
        [request setValue:[NSString stringWithFormat:@"bytes=%llu-", (unsigned long long)offset]
            forHTTPHeaderField:@"Range"];
        if (etag && *etag)
            [request setValue:@(etag) forHTTPHeaderField:@"If-Range"];
    }
    NSURLSessionDataTask *task = [network_session(d,target) dataTaskWithRequest:request];
    task.taskDescription = [NSString stringWithFormat:@"%llu:%u", (unsigned long long)id, (unsigned)scope];
    d.libraryTasks[@(id)] = task;
    [task resume];
}
static void library_cancel(void *context, uint64_t id) {
    Desktop *d = (__bridge Desktop *)context;
    [d.libraryTasks[@(id)] cancel];
    [d.libraryTasks removeObjectForKey:@(id)];
}
static void library_connected(void *context) {
    Desktop *d = (__bridge Desktop *)context;
    d.sessionCredential = nil;
    dispatch_async(dispatch_get_main_queue(), ^{ [d.connectionManager cancel]; d.connectionRestoring = NO; });
}

@implementation Desktop
- (void)checkWindowVisibilityUntil:(NSTimeInterval)deadline completion:(void (^)(BOOL))completion {
    BOOL visible = NSApp.isActive && self.window.isKeyWindow && self.window.isOnActiveSpace && self.window.isVisible &&
                   (self.window.occlusionState & NSWindowOcclusionStateVisible);
    if (visible || self.terminating || NSProcessInfo.processInfo.systemUptime >= deadline) {
        completion(visible);
        return;
    }
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW,50*NSEC_PER_MSEC),dispatch_get_main_queue(), ^{
      [self checkWindowVisibilityUntil:deadline completion:completion];
    });
}
- (void)recoverWindow:(void (^)(BOOL))completion {
    if (NSApp.isActive && self.window.isKeyWindow && self.window.isOnActiveSpace && self.window.isVisible &&
        (self.window.occlusionState & NSWindowOcclusionStateVisible)) {
        completion(YES);
        return;
    }
    [NSApp unhide:nil];
    if (self.window.isMiniaturized) [self.window deminiaturize:nil];
    [self.window makeKeyAndOrderFront:nil];
    NSWorkspaceOpenConfiguration *configuration = [NSWorkspaceOpenConfiguration configuration];
    configuration.activates = YES;
    configuration.addsToRecentItems = NO;
    // Activate the app's existing Space. Never move its window into the caller's
    // desktop or join other apps' Spaces just to obtain a measurement.
    NSTimeInterval deadline = NSProcessInfo.processInfo.systemUptime + 1.5;
    [NSWorkspace.sharedWorkspace openApplicationAtURL:NSBundle.mainBundle.bundleURL
                                       configuration:configuration completionHandler:nil];
    // Space activation is asynchronous; report its actual result.
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW,500*NSEC_PER_MSEC),dispatch_get_main_queue(), ^{
        if (!self.window.isOnActiveSpace) {
            NSInteger index = 0;
            for (NSMenuItem *item in NSApp.windowsMenu.itemArray) {
                if (item.target == self.window || item.representedObject == self.window) {
                    [NSApp.windowsMenu performActionForItemAtIndex:index];
                    break;
                }
                index++;
            }
        }
        [self checkWindowVisibilityUntil:deadline completion:completion];
    });
}
- (void)stopObservation {
    if (self.observationTimer) dispatch_source_cancel(self.observationTimer);
    self.observationTimer = nil;
    if (!self.observationActivity) return;
    [NSProcessInfo.processInfo endActivity:self.observationActivity];
    self.observationActivity = nil;
    [self layoutChrome];
}
- (void)setTheme:(NSString *)theme {
    // Keep the application at the OS appearance so its effectiveAppearance remains
    // an independent system signal. Only this window overrides the user's choice.
    NSApp.appearance = nil;
    self.window.appearance = [theme isEqual:@"system"]
                           ? nil
                           : [NSAppearance appearanceNamed:[theme isEqual:@"dark"] ? NSAppearanceNameDarkAqua
                                                                                   : NSAppearanceNameAqua];
}
- (void)observeValueForKeyPath:(NSString *)keyPath ofObject:(id)object
                       change:(NSDictionary<NSKeyValueChangeKey, id> *)change context:(void *)context {
    if (context != &appearance_context) {
        [super observeValueForKeyPath:keyPath ofObject:object change:change context:context];
        return;
    }
    dispatch_async(dispatch_get_main_queue(), ^{
      NSString *script = [NSString stringWithFormat:
          @"document.documentElement.dataset.systemTheme='%@';window.updateChromeAppearance?.()", system_theme()];
      [self.web evaluateJavaScript:script completionHandler:nil];
      [self layoutChrome];
    });
}
- (void)setWindowControlsHidden:(BOOL)hidden {
    for (NSNumber *kind in @[@(NSWindowCloseButton), @(NSWindowMiniaturizeButton), @(NSWindowZoomButton)])
        [self.window standardWindowButton:(NSWindowButton)kind.integerValue].hidden = hidden;
}
- (void)layoutChrome {
    if (!self.web.window || !self.chrome)
        return;
    NSRect bounds = self.web.superview.bounds;
    if (!NSEqualRects(self.web.frame, bounds))
        self.web.frame = bounds;
    const CGFloat height = 64;
    CGFloat left = 0, right = 0;
    BOOL fullScreen = (self.window.styleMask & NSWindowStyleMaskFullScreen) != 0;
    BOOL targetFullScreen = self.chromeTransition ? self.chromeTransition > 0 : fullScreen;
    NSMutableDictionary *windowLayout = [self.windowLayout mutableCopy] ?: [NSMutableDictionary new];
    BOOL activeChanged = [windowLayout[@"active"] boolValue] != self.window.isKeyWindow;
    [windowLayout addEntriesFromDictionary:@{
        @"width" : @(self.window.frame.size.width),
        @"x" : @(self.window.frame.origin.x),
        @"y" : @(self.window.frame.origin.y),
        @"dragEvents" : @(self.chrome.dragEvents),
        @"height" : @(self.window.frame.size.height),
        @"contentWidth" : @(bounds.size.width),
        @"contentHeight" : @(bounds.size.height),
        @"webWidth" : @(self.web.frame.size.width),
        @"webBoundsWidth" : @(self.web.bounds.size.width),
        @"webHeight" : @(self.web.frame.size.height),
        @"fullScreen" : @(fullScreen),
        @"active" : @(self.window.isKeyWindow),
        @"applicationActive" : @(NSApp.isActive),
        @"applicationHidden" : @(NSApp.isHidden),
        @"onActiveSpace" : @(self.window.isOnActiveSpace),
        @"level" : @(self.window.level),
        @"observationActive" : @(self.observationActivity != nil),
        @"collectionBehavior" : @(self.window.collectionBehavior),
        @"hidesOnDeactivate" : @(self.window.hidesOnDeactivate),
        @"visible" : @(self.window.isVisible),
        @"occluded" : @(!(self.window.occlusionState & NSWindowOcclusionStateVisible)),
        @"systemDark" : @([system_theme() isEqual:@"dark"]),
        @"maximumRefreshHz" : @((self.window.screen ?: NSScreen.mainScreen).maximumFramesPerSecond),
        @"fullScreenPreparations" : @(((WorkspaceWindow *)self.window).fullScreenPreparation),
        @"chromeHeight" : @(height),
        @"dragEnabled" : @(!fullScreen && !self.chromeTransition)
    }];
    for (NSNumber *kind in @[ @(NSWindowCloseButton), @(NSWindowMiniaturizeButton), @(NSWindowZoomButton) ]) {
        NSButton *button = [self.window standardWindowButton:(NSWindowButton)kind.integerValue];
        if (!button || button.window != self.window)
            continue;
        NSRect r = [button convertRect:button.bounds toView:self.web];
        if (NSMidX(r) < self.web.bounds.size.width / 2)
            left = MAX(left, NSMaxX(r) + 12);
        else
            right = MAX(right, self.web.bounds.size.width - NSMinX(r) + 12);
    }
    windowLayout[@"windowButtonsRight"] = @(MAX(0, left - 12));
    if (fullScreen || (left <= 0 && right <= 0)) {
        /* Full-screen controls reappear over the content on pointer entry. Keep their space. */
        left = self.chrome.leading;
        right = self.chrome.trailing;
        if (left <= 0 && right <= 0)
            left = 112; /* Includes AppKit's extra inset when restoring the toolbar. */
    }
    /* AppKit can move its buttons inward by 10 pt when restoring the toolbar after
     * full screen. Keep the largest observed safe area for this window's lifetime. */
    left = MAX(MAX(left, self.chrome.leading), 112);
    right = MAX(right, self.chrome.trailing);
    windowLayout[@"controlsLeft"] = @(left);
    windowLayout[@"controlsRight"] = @(right);
    self.windowLayout = [windowLayout copy];
    BOOL changed = height != self.chrome.bounds.size.height ||
                   bounds.size.width != self.chrome.bounds.size.width || left != self.chrome.leading ||
                   right != self.chrome.trailing || targetFullScreen != self.chromeFullScreen ||
                   self.chromeTransition != self.chromePublishedTransition || activeChanged;
    self.chrome.leading = left;
    self.chrome.trailing = right;
    self.chromeFullScreen = targetFullScreen;
    self.chromePublishedTransition = self.chromeTransition;
    self.chrome.draggingAllowed = !fullScreen && !self.chromeTransition;
    self.chrome.hidden = !self.chrome.draggingAllowed;
    self.chrome.frame = NSMakeRect(0, bounds.size.height - height, bounds.size.width, height);
    if (!changed && self.chromePublished)
        return;
    [self.window invalidateCursorRectsForView:self.chrome];
    [self.web callAsyncJavaScript:@"if (!window.overtoneChromeLayout) return false;"
                                   "window.overtoneChromeLayout(height,left,right,fullScreen,transition,active);return true"
                        arguments:@{
                            @"height" : @(height),
                            @"left" : @(left),
                            @"right" : @(right),
                            @"fullScreen" : @(targetFullScreen),
                            @"transition" : @(self.chromeTransition),
                            @"active" : @(self.window.isKeyWindow)
                        }
                          inFrame:nil
                   inContentWorld:WKContentWorld.pageWorld
                completionHandler:^(id value, NSError *error) {
                  self.chromePublished = error == nil && [value isEqual:@YES];
                  NSMutableDictionary *layout = [self.windowLayout mutableCopy];
                  layout[@"chromeReady"] = @(self.chromePublished);
                  layout[@"chromeError"] = error ? @(error.code) : @0;
                  if ([error.userInfo[@"WKJavaScriptExceptionLineNumber"] isKindOfClass:NSNumber.class])
                      layout[@"chromeErrorLine"] = error.userInfo[@"WKJavaScriptExceptionLineNumber"];
                  self.windowLayout = layout;
                }];
}
- (void)windowDidResize:(NSNotification *)notification {
    (void)notification;
    [self layoutChrome];
}
- (void)windowDidMove:(NSNotification *)notification {
    (void)notification;
    [self layoutChrome];
}
- (void)windowDidBecomeKey:(NSNotification *)notification {
    (void)notification;
    [self layoutChrome];
    [self.web callAsyncJavaScript:@"window.setRegionalEnvironment?.(environment)"
                       arguments:@{@"environment" : regional_environment()}
                         inFrame:nil
                  inContentWorld:WKContentWorld.pageWorld
               completionHandler:nil];
}
- (void)applicationDidBecomeActive:(NSNotification *)notification {
    (void)notification;
    [self layoutChrome];
}
- (void)applicationDidResignActive:(NSNotification *)notification {
    (void)notification;
    [self layoutChrome];
}
- (void)applicationDidHide:(NSNotification *)notification {
    (void)notification;
    [self layoutChrome];
}
- (void)applicationDidUnhide:(NSNotification *)notification {
    (void)notification;
    [self layoutChrome];
}
- (void)windowDidChangeOcclusionState:(NSNotification *)notification {
    (void)notification;
    [self layoutChrome];
}
- (void)windowDidResignKey:(NSNotification *)notification {
    (void)notification;
    [self layoutChrome];
}
- (void)windowDidEnterFullScreen:(NSNotification *)notification {
    (void)notification;
    [self setWindowControlsHidden:NO];
    self.chromeTransition = 0;
    [self layoutChrome];
}
- (void)windowWillEnterFullScreen:(NSNotification *)notification {
    (void)notification;
    self.chromeTransition = 1;
    /* An empty AppKit toolbar must not slide over the web navigation in full screen. */
    self.window.toolbar.visible = NO;
    [self layoutChrome];
    self.window.movableByWindowBackground = NO;
    self.chrome.draggingAllowed = NO;
    self.chrome.hidden = YES;
    [self.window invalidateCursorRectsForView:self.chrome];
}
- (void)windowWillExitFullScreen:(NSNotification *)notification {
    (void)notification;
    /* Reserve the restored controls before AppKit starts bringing them back. */
    self.chromeTransition = -1;
    [self layoutChrome];
    self.window.toolbar.visible = YES;
}
- (void)windowDidExitFullScreen:(NSNotification *)notification {
    (void)notification;
    [self setWindowControlsHidden:NO];
    self.chromeTransition = 0;
    self.window.movableByWindowBackground = YES;
    [self layoutChrome];
}
- (void)windowDidFailToEnterFullScreen:(NSWindow *)window {
    (void)window;
    [self setWindowControlsHidden:NO];
    self.chromeTransition = 0;
    self.window.toolbar.visible = YES;
    self.window.movableByWindowBackground = YES;
    [self layoutChrome];
}
- (void)windowDidFailToExitFullScreen:(NSWindow *)window {
    (void)window;
    [self setWindowControlsHidden:NO];
    self.chromeTransition = 0;
    self.window.toolbar.visible = NO;
    [self layoutChrome];
}
- (void)webView:(WKWebView *)webView didStartProvisionalNavigation:(WKNavigation *)navigation {
    (void)webView;
    (void)navigation;
    [self.loading prepare];
    self.rendererEpoch++;
    self.shownRevision = self.shownStreamRevision = self.shownLibraryRevision = self.shownMemoryRevision = 0;
    self.rendering = self.memoryRendering = NO;
    self.resourceSampleTime = 0;
    self.rendererFault = nil;
    self.uiStatus = @"loading";
}
- (void)webView:(WKWebView *)webView didFinishNavigation:(WKNavigation *)navigation {
    (void)webView;
    (void)navigation;
    self.chromePublished = NO;
    [self.web callAsyncJavaScript:@"window.overtoneSetLocale?.(code,strings)"
                        arguments:@{@"code": self.language, @"strings": self.translations}
                          inFrame:nil inContentWorld:WKContentWorld.pageWorld completionHandler:nil];
    [self layoutChrome];
    [self sendLocation];
    [self render];
    [self refreshConnection];
}
- (void)refreshConnection {
    if (self.opening || self.servicesStopped || !self.connectionManager) return;
    dispatch_async(self->owner, ^{
        if (!self->core || !self->library) return;
        ot_connection c = *ot_configuration(self->core);
        if (!*c.model || ot_library_manages_model(self->library)) return;
        if (!network_allowed(self, destination(@(c.endpoint),c.ollama ? @"ollama" : @"openai",c.remote), OT_NETWORK_SERVERS)) {
            ot_connection_ready(self->core,false); return;
        }
        const ot_server_info *known = ot_connection_model(self->core);
        if (!known) ot_connection_ready(self->core,false);
        NSString *key = session_key(self,OTServerBase(@(c.endpoint),[@(c.endpoint) hasPrefix:@"https:"])) ?: @"";
        BOOL authenticate = key.length || !known || known->has_key;
        dispatch_async(dispatch_get_main_queue(), ^{
            if (self.terminating || self.connectionEditing) return;
            self.connectionRestoring = YES;
            [self.connectionManager probe:@{@"endpoint":@(c.endpoint),@"model":@(c.model),
                @"protocol":c.ollama ? @"ollama" : @"openai",@"https":@([@(c.endpoint) hasPrefix:@"https:"]),
                @"key":key,@"useKey":@(authenticate),@"restore":@YES}];
        });
    });
}
- (NSString *)text:(NSString *)italian english:(NSString *)english {
    return self.translations[italian] ?: ([self.language isEqualToString:@"it"] ? italian : english);
}
- (void)showAbout:(id)sender {
    (void)sender;
    [NSApp orderFrontStandardAboutPanelWithOptions:@{
        NSAboutPanelOptionApplicationIcon : NSApp.applicationIconImage
    }];
}
- (void)addWorkspaceCommand:(NSString *)command title:(NSString *)title english:(NSString *)english
                       key:(NSString *)key modifiers:(NSEventModifierFlags)modifiers menu:(NSMenu *)menu {
    NSMenuItem *item = [menu addItemWithTitle:[self text:title english:english]
                                     action:@selector(performWorkspaceAction:) keyEquivalent:key];
    item.keyEquivalentModifierMask = modifiers;
    item.representedObject = command;
    item.target = self;
    item.state = [self.checkedWorkspaceActions containsObject:command] ? NSControlStateValueOn : NSControlStateValueOff;
    self.workspaceMenuItems[command] = item;
}
- (BOOL)validateMenuItem:(NSMenuItem *)item {
    if (item.action != @selector(performWorkspaceAction:)) return YES;
    return self.web && !self.opening && !self.fileBusy && !self.locationBusy &&
        !self.window.attachedSheet && (!NSApp.keyWindow || NSApp.keyWindow == self.window) &&
        [self.enabledWorkspaceActions containsObject:item.representedObject];
}
- (void)performWorkspaceAction:(NSMenuItem *)item {
    if (![self validateMenuItem:item]) return;
    [self.window makeKeyAndOrderFront:self];
    [self.web callAsyncJavaScript:@"return window.overtoneMenu?.(command,keyboard) || false"
                       arguments:@{@"command":item.representedObject,
                                   @"keyboard":@(NSApp.currentEvent.type == NSEventTypeKeyDown)} inFrame:nil
                  inContentWorld:WKContentWorld.pageWorld completionHandler:nil];
}
- (void)applyLanguage:(NSString *)language {
    if ([@[@"it", @"en", @"es", @"fr", @"de", @"pt", @"ru", @"zh", @"ja", @"ko"]
            containsObject:language] == NO)
        language = @"en";
    self.language = language;
    NSURL *file = [[NSBundle.mainBundle resourceURL]
        URLByAppendingPathComponent:[NSString stringWithFormat:@"ui/locales/%@.json", language]];
    NSData *data = [NSData dataWithContentsOfURL:file];
    id strings = data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:NULL] : nil;
    self.translations = [strings isKindOfClass:NSDictionary.class] ? strings : @{};
    if (self.web)
        [self.web callAsyncJavaScript:@"window.overtoneSetLocale(code,strings)"
                            arguments:@{@"code":language, @"strings":self.translations}
                              inFrame:nil inContentWorld:WKContentWorld.pageWorld completionHandler:nil];
    self.workspaceMenuItems = [NSMutableDictionary new];
    NSMenu *menu = [NSMenu new], *application = [NSMenu new], *edit = [NSMenu new];
    NSMenuItem *appItem = [[NSMenuItem alloc] initWithTitle:@"OverTone" action:nil keyEquivalent:@""];
    application.title = @"OverTone";
    [menu addItem:appItem];
    appItem.submenu = application;
    NSMenuItem *about = [application addItemWithTitle:[self text:@"Informazioni su OverTone"
                                                          english:@"About OverTone"]
                                               action:@selector(showAbout:)
                                        keyEquivalent:@""];
    about.target = self;
    [application addItem:NSMenuItem.separatorItem];
    [self addWorkspaceCommand:@"settings" title:@"Impostazioni" english:@"Settings" key:@","
                   modifiers:NSEventModifierFlagCommand menu:application];
    [application addItem:NSMenuItem.separatorItem];
    NSMenuItem *hide = [application addItemWithTitle:[self text:@"Nascondi OverTone" english:@"Hide OverTone"]
                                             action:@selector(hide:) keyEquivalent:@"h"];
    hide.target = NSApp;
    [application addItem:NSMenuItem.separatorItem];
    [application addItemWithTitle:[self text:@"Esci da OverTone" english:@"Quit OverTone"]
                           action:@selector(terminate:)
                    keyEquivalent:@"q"];
    NSMenuItem *fileItem = [[NSMenuItem alloc] initWithTitle:[self text:@"File" english:@"File"] action:nil keyEquivalent:@""];
    NSMenu *fileMenu = [[NSMenu alloc] initWithTitle:fileItem.title];
    fileItem.submenu = fileMenu; [menu addItem:fileItem];
    [self addWorkspaceCommand:@"new-chat" title:@"Nuova conversazione" english:@"New conversation" key:@"n"
                   modifiers:NSEventModifierFlagCommand menu:fileMenu];
    [self addWorkspaceCommand:@"find" title:@"Cerca" english:@"Search" key:@"f"
                   modifiers:NSEventModifierFlagCommand menu:fileMenu];
    [fileMenu addItem:NSMenuItem.separatorItem];
    [self addWorkspaceCommand:@"stop" title:@"Interrompi" english:@"Stop" key:@"."
                   modifiers:NSEventModifierFlagCommand menu:fileMenu];
    NSMenuItem *editItem = [[NSMenuItem alloc] initWithTitle:[self text:@"Modifica" english:@"Edit"]
                                                      action:nil
                                               keyEquivalent:@""];
    [menu addItem:editItem];
    editItem.submenu = edit;
    for (NSArray *binding in @[
        @[@"Annulla ultima modifica",@"Undo",@"undo:",@"z",@(NSEventModifierFlagCommand)],
        @[@"Ripeti",@"Redo",@"redo:",@"z",@(NSEventModifierFlagCommand | NSEventModifierFlagShift)],
        @[],
        @[@"Taglia",@"Cut",@"cut:",@"x",@(NSEventModifierFlagCommand)],
        @[@"Copia",@"Copy",@"copy:",@"c",@(NSEventModifierFlagCommand)],
        @[@"Incolla",@"Paste",@"paste:",@"v",@(NSEventModifierFlagCommand)],
        @[],
        @[@"Seleziona tutto",@"Select All",@"selectAll:",@"a",@(NSEventModifierFlagCommand)]]) {
        if (!binding.count) { [edit addItem:NSMenuItem.separatorItem]; continue; }
        NSMenuItem *item = [edit addItemWithTitle:[self text:binding[0] english:binding[1]]
                                         action:NSSelectorFromString(binding[2]) keyEquivalent:binding[3]];
        item.keyEquivalentModifierMask = [binding[4] unsignedIntegerValue];
    }
    NSMenuItem *viewItem = [[NSMenuItem alloc] initWithTitle:[self text:@"Vista" english:@"View"] action:nil keyEquivalent:@""];
    NSMenu *view = [[NSMenu alloc] initWithTitle:viewItem.title];
    viewItem.submenu = view; [menu addItem:viewItem];
    for (NSArray<NSString *> *entry in @[
        @[@"chat",@"Chat",@"Chat",@"1"], @[@"models",@"Modelli",@"Models",@"2"],
        @[@"knowledge",@"Documenti",@"Documents",@"3"], @[@"projects",@"Progetti",@"Projects",@"4"],
        @[@"tools",@"Strumenti",@"Tools",@"5"]])
        [self addWorkspaceCommand:entry[0] title:entry[1] english:entry[2] key:entry[3]
                       modifiers:NSEventModifierFlagCommand menu:view];
    [view addItem:NSMenuItem.separatorItem];
    [self addWorkspaceCommand:@"details" title:@"Dettagli" english:@"Details" key:@"i"
                   modifiers:NSEventModifierFlagCommand | NSEventModifierFlagShift menu:view];
    [self addWorkspaceCommand:@"parameters" title:@"Parametri del modello" english:@"Model parameters" key:@""
                   modifiers:0 menu:view];
    NSMenuItem *windowItem = [[NSMenuItem alloc] initWithTitle:[self text:@"Finestra" english:@"Window"]
                                                        action:nil
                                                 keyEquivalent:@""];
    [menu addItem:windowItem];
    NSMenu *windows = [[NSMenu alloc] initWithTitle:windowItem.title];
    windowItem.submenu = windows;
    [windows addItemWithTitle:[self text:@"Chiudi finestra" english:@"Close Window"]
                       action:@selector(performClose:)
                keyEquivalent:@"w"];
    [windows addItemWithTitle:[self text:@"Riduci a icona" english:@"Minimize"]
                       action:@selector(performMiniaturize:)
                keyEquivalent:@"m"];
    [windows addItemWithTitle:[self text:@"Ingrandisci" english:@"Zoom"]
                       action:@selector(performZoom:)
                keyEquivalent:@""];
    NSMenuItem *fullScreen = [windows addItemWithTitle:[self text:@"Schermo intero" english:@"Full Screen"]
                                                action:@selector(toggleFullScreen:)
                                         keyEquivalent:@"f"];
    fullScreen.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagControl;
    NSApp.mainMenu = menu;
    NSApp.windowsMenu = windows;
    NSMenuItem *helpItem = [[NSMenuItem alloc] initWithTitle:[self text:@"Aiuto" english:@"Help"] action:nil keyEquivalent:@""];
    NSMenu *help = [[NSMenu alloc] initWithTitle:helpItem.title];
    helpItem.submenu = help;
    [menu addItem:helpItem];
    [self addWorkspaceCommand:@"guide" title:@"Guida essenziale" english:@"Essential guide" key:@""
                   modifiers:0 menu:help];
    NSApp.helpMenu = help;
}
- (void)applicationDidFinishLaunching:(NSNotification *)notification {
    (void)notification;
    [NSApp addObserver:self forKeyPath:@"effectiveAppearance" options:0 context:&appearance_context];
    self.observingAppearance = YES;
    dispatch_queue_attr_t attributes = dispatch_queue_attr_make_with_autorelease_frequency(
        DISPATCH_QUEUE_SERIAL, DISPATCH_AUTORELEASE_FREQUENCY_WORK_ITEM);
    owner = dispatch_queue_create("com.overtone.desktop.owner", attributes);
    NSFileManager *files = NSFileManager.defaultManager;
    NSError *failure = nil;
    BOOL made = [files createDirectoryAtPath:self.profile
                 withIntermediateDirectories:YES
                                  attributes:@{NSFilePosixPermissions : @0700}
                                       error:&failure];
    struct stat st;
    BOOL directory = made && !lstat(self.profile.fileSystemRepresentation, &st) && S_ISDIR(st.st_mode) &&
                     st.st_uid == getuid() && !(st.st_mode & 0077);
    self.locationLock =
        directory
            ? open([[self.profile stringByAppendingPathComponent:@"location.lock"] fileSystemRepresentation],
                   O_CREAT | O_RDWR | O_NOFOLLOW, 0600)
            : -1;
    if (self.locationLock < 0 || flock(self.locationLock, LOCK_EX | LOCK_NB) ||
        !ot_location_open(&location, self.profile.fileSystemRepresentation)) {
        [self fail:[self text:@"Profilo non disponibile o già aperto."
                       english:@"Profile unavailable or already open."]];
        return;
    }
    self.profile = @(location.active);
    if (lstat(self.profile.fileSystemRepresentation, &st) || !S_ISDIR(st.st_mode) || st.st_uid != getuid() ||
        (st.st_mode & 0077)) {
        [self fail:[self text:@"La cartella dati non è privata o non è disponibile."
                       english:@"The data folder is not private or is unavailable."]];
        return;
    }
    self.lock = open([[self.profile stringByAppendingPathComponent:@"profile.lock"] fileSystemRepresentation],
                     O_CREAT | O_RDWR | O_NOFOLLOW, 0600);
    if (self.lock < 0 || flock(self.lock, LOCK_EX | LOCK_NB)) {
        [self fail:[self text:@"Profilo non disponibile o già aperto."
                       english:@"Profile unavailable or already open."]];
        return;
    }
    atomic_init(&locationCancel, false);
    if (!*location.pending) {
        [self openWorkspace];
        return;
    }
    self.locationBusy = YES;
    self.opening = YES;
    [self createWorkspaceWithTheme:@"system" accent:@"teal"];
    NSView *content = self.window.contentView;
    NSView *transfer = [[NSView alloc] initWithFrame:NSMakeRect((content.bounds.size.width-392)/2,24,392,110)];
    transfer.autoresizingMask = NSViewMinXMargin | NSViewMaxXMargin | NSViewMaxYMargin;
    NSTextField *label = [NSTextField labelWithString:[self text:@"Trasferimento dei dati"
                                                          english:@"Transferring data"]];
    label.frame = NSMakeRect(0,78,392,24);
    NSProgressIndicator *progress = [[NSProgressIndicator alloc] initWithFrame:NSMakeRect(0,52,392,16)];
    progress.indeterminate = YES;
    [progress startAnimation:nil];
    NSButton *cancelTransfer = [NSButton buttonWithTitle:[self text:@"Annulla" english:@"Cancel"]
                                                  target:self action:@selector(cancelLocationTransfer:)];
    cancelTransfer.frame = NSMakeRect(300,0,92,32);
    [transfer addSubview:label]; [transfer addSubview:progress]; [transfer addSubview:cancelTransfer];
    [content addSubview:transfer positioned:NSWindowAbove relativeTo:self.loading ?: self.web];
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
      TransferPresentation presentation = {
          .label = label, .bar = progress, .cancel = cancelTransfer,
          .finishing = [self text:@"Completamento" english:@"Finalizing"],
          .verifying = [self text:@"Verifica dei dati" english:@"Verifying data"],
          .copying = [self text:@"Copia dei dati" english:@"Copying data"]};
      ot_location_apply(&self->location, &self->locationCancel, transferProgress, &presentation);
      if (!strcmp(self->location.error, "cancelled"))
          ot_location_cancel(&self->location);
      dispatch_async(dispatch_get_main_queue(), ^{
        self.locationBusy = NO;
        self.loading.reservedHeight=0;self.loading.needsLayout=YES;
        [transfer removeFromSuperview];
        close(self.lock);
        self.profile = @(self->location.active);
        self.lock =
            open([[self.profile stringByAppendingPathComponent:@"profile.lock"] fileSystemRepresentation],
                 O_CREAT | O_RDWR | O_NOFOLLOW, 0600);
        if (self.lock < 0 || flock(self.lock, LOCK_EX | LOCK_NB)) {
            [self fail:[self text:@"Profilo non disponibile o già aperto."
                           english:@"Profile unavailable or already open."]];
            return;
        }
        [self openWorkspace];
      });
    });
}
- (void)cancelLocationTransfer:(id)sender {
    (void)sender;
    atomic_store(&locationCancel, true);
}
- (void)createWorkspaceWithTheme:(NSString *)theme accent:(NSString *)accent {
    self.loading=[[OTLoadingView alloc] initWithFrame:NSZeroRect resource:
        [NSBundle.mainBundle.resourceURL URLByAppendingPathComponent:@"ui/loading.svg"]];
    WKWebViewConfiguration *configuration = private_renderer();
    [configuration.userContentController addScriptMessageHandler:self name:@"overtone"];
    NSData *appearanceData = [NSJSONSerialization dataWithJSONObject:@{
        @"theme" : theme,
        @"accent" : accent,
        @"chrome" : @"mac",
        @"systemTheme" : system_theme(),
        @"device" : @"Mac",
        @"transfer" : self.locationBusy ? @"true" : @"false",
        @"nativeLoading" : self.loading ? @"true" : @"false"
    }
                                                             options:0
                                                               error:NULL];
    NSString *appearance = [[NSString alloc] initWithData:appearanceData encoding:NSUTF8StringEncoding];
    NSData *regionData = [NSJSONSerialization dataWithJSONObject:regional_environment() options:0 error:NULL];
    NSString *region = [[NSString alloc] initWithData:regionData encoding:NSUTF8StringEncoding];
    NSData *localeData = [NSJSONSerialization dataWithJSONObject:
        @{@"language":self.language, @"strings":self.translations} options:0 error:NULL];
    NSString *locale = [[NSString alloc] initWithData:localeData encoding:NSUTF8StringEncoding];
    [configuration.userContentController addUserScript:[[WKUserScript alloc]
        initWithSource:[@"window.overtoneLocale=" stringByAppendingString:locale]
        injectionTime:WKUserScriptInjectionTimeAtDocumentStart forMainFrameOnly:YES]];
    NSString *bootstrap =
        [NSString stringWithFormat:
                      @"Object.assign(document.documentElement.dataset,%@);window.updateChromeAppearance?.();"
                       "window.overtoneRegion=%@",
                      appearance, region];
    [configuration.userContentController
        addUserScript:[[WKUserScript alloc] initWithSource:bootstrap
                                             injectionTime:WKUserScriptInjectionTimeAtDocumentEnd
                                          forMainFrameOnly:YES]];
    self.web = [[WorkspaceView alloc] initWithFrame:NSZeroRect configuration:configuration];
    self.web.navigationDelegate = self;
    self.web.UIDelegate = self;
    self.uiStatus = @"loading";
    NSRect available = NSScreen.mainScreen.visibleFrame;
    NSSize initial = NSMakeSize(MIN(1280, available.size.width * .9), MIN(800, available.size.height * .85));
    WorkspaceWindow *window = [[WorkspaceWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, initial.width, initial.height)
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable |
                            NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskFullSizeContentView
                    backing:NSBackingStoreBuffered
                      defer:NO];
    self.window = window;
    window.collectionBehavior = NSWindowCollectionBehaviorFullScreenPrimary;
    __weak Desktop *weakDesktop = self;
    window.prepareFullScreen = ^(BOOL fullScreen, dispatch_block_t finish) {
      Desktop *desktop = weakDesktop;
      desktop.chromeTransition = fullScreen ? 1 : -1;
      [desktop setWindowControlsHidden:YES];
      if (fullScreen)
          desktop.window.toolbar.visible = NO;
      [desktop layoutChrome];
      [desktop.web callAsyncJavaScript:@"await window.overtonePrepareFullScreen?.(fullScreen)"
                            arguments:@{@"fullScreen" : @(fullScreen)}
                              inFrame:nil
                       inContentWorld:WKContentWorld.pageWorld
                    completionHandler:^(id value, NSError *error) {
                      (void)value;
                      (void)error;
                      if (!fullScreen) {
                          [desktop setWindowControlsHidden:NO];
                          desktop.window.toolbar.visible = YES;
                      }
                      finish();
                    }];
    };
    self.window.title = @"OverTone";
    self.window.titleVisibility = NSWindowTitleHidden;
    self.window.titlebarAppearsTransparent = YES;
    self.window.titlebarSeparatorStyle = NSTitlebarSeparatorStyleNone;
    self.window.toolbar = [[NSToolbar alloc] initWithIdentifier:@"OverTone"];
    self.window.toolbarStyle = NSWindowToolbarStyleUnified;
    self.window.movableByWindowBackground = YES;
    self.window.delegate = self;
    [self setTheme:theme];
    self.window.contentMinSize = NSMakeSize(640, 480);
    [self.window center];
    [self.window setFrameAutosaveName:@"OverTone"];
    NSView *content = [NSView new];
    self.window.contentView = content;
    self.web.frame = content.bounds;
    self.web.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    [content addSubview:self.web];
    if (self.loading) {
        self.loading.frame=content.bounds;
        self.loading.reservedHeight=self.locationBusy ? 320 : 0;
        self.loading.autoresizingMask=NSViewWidthSizable | NSViewHeightSizable;
        self.loading.backgroundColor=self.window.backgroundColor;
        [content addSubview:self.loading positioned:NSWindowAbove relativeTo:self.web];
        [self.loading prepare];
    }
    self.chrome = [ChromeBackground new];
    self.chrome.hidden = YES;
    self.chrome.autoresizingMask = NSViewWidthSizable | NSViewMinYMargin;
    [content addSubview:self.chrome positioned:NSWindowAbove relativeTo:self.web];
    [self.window makeKeyAndOrderFront:nil];
    if (@available(macOS 14.0, *))
        [NSApp activate];
    else
        [NSApp activateIgnoringOtherApps:YES];
    NSURL *ui = [[NSBundle.mainBundle resourceURL] URLByAppendingPathComponent:@"ui"];
    [self.web loadFileURL:[ui URLByAppendingPathComponent:@"index.html"] allowingReadAccessToURL:ui];
}
- (void)openWorkspace {
    self.opening = YES;
    if (!self.window) [self createWorkspaceWithTheme:@"system" accent:@"teal"];
    NSURLSessionConfiguration *network = OTNetworkConfiguration(YES);
    network.timeoutIntervalForRequest = 60;
    network.timeoutIntervalForResource = 86400;
    NSOperationQueue *callbacks = [NSOperationQueue new];
    callbacks.maxConcurrentOperationCount = 1;
    callbacks.underlyingQueue = owner;
    self.networkCallbacks = callbacks;
    self.localSession = [NSURLSession sessionWithConfiguration:network delegate:self delegateQueue:callbacks];
    self.libraryTasks = [NSMutableDictionary new];
    /* Data I/O remains on its serial owner while the real window displays its
     * bundled loading surface. Readiness replaces content, never the window. */
    dispatch_async(owner, ^{
      BOOL restored = ot_store_apply_restore(self.profile.fileSystemRepresentation);
      if (restored)
          self->core = ot_open(
              [[self.profile stringByAppendingPathComponent:@"workspace.sqlite"] fileSystemRepresentation],
              (ot_io){(__bridge void *)self, request, cancel, prompt_language});
      NSString *failure =
          !restored ? [self text:@"Ripristino non riuscito. Il backup precedente è conservato nel profilo."
                          english:@"Restore failed. The previous backup is kept in the profile."]
          : !self->core ? [self text:@"Impossibile aprire i dati. Verifica l’accesso alla cartella o riprova."
                              english:@"Could not open data. Check folder access or try again."]
                        : nil;
      NSString *uiLanguage = @"it";
      NSString *theme = nil, *accent = nil;
      if (self->core) {
          uiLanguage = @(ot_get_preferences(self->core)->language);
          theme = @(ot_get_preferences(self->core)->theme);
          accent = @(ot_get_preferences(self->core)->accent);
          ot_resources r = ot_resources_sample(self.profile.fileSystemRepresentation);
          ot_resources_update(self->core, &r);
          self->library = ot_library_open(
              self->core, self.profile.fileSystemRepresentation,
              (ot_library_io){.context=(__bridge void *)self,.request=library_request,
                              .cancel=library_cancel,.connected=library_connected,.components=self->location.base});
          if (!self->library)
              failure = [self text:@"Impossibile aprire la libreria modelli."
                           english:@"Could not open the model library."];
      }
      dispatch_async(dispatch_get_main_queue(), ^{
        self.opening = NO;
        if (self.servicesStopped)
            return;
        if (failure) {
            [self fail:failure];
            return;
        }
        [self applyLanguage:uiLanguage];
        [self showWorkspaceWithTheme:theme accent:accent];
      });
    });
}
- (void)showWorkspaceWithTheme:(NSString *)theme accent:(NSString *)accent {
    (void)accent;
    [self setTheme:theme];
    [self.web callAsyncJavaScript:@"window.overtoneSetLocale?.(code,strings)"
                        arguments:@{@"code":self.language,@"strings":self.translations}
                          inFrame:nil inContentWorld:WKContentWorld.pageWorld completionHandler:nil];
    self.connectionManager = [OTConnection new];
    __weak Desktop *connectionOwner = self;
    self.connectionManager.allowsURL = ^BOOL(NSURL *url) {
        Desktop *d = connectionOwner;
        if (!d) return NO;
        __block BOOL allowed = NO;
        dispatch_sync(d->owner, ^{ allowed = network_admit(d, url, OT_NETWORK_SERVERS); });
        return allowed;
    };
    self.connectionManager.update = ^(NSDictionary *state) {
        Desktop *d = connectionOwner;
        if (!d || d.terminating) return;
        if ([state[@"restore"] boolValue]) {
            d.connectionRestoring = NO;
            dispatch_async(d->owner, ^{
                const ot_connection *c = ot_configuration(d->core);
                if (!ot_library_manages_model(d->library) && [state[@"model"] isEqual:@(c->model)] &&
                    [OTServerBase(@(c->endpoint),[@(c->endpoint) hasPrefix:@"https:"]) isEqual:state[@"server"]])
                    ot_connection_failed(d->core,[state[@"error"] UTF8String]);
                dispatch_async(dispatch_get_main_queue(), ^{ [d requestRender]; });
            });
            return;
        }
        [d.web callAsyncJavaScript:@"window.overtoneConnection?.(state)" arguments:@{@"state":state}
            inFrame:nil inContentWorld:WKContentWorld.pageWorld completionHandler:nil];
    };
    self.connectionManager.apply = ^(ot_connection connection, ot_server_info info, NSString *key) {
        Desktop *d = connectionOwner;
        if (!d || d.terminating) return;
        BOOL restoring = d.connectionRestoring;
        d.connectionRestoring = NO;
        dispatch_async(d->owner, ^{
            if (!network_allowed(d, destination(@(connection.endpoint),connection.ollama ? @"ollama" : @"openai",connection.remote), OT_NETWORK_SERVERS)) {
                ot_connection_failed(d->core,"internet_disabled");
                dispatch_async(dispatch_get_main_queue(), ^{ [d requestRender]; });
                return;
            }
            const ot_connection *previous = ot_configuration(d->core);
            BOOL same = previous->ollama == connection.ollama &&
                [destination(@(previous->endpoint),previous->ollama ? @"ollama" : @"openai",previous->remote)
                    isEqual:destination(@(connection.endpoint),connection.ollama ? @"ollama" : @"openai",connection.remote)] &&
                !strcmp(previous->model, connection.model);
            if (restoring && (!same || ot_library_manages_model(d->library))) return;
            BOOL alreadyManaged = same && ot_ready(d->core) && ot_library_manages_model(d->library);
            if (alreadyManaged && !restoring) {
                dispatch_async(dispatch_get_main_queue(), ^{
                    [d.web callAsyncJavaScript:@"window.overtoneConfigured()" arguments:@{} inFrame:nil
                        inContentWorld:WKContentWorld.pageWorld completionHandler:nil];
                });
                return;
            }
            if (restoring || ot_configure(d->core, &connection, 0)) {
                if (!restoring) ot_library_manual(d->library);
                d.sessionCredential = @{@"server":OTServerBase(@(connection.endpoint),[@(connection.endpoint) hasPrefix:@"https:"]),@"key":key};
                ot_connection_observe(d->core, &info);
                ot_library_server_changed(d->library);
                dispatch_async(dispatch_get_main_queue(), ^{ [d requestRender]; });
                if (!restoring) dispatch_async(dispatch_get_main_queue(), ^{
                    [d.web callAsyncJavaScript:@"window.overtoneConfigured()" arguments:@{} inFrame:nil
                        inContentWorld:WKContentWorld.pageWorld completionHandler:nil];
                });
            } else if (!restoring) {
                dispatch_async(dispatch_get_main_queue(), ^{
                    [d.web callAsyncJavaScript:@"window.overtoneConnectionCommitFailed?.()" arguments:@{}
                        inFrame:nil inContentWorld:WKContentWorld.pageWorld completionHandler:nil];
                });
            }
        });
    };
    self.fileWork = dispatch_group_create();
    self.diagnostics =
        ot_diagnostics_open(self.profile.fileSystemRepresentation, local_request, (__bridge void *)self);
    if (!self.diagnostics) {
        [self fail:[self text:@"Impossibile avviare la diagnostica locale."
                       english:@"Could not start local diagnostics."]];
        return;
    }
    self.timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
    self.renderInterval = 100;
    dispatch_source_set_timer(self.timer, dispatch_time(DISPATCH_TIME_NOW, 0), 100 * NSEC_PER_MSEC,
                              20 * NSEC_PER_MSEC);
    __weak Desktop *weak = self;
    dispatch_source_set_event_handler(self.timer, ^{
      [weak render];
    });
    dispatch_resume(self.timer);
    [self refreshConnection];
}
- (void)fail:(NSString *)message {
    NSAlert *alert = [NSAlert new];
    alert.window.appearance = self.window.effectiveAppearance;
    alert.messageText = @"OverTone";
    alert.informativeText = message;
    [alert runModal];
    [NSApp terminate:nil];
}
- (void)requestRender {
    self.renderRequested = YES;
    [self render];
}
- (void)finishRender {
    self.rendering = NO;
    if (self.renderRequested)
        [self render];
}
- (void)render {
    if (self.servicesStopped || self.opening || !self->core || !self->library || self.rendering || self.web.loading)
        return;
    self.rendering = YES;
    self.renderRequested = NO;
    uint64_t epoch = self.rendererEpoch;
    uint64_t previous = self.shownRevision;
    uint64_t previousStream = self.shownStreamRevision;
    dispatch_async(owner, ^{
      if (epoch != self.rendererEpoch)
          return;
      ot_library_tick(self->library);
      uint64_t memoryRevision = ot_library_memory_revision(self->library);
      if (memoryRevision != self.shownMemoryRevision && !self.memoryRendering) {
          char *memory = ot_library_memory(self->library);
          NSString *memoryPayload = memory ? @(memory) : nil;
          free(memory);
          if (memoryPayload) {
              self.memoryRendering = YES;
              dispatch_async(dispatch_get_main_queue(), ^{
                if (epoch != self.rendererEpoch) return;
                [self.web callAsyncJavaScript:@"window.overtoneMemory?.(JSON.parse(payload))"
                                    arguments:@{@"payload" : memoryPayload}
                                      inFrame:nil
                               inContentWorld:WKContentWorld.pageWorld
                            completionHandler:^(id value, NSError *error) {
                              (void)value;
                              if (epoch != self.rendererEpoch) return;
                              if (!error)
                                  self.shownMemoryRevision = memoryRevision;
                              self.memoryRendering = NO;
                            }];
              });
          }
      }
      uint64_t libraryRevision = ot_library_revision(self->library);
      if (libraryRevision != self.shownLibraryRevision) {
          char *raw = ot_library_view(self->library);
          NSString *payload = raw ? @(raw) : nil;
          free(raw);
          if (payload)
              dispatch_async(dispatch_get_main_queue(), ^{
                if (epoch != self.rendererEpoch) return;
                [self.web callAsyncJavaScript:@"window.overtoneLibrary(JSON.parse(payload))"
                                    arguments:@{@"payload" : payload}
                                      inFrame:nil
                               inContentWorld:WKContentWorld.pageWorld
                            completionHandler:^(id value, NSError *error) {
                              (void)value;
                              if (epoch != self.rendererEpoch) return;
                              if (!error)
                                  self.shownLibraryRevision = libraryRevision;
                            }];
              });
      }
      ot_background(self->core);
      NSTimeInterval time = NSProcessInfo.processInfo.systemUptime;
      if (time - self.resourceSampleTime >= 2) {
          self.resourceSampleTime = time;
          ot_resources sample = ot_resources_sample(self.profile.fileSystemRepresentation);
          ot_resources_update(self->core, &sample);
          char *raw = ot_resources_view(self->core);
          NSString *resources = raw ? @(raw) : @"{}";
          free(raw);
          dispatch_async(dispatch_get_main_queue(), ^{
            if (epoch != self.rendererEpoch) return;
            [self.web callAsyncJavaScript:@"window.overtoneResources?.(JSON.parse(payload))"
                                arguments:@{@"payload" : resources}
                                  inFrame:nil
                           inContentWorld:WKContentWorld.pageWorld
                        completionHandler:nil];
          });
      }
      uint64_t revision = ot_revision(self->core);
      uint64_t stream = ot_stream_revision(self->core);
      BOOL full = revision != previous;
      char *raw = full ? ot_view(self->core) : stream != previousStream ? ot_progress(self->core) : NULL;
      NSString *payload = raw ? [[NSString alloc] initWithUTF8String:raw] : nil;
      free(raw);
      uint64_t interval = !ot_is_idle(self->core) || ot_library_polling(self->library) ? 100 : 1000;
      dispatch_async(dispatch_get_main_queue(), ^{
        if (epoch != self.rendererEpoch) return;
        if (self.renderInterval != interval) {
            self.renderInterval = interval;
            dispatch_source_set_timer(self.timer, dispatch_time(DISPATCH_TIME_NOW, (int64_t)(interval * NSEC_PER_MSEC)),
                                      interval * NSEC_PER_MSEC, (interval / 5) * NSEC_PER_MSEC);
        }
        if (!payload) {
            if (full || stream != previousStream) {
                self.uiStatus = @"snapshot_failed";
                [self.web callAsyncJavaScript:
                              @"window.overtoneError('Impossibile leggere i dati della conversazione.', true)"
                                    arguments:@{}
                                      inFrame:nil
                               inContentWorld:WKContentWorld.pageWorld
                            completionHandler:nil];
            }
            [self finishRender];
            return;
        }
        [self.web callAsyncJavaScript:full ? @"window.overtoneReceive(JSON.parse(payload))"
                                           : @"window.overtoneProgress(JSON.parse(payload))"
                            arguments:@{@"payload" : payload}
                              inFrame:nil
                       inContentWorld:WKContentWorld.pageWorld
                    completionHandler:^(id value, NSError *error) {
                      (void)value;
                      if (epoch != self.rendererEpoch) return;
                      if (!error) {
                          self.shownRevision = revision;
                          self.shownStreamRevision = stream;
                          self.uiStatus = @"ready";
                          if (full && self.showSetup) {
                              self.showSetup = NO;
                              [self.web callAsyncJavaScript:@"navigate('models');window.openConfiguration()"
                                  arguments:@{} inFrame:nil inContentWorld:WKContentWorld.pageWorld
                                  completionHandler:nil];
                          }
                      } else
                          self.uiStatus = @"render_error";
                      [self finishRender];
                    }];
      });
    });
}
- (void)scheduleStreamRender {
    // Called on the owner: keep one scheduled frame even when the main thread is busy.
    if (self.streamRenderScheduled || self.terminating)
        return;
    self.streamRenderScheduled = YES;
    __weak Desktop *weak = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 33 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
      Desktop *strong = weak;
      if (!strong)
          return;
      [strong requestRender];
      dispatch_async(strong->owner, ^{
        strong.streamRenderScheduled = NO;
      });
    });
}
- (void)userContentController:(WKUserContentController *)controller
      didReceiveScriptMessage:(WKScriptMessage *)message {
    (void)controller;
    if (!message.frameInfo.mainFrame || ![message.frameInfo.request.URL.scheme isEqual:@"file"] ||
        ![message.body isKindOfClass:NSString.class])
        return;
    NSData *data = [message.body dataUsingEncoding:NSUTF8StringEncoding];
    if (data.length > 1024 * 1024)
        return;
    NSDictionary *input = [NSJSONSerialization JSONObjectWithData:data options:0 error:NULL];
    if (![input isKindOfClass:NSDictionary.class])
        return;
    NSString * (^string)(NSString *) = ^NSString *(NSString *key) {
      return [input[key] isKindOfClass:NSString.class] ? input[key] : @"";
    };
    NSString *command = string(@"command");
    if ([command isEqual:@"startup"]) {
        NSString *mode=string(@"mode");
        if ([mode isEqual:@"finish"]) {
            double duration=[input[@"duration"] isKindOfClass:NSNumber.class] ? [input[@"duration"] doubleValue] : 0;
            if (isfinite(duration) && duration>=0 && duration<=600) [self.loading finishWithDuration:duration/1000];
        } else if ([mode isEqual:@"play"]) {
            [self.loading prepare];
        } else if ([mode isEqual:@"replay"]) {
            dispatch_async(self->owner, ^{
                if (!self->core || !ot_get_preferences(self->core)->developer) return;
                dispatch_async(dispatch_get_main_queue(), ^{[self.loading play];});
            });
        } else if ([mode isEqual:@"phase"]) {
            double value=[input[@"value"] isKindOfClass:NSNumber.class] ? [input[@"value"] doubleValue] : -1;
            if (isfinite(value) && value>=0 && value<=1) dispatch_async(self->owner, ^{
                if (!self->core || !ot_get_preferences(self->core)->developer) return;
                dispatch_async(dispatch_get_main_queue(), ^{[self.loading setProgress:value];});
            });
        }
        return;
    }
    if ([command isEqual:@"menuState"]) {
        NSArray *enabled = input[@"enabled"], *checked = input[@"checked"];
        if (![enabled isKindOfClass:NSArray.class] || ![checked isKindOfClass:NSArray.class] ||
            enabled.count > self.workspaceMenuItems.count || checked.count > self.workspaceMenuItems.count) return;
        for (id entry in [enabled arrayByAddingObjectsFromArray:checked])
            if (![entry isKindOfClass:NSString.class] || !self.workspaceMenuItems[entry]) return;
        self.enabledWorkspaceActions = [NSSet setWithArray:enabled];
        self.checkedWorkspaceActions = [NSSet setWithArray:checked];
        for (NSString *identifier in self.workspaceMenuItems) {
            NSMenuItem *item = self.workspaceMenuItems[identifier];
            item.enabled = [self validateMenuItem:item];
            item.state = [self.checkedWorkspaceActions containsObject:identifier] ? NSControlStateValueOn : NSControlStateValueOff;
        }
        return;
    }
    if (self.opening && ![@[@"appearance",@"chrome"] containsObject:command]) return;
    if ([command isEqual:@"rendererFault"]) {
        NSString *file = string(@"file");
        NSNumber *line = input[@"line"], *column = input[@"column"];
        NSURL *script = [[NSBundle.mainBundle.resourceURL URLByAppendingPathComponent:@"ui"]
            URLByAppendingPathComponent:file];
        if (file.length <= 64 && [file.lastPathComponent isEqual:file] && [file.pathExtension isEqual:@"js"] &&
            [NSFileManager.defaultManager fileExistsAtPath:script.path] &&
            [line isKindOfClass:NSNumber.class] && [column isKindOfClass:NSNumber.class] &&
            line.doubleValue >= 0 && line.doubleValue <= 100000 && column.doubleValue >= 0 &&
            column.doubleValue <= 100000)
            self.rendererFault = @{@"file" : file, @"line" : line, @"column" : column};
        return;
    }
    if ([command isEqual:@"readingMetrics"]) {
        NSDictionary *metrics = input[@"value"];
        if (![metrics isKindOfClass:NSDictionary.class] || metrics.count != 10)
            return;
        for (NSString *key in @[ @"turns", @"pending", @"loadMs", @"shiftPx", @"anchorLost", @"updateMs", @"following", @"tailGap", @"contentHeight", @"viewportHeight" ])
            if (![metrics[key] isKindOfClass:NSNumber.class] || !isfinite([metrics[key] doubleValue]))
                return;
        if ([metrics[@"turns"] doubleValue] < 0 || [metrics[@"turns"] doubleValue] > 96 ||
            [metrics[@"loadMs"] doubleValue] < 0 || [metrics[@"loadMs"] doubleValue] > 30000 ||
            [metrics[@"updateMs"] doubleValue] < 0 || [metrics[@"updateMs"] doubleValue] > 30000 ||
            fabs([metrics[@"shiftPx"] doubleValue]) > 100000 ||
            [metrics[@"tailGap"] doubleValue] < 0 || [metrics[@"tailGap"] doubleValue] > 100000000 ||
            [metrics[@"contentHeight"] doubleValue] < 0 || [metrics[@"contentHeight"] doubleValue] > 100000000 ||
            [metrics[@"viewportHeight"] doubleValue] < 0 || [metrics[@"viewportHeight"] doubleValue] > 100000)
            return;
        self.readingMetrics = [metrics copy];
        return;
    }
    if ([command isEqual:@"renderMetrics"]) {
        NSDictionary *metrics = input[@"value"];
        if (![metrics isKindOfClass:NSDictionary.class] || metrics.count != 8)
            return;
        for (NSString *key in @[@"blocks", @"coloured", @"pending", @"highlightMs", @"detailsVisible",
                                @"known", @"chars", @"mismatched"])
            if (![metrics[key] isKindOfClass:NSNumber.class] || !isfinite([metrics[key] doubleValue]) ||
                [metrics[key] doubleValue] < 0 || [metrics[key] doubleValue] > 100000)
                return;
        self.renderMetrics = [metrics copy];
        return;
    }
    if ([command isEqual:@"motionMetrics"]) {
        NSDictionary *metrics = input[@"value"];
        if (![metrics isKindOfClass:NSDictionary.class] || metrics.count != 23 ||
            ![@[@"details", @"surface", @"chrome", @"selection", @"dialog", @"filters", @"reasoning",
                @"variants", @"route", @"transfer", @"discovery", @"startup", @"indicator", @"popover", @"drawer", @"scroll", @"suggestion"] containsObject:metrics[@"name"]])
            return;
        if (![metrics[@"startedAt"] isKindOfClass:NSNumber.class] ||
            !isfinite([metrics[@"startedAt"] doubleValue]) ||
            [metrics[@"startedAt"] doubleValue] <= 0 || [metrics[@"startedAt"] doubleValue] > 1e12)
            return;
        for (NSString *key in @[@"frames", @"medianMs", @"p95Ms", @"p99Ms", @"maximumMs", @"maximumAtMs", @"arrivalMaxMs",
                               @"lagMaxMs", @"lateCallbacks", @"callbackDebtMs", @"elapsedMs", @"interrupted", @"inactive",
                               @"setupMs", @"firstFrameMs", @"readyMs", @"chromeMs", @"readingMs", @"highlightMs",
                               @"navigationMs", @"updateMs"])
            if (![metrics[key] isKindOfClass:NSNumber.class] || !isfinite([metrics[key] doubleValue]) ||
                [metrics[key] doubleValue] < 0 || [metrics[key] doubleValue] > 30000)
                return;
        NSMutableDictionary *samples = [self.motionMetrics mutableCopy] ?: [NSMutableDictionary new];
        samples[metrics[@"name"]] = [metrics copy];
        self.motionMetrics = [samples copy];
        return;
    }
    if ([command isEqual:@"appearance"]) {
        NSArray *raw = input[@"background"];
        if (![raw isKindOfClass:NSArray.class] || raw.count != 3)
            return;
        double rgb[3];
        for (unsigned i = 0; i < 3; i++)
            if (![raw[i] isKindOfClass:NSNumber.class] || !isfinite(rgb[i] = [raw[i] doubleValue]) ||
                rgb[i] < 0 || rgb[i] > 1)
                return;
        self.window.backgroundColor = [NSColor colorWithSRGBRed:rgb[0] green:rgb[1] blue:rgb[2] alpha:1];
        self.loading.backgroundColor=self.window.backgroundColor;
        return;
    }
    if ([command isEqual:@"chrome"]) {
        NSArray *rects = [input[@"rects"] isKindOfClass:NSArray.class] ? input[@"rects"] : nil;
        double viewport = [input[@"width"] isKindOfClass:NSNumber.class] ? [input[@"width"] doubleValue] : 0;
        if (!rects.count || rects.count > 64 || !isfinite(viewport) || viewport < 100 || viewport > 100000)
            return;
        if (fabs(self.web.bounds.size.width - viewport) > 1) {
            self.chrome.hidden = YES;
            return;
        }
        NSMutableArray *controls = [NSMutableArray new];
        for (id raw in rects) {
            if (![raw isKindOfClass:NSArray.class] || [raw count] != 4)
                return;
            double v[4];
            for (unsigned i = 0; i < 4; i++) {
                if (![raw[i] isKindOfClass:NSNumber.class] || !isfinite(v[i] = [raw[i] doubleValue]))
                    return;
            }
            if (fabs(v[0]) > 100000 || fabs(v[1]) > 100000 || v[2] < 0 || v[3] < 0 || v[2] > 100000 ||
                v[3] > 100000)
                return;
            NSRect r = NSMakeRect(v[0], v[1], v[2], v[3]);
            [controls addObject:[NSValue valueWithRect:NSInsetRect(r, -2, -2)]];
        }
        self.chrome.controls = controls;
        self.chrome.hidden = !self.chrome.draggingAllowed;
        [self.window invalidateCursorRectsForView:self.chrome];
        NSMutableDictionary *layout = [self.windowLayout mutableCopy];
        layout[@"viewportWidth"] = @(viewport);
        id viewportHeight = input[@"height"];
        if ([viewportHeight isKindOfClass:NSNumber.class] && isfinite([viewportHeight doubleValue]) &&
            [viewportHeight doubleValue] > 0 && [viewportHeight doubleValue] < 100000)
            layout[@"viewportHeight"] = viewportHeight;
        NSDictionary *metrics = input[@"metrics"];
        NSArray<NSString *> *metricNames = @[
                     @"rendererFocused",
                     @"rendererWindowActive",
                     @"rendererHidden",
                     @"rendererMotionAllowed",
                     @"rendererFrameRateAPI",
                     @"rendererMaximumFrameRate",
                     @"rendererHighRateCSS",
                     @"rendererOverlayScrollbars",
                     @"rendererAnimations",
                     @"contextScrimOpen",
                     @"contextScrimVisible",
                     @"contextScrimOpacity",
                     @"historyOpen",
                     @"detailsOpen",
                     @"modelsInert",
                     @"topbarHeight",
                     @"brandTop",
                     @"brandBottom",
                     @"brandLeft",
                     @"brandRight",
                     @"logoLeft",
                     @"logoWidth",
                     @"historyCardLeft",
                     @"historySearchLeft",
                     @"navTop",
                     @"navBottom",
                     @"navLeft",
                     @"navRight",
                     @"chatHeaderBottom",
                     @"historyHeaderBottom",
                     @"detailsHeaderBottom",
                     @"modelsHeaderBottom",
                     @"modelsRailHeaderBottom",
                     @"preparedHeaderBottom",
                     @"preparedRailHeaderBottom",
                     @"downloadIconOffsetX",
                     @"downloadIconOffsetY",
                     @"downloadRingOffsetX",
                     @"downloadRingOffsetY",
                     @"detailsWidth",
                     @"detailsLeft",
                     @"historyLeft",
                     @"composerWidth",
                     @"historyWidth",
                     @"chatLeft",
                     @"chatWidth",
                     @"responseWidth",
                     @"codeWidth",
                     @"transcriptOverflowX",
                     @"modelsWorkspaceWidth",
                     @"modelsSurfaceWidth",
                     @"modelsSearchWidth",
                     @"toggle-detailsCenterX",
                     @"preferencesCenterX",
                     @"trash-chatCenterX",
                     @"archive-chatCenterX",
                     @"newCenterX",
                     @"chat-filters-toggleCenterX",
                     @"topActionsRight"
                 ];
        if ([metrics isKindOfClass:NSDictionary.class] && metrics.count <= metricNames.count) {
            NSMutableDictionary *measured = [NSMutableDictionary new];
            for (NSString *name in metricNames) {
                id value = metrics[name];
                if ([value isKindOfClass:NSNumber.class] && isfinite([value doubleValue]) &&
                    fabs([value doubleValue]) < 100000)
                    measured[name] = value;
            }
            layout[@"measured"] = measured;
        }
        self.windowLayout = layout;
        return;
    }
    if ([command isEqual:@"copy"]) {
        [NSPasteboard.generalPasteboard clearContents];
        [NSPasteboard.generalPasteboard setString:string(@"text") forType:NSPasteboardTypeString];
        return;
    }
    // Reject embedded NULs before passing NSString text to C string APIs.
    for (id value in input.allValues) {
        if ([value isKindOfClass:NSString.class] &&
            strlen([value UTF8String]) != [value lengthOfBytesUsingEncoding:NSUTF8StringEncoding])
            return;
    }
    int64_t (^number)(NSString *) = ^int64_t(NSString *key) {
      return [input[key] isKindOfClass:NSNumber.class] ? [input[key] longLongValue] : 0;
    };
    /* Queued after this handler: every C command is already enqueued before the
     * projection. Coalesce one trailing render if an older snapshot is in flight. */
    dispatch_async(dispatch_get_main_queue(), ^{ [self requestRender]; });
    if ([command isEqual:@"openLink"]) {
        NSURL *url = [NSURL URLWithString:string(@"url")];
        __block BOOL allowed = NO;
        dispatch_sync(owner, ^{ allowed = network_allowed(self, url, OT_NETWORK_CATALOGUES); });
        if (allowed && libraryURL(url)) [NSWorkspace.sharedWorkspace openURL:url];
        else [self.web callAsyncJavaScript:@"window.overtoneError?.(tr('Accesso a Internet disattivato.'))"
            arguments:@{} inFrame:nil inContentWorld:WKContentWorld.pageWorld completionHandler:nil];
        return;
    }
    if ([command isEqual:@"location"]) {
        [self locationAction:string(@"action")];
        return;
    }
    if ([command isEqual:@"notices"]) {
        NSMutableString *notices = [NSMutableString new];
        for (NSString *name in @[ @"THIRD_PARTY_NOTICES.md", @"yyjson-LICENSE", @"LICENSE" ]) {
            NSURL *url = [NSBundle.mainBundle.resourceURL URLByAppendingPathComponent:name];
            NSString *text = [NSString stringWithContentsOfURL:url encoding:NSUTF8StringEncoding error:nil];
            if (text.length && text.length <= 65536) {
                if ([name isEqual:@"yyjson-LICENSE"]) [notices appendString:@"## yyjson · MIT\n\n"];
                else if ([name isEqual:@"LICENSE"]) [notices appendString:@"## OverTone · Apache-2.0\n\n"];
                [notices appendFormat:@"%@\n\n", text];
            }
        }
        if (!notices.length)
            [notices appendString:[self text:@"Licenze non disponibili." english:@"Licenses unavailable."]];
        [self.web callAsyncJavaScript:@"window.overtoneNotices?.(text)"
                            arguments:@{@"text":notices}
                              inFrame:nil
                       inContentWorld:WKContentWorld.pageWorld
                    completionHandler:nil];
        return;
    }
    if ([command isEqual:@"diagnostics"]) {
        dispatch_async(owner, ^{
          if (!ot_get_preferences(self->core)->developer) return;
          char *raw = snapshot_on_owner(self);
          NSString *payload = raw ? @(raw) : @"{}";
          free(raw);
          dispatch_async(dispatch_get_main_queue(), ^{
            [self.web callAsyncJavaScript:@"window.overtoneDiagnostic?.(payload)"
                                arguments:@{@"payload" : payload}
                                  inFrame:nil
                           inContentWorld:WKContentWorld.pageWorld
                        completionHandler:nil];
          });
        });
        return;
    }
    if ([command isEqual:@"library"]) {
        if ([string(@"action") isEqual:@"engineNotices"]) {
            int kind = (int)number(@"kind");
            dispatch_async(owner, ^{
                const ot_package *package = ot_package_for_kind(kind);
                char *json = ot_package_notices(package, self->location.base);
                NSString *payload = json ? @(json) : @"[]";
                free(json);
                NSString *name = package ? @(package->name) : @"";
                dispatch_async(dispatch_get_main_queue(), ^{
                    if (self.terminating) return;
                    [self.web callAsyncJavaScript:@"window.overtoneEngineNotices?.(name,payload)"
                                        arguments:@{@"name":name,@"payload":payload}
                                          inFrame:nil inContentWorld:WKContentWorld.pageWorld completionHandler:nil];
                });
            });
            return;
        }
        if ([@[ @"pickModel", @"pickEngine" ] containsObject:string(@"action")]) {
            BOOL model = [string(@"action") isEqual:@"pickModel"];
            NSNumber *kind = @(number(@"kind"));
            NSOpenPanel *panel = [NSOpenPanel openPanel];
            panel.appearance = self.window.effectiveAppearance;
            panel.canChooseFiles = YES;
            panel.canChooseDirectories = NO;
            panel.allowsMultipleSelection = NO;
            panel.title = model ? [self text:@"Importa modello GGUF" english:@"Import GGUF model"]
                                : [self text:@"Scegli eseguibile del motore"
                                      english:@"Choose engine executable"];
            panel.prompt = [self text:@"Scegli" english:@"Choose"];
            [panel beginSheetModalForWindow:self.window
                          completionHandler:^(NSModalResponse response) {
                            if (response != NSModalResponseOK)
                                return;
                            NSInteger engineKind = kind.integerValue;
                            if (!model && !engineKind) {
                                NSString *base = panel.URL.lastPathComponent.lowercaseString;
                                engineKind = [base isEqual:@"ollama"]                ? 2
                                             : [base containsString:@"llama-server"] ? 1
                                                                                     : 0;
                            }
                            NSDictionary *value = @{
                                @"action" : model ? @"previewImport" : @"engine",
                                @"path" : panel.URL.path,
                                @"kind" : @(engineKind)
                            };
                            NSString *json =
                                [[NSString alloc] initWithData:[NSJSONSerialization dataWithJSONObject:value
                                                                                               options:0
                                                                                                 error:NULL]
                                                      encoding:NSUTF8StringEncoding];
                            dispatch_async(self->owner, ^{
                              ot_library_command(self->library, json.UTF8String);
                              [self scheduleStreamRender];
                            });
                          }];
        } else {
            NSString *json = (NSString *)message.body;
            dispatch_async(owner, ^{
              ot_library_command(self->library, json.UTF8String);
            });
        }
        return;
    }
    if ([@[ @"backup", @"export", @"restore" ] containsObject:command]) {
        [self fileOperation:command];
        return;
    }
    if ([command isEqual:@"preferences"]) {
        NSDictionary *preferences =
            [input[@"value"] isKindOfClass:NSDictionary.class] ? input[@"value"] : @{};
        dispatch_async(owner, ^{
          ot_preferences previous = *ot_get_preferences(self->core);
          /* Library-owned state must survive delayed settings projections:
           * setup activation/completion and recommendation read acknowledgement. */
          NSMutableDictionary *values = [preferences mutableCopy];
          values[@"setup"] = @(previous.setup);
          values[@"purpose"] = @(previous.purpose);
          values[@"setupEngines"] = @(previous.setup_engines);
          values[@"adviceSizeGB"] = @(previous.advice_size_gb);
          values[@"setupTarget"] = @(previous.setup_target);
          values[@"setupTargetEngines"] = @(previous.setup_target_engines);
          values[@"setupPaused"] = @(previous.setup_paused);
          values[@"adviceSeen"] = @(previous.advice_seen);
          NSData *encoded = [NSJSONSerialization dataWithJSONObject:values options:0 error:NULL];
          NSString *json = [[NSString alloc] initWithData:encoded encoding:NSUTF8StringEncoding];
          yyjson_doc *validation = yyjson_read(json.UTF8String, strlen(json.UTF8String), 0);
          ot_preferences next;
          BOOL reset = [input[@"resetGlobal"] boolValue];
          BOOL allowed =
              reset || (validation && ot_preferences_read(&next, yyjson_doc_get_root(validation)) &&
                        ot_library_preferences_allowed(self->library, &next));
          yyjson_doc_free(validation);
          if (allowed &&
              (reset ? ot_reset_preferences(self->core) : ot_set_preferences(self->core, json.UTF8String))) {
              ot_library_preferences_changed(self->library, &previous);
              for (NSNumber *key in [self.libraryTasks.allKeys copy]) {
                  NSURLSessionDataTask *task = self.libraryTasks[key];
                  if (!network_allowed(self, task.currentRequest.URL, task_scope(task))) {
                      [task cancel]; [self.libraryTasks removeObjectForKey:key];
                      ot_library_end(self->library, key.unsignedLongLongValue, "internet_disabled");
                  }
              }
              if (self.task && !network_allowed(self, self.task.currentRequest.URL, OT_NETWORK_SERVERS)) {
                  [self.task cancel]; self.task = nil;
                  ot_end(self->core, self.operation, "internet_disabled");
              }
              if (!ot_get_preferences(self->core)->internet) {
                  [self.session invalidateAndCancel]; self.session = nil;
              }
              BOOL servers = ot_network_allowed(ot_get_preferences(self->core), OT_NETWORK_SERVERS);
              const ot_connection *connection = ot_configuration(self->core);
              if (!servers && connection->remote) ot_connection_failed(self->core,"internet_disabled");
              dispatch_async(dispatch_get_main_queue(), ^{
                  if (!servers) [self.connectionManager revokeRemoteAccess];
                  else [self refreshConnection];
              });
              NSString *uiLanguage = @(ot_get_preferences(self->core)->language);
              NSString *theme = @(ot_get_preferences(self->core)->theme);
              BOOL developer = ot_get_preferences(self->core)->developer;
              dispatch_async(dispatch_get_main_queue(), ^{
                if (!developer) { [self stopObservation]; [self.motionTrace cancel]; }
                [self setTheme:theme];
                if (![uiLanguage isEqualToString:self.language])
                    [self applyLanguage:uiLanguage];
                [self.web callAsyncJavaScript:@"window.overtonePreferencesSaved()"
                                    arguments:@{}
                                      inFrame:nil
                               inContentWorld:WKContentWorld.pageWorld
                            completionHandler:nil];
              });
          }
        });
        return;
    }
    if ([command isEqual:@"chatThinking"]) {
        NSString *mode = string(@"mode");
        dispatch_async(owner, ^{
          ot_chat_thinking(self->core, mode.UTF8String);
        });
        return;
    }
    if ([command isEqual:@"draft"]) {
        NSString *text = string(@"text");
        int64_t chat = number(@"chat");
        dispatch_async(owner, ^{
          ot_draft(self->core, chat, text.UTF8String);
        });
        return;
    }
    if ([command isEqual:@"classify"]) {
        int value = (int)number(@"value");
        dispatch_async(owner, ^{
          ot_classify(self->core, value);
        });
        return;
    }
    if ([command isEqual:@"emptyTrash"]) {
        dispatch_async(owner, ^{
          ot_empty_trash(self->core);
        });
        return;
    }
    if ([command isEqual:@"restoreArchive"]) {
        dispatch_async(owner, ^{
          ot_restore_archive(self->core);
        });
        return;
    }
    if ([command isEqual:@"filter"]) {
        int value = (int)number(@"value");
        dispatch_async(owner, ^{
          ot_filter(self->core, value);
        });
        return;
    }
    if ([command isEqual:@"improve"]) {
        NSString *text = string(@"text");
        dispatch_async(owner, ^{
          ot_improve(self->core, text.UTF8String);
        });
        return;
    }
    if ([command isEqual:@"connectionDraft"]) {
        self.connectionEditing = YES;
        self.connectionRestoring = NO;
        [self.connectionManager cancel];
    } else if ([command isEqual:@"connectionDismiss"]) {
        self.connectionEditing = NO;
        self.connectionRestoring = NO;
        [self.connectionManager cancel];
    } else if ([command isEqual:@"connectionRefresh"]) {
        [self refreshConnection];
    } else if ([command isEqual:@"connectionProbe"]) {
        self.connectionRestoring = NO;
        NSMutableDictionary *draft = [input mutableCopy];
        if ([input[@"useKey"] isKindOfClass:NSNumber.class] && [input[@"useKey"] boolValue] &&
            [input[@"https"] isKindOfClass:NSNumber.class] && [input[@"key"] isEqual:@""]) {
            NSString *server = OTServerBase(input[@"endpoint"],[input[@"https"] boolValue]);
            draft[@"key"] = session_key(self,server) ?: @"";
        }
        [self.connectionManager probe:draft];
    } else if ([command isEqual:@"connectionSelect"]) {
        [self.connectionManager select:string(@"model")];
    } else if ([command isEqual:@"connectionCommit"]) {
        [self.connectionManager commit:(NSUInteger)number(@"revision") remember:[input[@"remember"] boolValue]];
    } else if ([command isEqual:@"connectionCancel"]) {
        self.connectionRestoring = NO;
        [self.connectionManager cancel];
    } else if ([command isEqual:@"send"] || [command isEqual:@"retry"]) {
        NSString *prompt = [command isEqual:@"retry"] ? nil : string(@"prompt");
        int64_t replace = number(@"replace");
        int64_t chat = number(@"chat"), request = number(@"request");
        BOOL follow = [input[@"follow"] isKindOfClass:NSNumber.class] && [input[@"follow"] boolValue];
        dispatch_async(owner, ^{
          const ot_connection *c = ot_configuration(self->core);
          if (!destination(@(c->endpoint), c->ollama ? @"ollama" : @"openai", c->remote)) {
              dispatch_async(dispatch_get_main_queue(), ^{
                [self.web
                    callAsyncJavaScript:@"window.overtoneAccepted(result);window.overtoneError('Controlla la configurazione del server.',false,'notice')"
                              arguments:@{@"result":@{@"request":@(request),@"chat":@(chat),@"accepted":@NO}}
                                inFrame:nil
                         inContentWorld:WKContentWorld.pageWorld
                      completionHandler:nil];
              });
              return;
          }
          /* Reject a stale renderer selection; never submit to a different chat. */
          BOOL accepted = chat == ot_selected_chat(self->core) &&
                          (!follow || ot_page(self->core, chat, 0)) &&
                          ot_submit(self->core, prompt.UTF8String, replace);
          int64_t selected = ot_selected_chat(self->core);
          dispatch_async(dispatch_get_main_queue(), ^{
            [self.web callAsyncJavaScript:@"window.overtoneAccepted(result)"
                                arguments:@{@"result":@{@"request":@(request), @"chat":@(selected), @"accepted":@(accepted)}}
                                  inFrame:nil
                           inContentWorld:WKContentWorld.pageWorld
                        completionHandler:nil];
          });
        });
    } else if ([command isEqual:@"stop"])
        dispatch_async(owner, ^{
          ot_stop(self->core);
        });
    else if ([command isEqual:@"chatVisibility"]) {
        if (![input[@"visible"] isKindOfClass:NSNumber.class])
            return;
        BOOL visible = [input[@"visible"] boolValue];
        dispatch_async(owner, ^{
          ot_chat_visible(self->core, visible);
        });
    } else if ([command isEqual:@"select"]) {
        int64_t chat = number(@"id"), match = number(@"match");
        dispatch_async(owner, ^{
          if (ot_select(self->core, chat) && match && ot_choose(self->core, match))
              ot_page(self->core, chat, match);
        });
    } else if ([command isEqual:@"choose"]) {
        int64_t turn = number(@"id");
        dispatch_async(owner, ^{
          ot_choose(self->core, turn);
        });
    } else if ([command isEqual:@"page"]) {
        int64_t anchor = number(@"id"), chat = number(@"chat");
        dispatch_async(owner, ^{
          ot_page(self->core, chat, anchor);
        });
    } else if ([command isEqual:@"searchFilters"]) {
        id filters = input[@"value"];
        NSData *filterData = [filters isKindOfClass:NSDictionary.class]
                                 ? [NSJSONSerialization dataWithJSONObject:filters options:0 error:NULL]
                                 : nil;
        NSString *json =
            filterData ? [[NSString alloc] initWithData:filterData encoding:NSUTF8StringEncoding] : nil;
        if (json)
            dispatch_async(owner, ^{
              ot_search_filters(self->core, json.UTF8String);
            });
    } else if ([command isEqual:@"search"]) {
        NSString *query = string(@"query");
        int64_t before = number(@"before");
        dispatch_async(owner, ^{
          ot_search(self->core, query.UTF8String, before);
        });
    } else if ([command isEqual:@"rename"]) {
        NSString *title = string(@"title");
        dispatch_async(owner, ^{
          if (ot_rename(self->core, title.UTF8String))
              dispatch_async(dispatch_get_main_queue(), ^{
                [self.web callAsyncJavaScript:@"closeDialog(document.getElementById('rename-dialog'))"
                                    arguments:@{}
                                      inFrame:nil
                               inContentWorld:WKContentWorld.pageWorld
                            completionHandler:nil];
              });
        });
    }
}
- (void)sendLocation {
    if (!self.web || self.web.loading)
        return;
    NSDictionary *status = @{
        @"active" : @(location.active),
        @"pending" : @(location.pending),
        @"custom" : @(strcmp(location.active, location.base) != 0),
        @"bytes" : @(location.bytes),
        @"files" : @(location.files),
        @"busy" : @(self.locationBusy),
        @"error" : @(location.error)
    };
    [self.web callAsyncJavaScript:@"window.overtoneLocation?.(status)"
                        arguments:@{@"status" : status}
                          inFrame:nil
                   inContentWorld:WKContentWorld.pageWorld
                completionHandler:nil];
}
- (void)locationAction:(NSString *)action {
    if (self.locationBusy)
        return;
    if ([action isEqual:@"status"]) {
        [self sendLocation];
        return;
    }
    if ([action isEqual:@"restart"] && *location.pending) {
        self.restarting = YES;
        [NSApp terminate:self];
        return;
    }
    if (![@[ @"change", @"default", @"cancel" ] containsObject:action])
        return;
    void (^prepare)(NSString *) = ^(NSString *target) {
      self.locationBusy = YES;
      [self sendLocation];
      ot_location initial = self->location;
      dispatch_group_async(self.fileWork, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        ot_location next = initial;
        if ([action isEqual:@"cancel"])
            ot_location_cancel(&next);
        else
            ot_location_plan(&next, target.fileSystemRepresentation);
        dispatch_async(dispatch_get_main_queue(), ^{
          self->location = next;
          self.locationBusy = NO;
          [self sendLocation];
        });
      });
    };
    if ([action isEqual:@"change"]) {
        NSOpenPanel *panel = [NSOpenPanel openPanel];
        panel.appearance = self.window.effectiveAppearance;
        panel.canChooseFiles = NO;
        panel.canChooseDirectories = YES;
        panel.canCreateDirectories = YES;
        panel.allowsMultipleSelection = NO;
        panel.title = [self text:@"Scegli cartella dati" english:@"Choose data folder"];
        panel.prompt = [self text:@"Scegli" english:@"Choose"];
        [panel beginSheetModalForWindow:self.window
                      completionHandler:^(NSModalResponse response) {
                        if (response == NSModalResponseOK)
                            prepare(panel.URL.path);
                      }];
    } else
        prepare(@(location.base));
}
- (void)fileOperation:(NSString *)operation {
    if (self.fileBusy)
        return;
    self.fileBusy = YES;
    BOOL restore = [operation isEqual:@"restore"];
    NSSavePanel *panel = restore ? [NSOpenPanel openPanel] : [NSSavePanel savePanel];
    panel.appearance = self.window.effectiveAppearance;
    panel.title = restore ? [self text:@"Ripristina backup" english:@"Restore backup"]
                          : [self text:@"Salva dati OverTone" english:@"Save OverTone data"];
    panel.prompt =
        restore ? [self text:@"Ripristina" english:@"Restore"] : [self text:@"Salva" english:@"Save"];
    if (!restore)
        panel.nameFieldStringValue = [operation isEqual:@"export"] ? @"OverTone.ndjson" : @"OverTone.sqlite";
    [panel
        beginSheetModalForWindow:self.window
               completionHandler:^(NSModalResponse response) {
                 if (response != NSModalResponseOK) {
                     self.fileBusy = NO;
                     return;
                 }
                 NSString *path = panel.URL.path;
                 dispatch_async(self->owner, ^{
                   if (restore && !ot_maintenance(self->core, true, NULL)) {
                       dispatch_async(dispatch_get_main_queue(), ^{ self.fileBusy = NO; });
                       return;
                   }
                   NSString *database = [self.profile stringByAppendingPathComponent:@"workspace.sqlite"];
                   NSString *temporary =
                       restore ? [self.profile stringByAppendingPathComponent:@"restore.sqlite"]
                               : [path stringByAppendingFormat:@".%@.partial", NSUUID.UUID.UUIDString];
                   dispatch_group_async(self.fileWork, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
                     BOOL ok = [operation isEqual:@"export"]
                                   ? ot_store_export(database.fileSystemRepresentation,
                                                     temporary.fileSystemRepresentation)
                                   : ot_store_backup((restore ? path : database).fileSystemRepresentation,
                                                     temporary.fileSystemRepresentation);
                     if (ok && !restore)
                         ok = rename(temporary.fileSystemRepresentation, path.fileSystemRepresentation) == 0;
                     if (!ok && !restore)
                         unlink(temporary.fileSystemRepresentation);
                     if (restore)
                         dispatch_async(self->owner, ^{
                           ot_maintenance(self->core, false, ok ? NULL : "file_operation");
                         });
                     dispatch_async(dispatch_get_main_queue(), ^{
                       self.fileBusy = NO;
                       [self.web callAsyncJavaScript:@"window.overtoneFileResult(ok,restore)"
                                           arguments:@{
                                               @"ok" : @(ok),
                                               @"restore" : @(restore)
                                           }
                                             inFrame:nil
                                      inContentWorld:WKContentWorld.pageWorld
                                   completionHandler:nil];
                     });
                   });
                 });
               }];
}
- (void)webView:(WKWebView *)web
    runJavaScriptConfirmPanelWithMessage:(NSString *)message
                        initiatedByFrame:(WKFrameInfo *)frame
                       completionHandler:(void (^)(BOOL))completion {
    (void)web;
    if (!frame.mainFrame) {
        completion(NO);
        return;
    }
    NSAlert *alert = [NSAlert new];
    alert.window.appearance = self.window.effectiveAppearance;
    alert.messageText = @"OverTone";
    alert.informativeText = message;
    [alert addButtonWithTitle:[self text:@"Conferma" english:@"Confirm"]];
    [alert addButtonWithTitle:[self text:@"Annulla" english:@"Cancel"]];
    [alert beginSheetModalForWindow:self.window
                  completionHandler:^(NSModalResponse response) {
                    completion(response == NSAlertFirstButtonReturn);
                  }];
}
- (void)webView:(WKWebView *)web
    decidePolicyForNavigationAction:(WKNavigationAction *)action
                    decisionHandler:(void (^)(WKNavigationActionPolicy))decision {
    (void)web;
    NSURL *url = action.request.URL;
    NSURL *allowed = [NSBundle.mainBundle.resourceURL URLByAppendingPathComponent:@"ui/index.html"];
    BOOL trusted = url.isFileURL && [url.path isEqual:allowed.path] && action.targetFrame.mainFrame;
    if (!trusted)
        self.uiStatus = @"navigation_rejected";
    decision(trusted ? WKNavigationActionPolicyAllow : WKNavigationActionPolicyCancel);
}
- (void)webView:(WKWebView *)web
    didFailProvisionalNavigation:(WKNavigation *)navigation
                       withError:(NSError *)error {
    (void)web;
    (void)navigation;
    self.uiStatus = [NSString stringWithFormat:@"load_error_%ld", (long)error.code];
}
- (void)webViewWebContentProcessDidTerminate:(WKWebView *)web {
    self.uiStatus = @"renderer_terminated";
    if (self.terminating || self.servicesStopped) return;
    NSTimeInterval time = NSProcessInfo.processInfo.systemUptime;
    if (self.rendererRecoveryTime > 0 && time - self.rendererRecoveryTime < 60) {
        [self fail:[self text:@"L’interfaccia non riesce a riavviarsi. Riapri OverTone."
                     english:@"The interface could not restart. Reopen OverTone."]];
        return;
    }
    self.rendererRecoveryTime = time;
    [web reload];
}
- (void)URLSession:(NSURLSession *)session task:(NSURLSessionTask *)task
    didFinishCollectingMetrics:(NSURLSessionTaskMetrics *)metrics {
    (void)session;
    // Opt-in, bounded, content-free observations; never endpoints or HTTP headers.
    if (!core || !ot_get_preferences(core)->developer || !task.taskDescription.length) return;
    int64_t wire = 0, decoded = 0;
    NSString *protocol = @"other";
    for (NSURLSessionTaskTransactionMetrics *transaction in metrics.transactionMetrics) {
        wire += MAX((int64_t)0, transaction.countOfResponseBodyBytesReceived);
        decoded += MAX((int64_t)0, transaction.countOfResponseBodyBytesAfterDecoding);
        if ([@[@"http/1.1",@"h2",@"h3"] containsObject:transaction.networkProtocolName])
            protocol = transaction.networkProtocolName;
    }
    BOOL file = [[task.originalRequest valueForHTTPHeaderField:@"Accept-Encoding"] isEqual:@"identity"] &&
                [task.originalRequest.HTTPMethod isEqual:@"GET"];
    if (!self.networkMetrics) self.networkMetrics = [NSMutableArray new];
    [self.networkMetrics addObject:@{@"id":@(task.taskDescription.longLongValue),@"file":@(file),
        @"wireBytes":@(wire),@"decodedBytes":@(decoded),@"protocol":protocol,
        @"durationMs":@(metrics.taskInterval.duration*1000),@"redirects":@(metrics.redirectCount)}];
    if (self.networkMetrics.count > 12) [self.networkMetrics removeObjectAtIndex:0];
}
- (void)URLSession:(NSURLSession *)session didReceiveChallenge:(NSURLAuthenticationChallenge *)challenge
 completionHandler:(void (^)(NSURLSessionAuthChallengeDisposition, NSURLCredential *))completion {
    (void)session;
    if (!OTLocalServerTrust(challenge,completion)) completion(NSURLSessionAuthChallengePerformDefaultHandling,nil);
}
- (void)URLSession:(NSURLSession *)session
                          task:(NSURLSessionTask *)task
    willPerformHTTPRedirection:(NSHTTPURLResponse *)response
                    newRequest:(NSURLRequest *)request
             completionHandler:(void (^)(NSURLRequest *))completion {
    (void)session;
    (void)task;
    (void)response;
    (void)request;
    uint64_t id = (uint64_t)task.taskDescription.longLongValue;
    BOOL acquisition = self.libraryTasks[@(id)] == task;
    BOOL secure = [request.URL.scheme.lowercaseString isEqual:@"https"] ||
                  [request.URL.host isEqual:task.originalRequest.URL.host];
    BOOL boundary = !OTServerLocal(request.URL) || OTServerLocal(task.originalRequest.URL);
    completion(acquisition && libraryURL(request.URL) && secure && boundary &&
               network_allowed(self, request.URL, task_scope(task)) ? request : nil);
}
- (void)URLSession:(NSURLSession *)session
              dataTask:(NSURLSessionDataTask *)task
    didReceiveResponse:(NSURLResponse *)response
     completionHandler:(void (^)(NSURLSessionResponseDisposition))completion {
    (void)session;
    NSInteger status = [(NSHTTPURLResponse *)response statusCode];
    uint64_t id = (uint64_t)task.taskDescription.longLongValue;
    if (self.libraryTasks[@(id)] == task) {
        NSHTTPURLResponse *http = (NSHTTPURLResponse *)response;
        BOOL ok = ot_library_headers(
            library, id, (int)status, [http valueForHTTPHeaderField:@"ETag"].UTF8String,
            [http valueForHTTPHeaderField:@"Content-Range"].UTF8String, response.expectedContentLength);
        completion(ok ? NSURLSessionResponseAllow : NSURLSessionResponseCancel);
        return;
    }
    BOOL ok = task == self.task && status >= 200 && status < 300;
    if (!ok && task == self.task)
        self.transportError = [NSString stringWithFormat:@"http_%ld", (long)status];
    completion(ok ? NSURLSessionResponseAllow : NSURLSessionResponseCancel);
}
- (void)URLSession:(NSURLSession *)session
          dataTask:(NSURLSessionDataTask *)task
    didReceiveData:(NSData *)data {
    (void)session;
    if (task == self.task) {
        ot_bytes(core, self.operation, data.bytes, data.length);
        [self scheduleStreamRender];
    } else {
        uint64_t id = (uint64_t)task.taskDescription.longLongValue;
        if (self.libraryTasks[@(id)] == task)
            ot_library_bytes(library, id, data.bytes, data.length);
    }
}
- (void)URLSession:(NSURLSession *)session
                    task:(NSURLSessionTask *)task
    didCompleteWithError:(NSError *)error {
    (void)session;
    if (task != self.task) {
        uint64_t id = (uint64_t)task.taskDescription.longLongValue;
        if (self.libraryTasks[@(id)] == task) {
            [self.libraryTasks removeObjectForKey:@(id)];
            ot_library_end(library, id, OTTransportError(error).UTF8String);
            [self scheduleStreamRender];
        }
        return;
    }
    NSString *code = self.transportError ?: OTTransportError(error);
    ot_end(core, self.operation, code.UTF8String);
    self.task = nil;
    [self scheduleStreamRender];
    dispatch_async(dispatch_get_main_queue(), ^{ [self refreshConnection]; });
}
- (BOOL)windowShouldClose:(NSWindow *)sender {
    (void)sender;
    if (!self.locationBusy) return YES;
    atomic_store(&locationCancel, true);
    return NO;
}
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender {
    (void)sender;
    return !self.locationBusy;
}
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)sender {
    if (self.locationBusy) {
        atomic_store(&locationCancel, true);
        return NSTerminateCancel;
    }
    (void)sender;
    if (self.opening || !core || !self.web)
        return NSTerminateNow;
    if (self.terminating)
        return NSTerminateLater;
    self.terminating = YES;
    __block BOOL submitted = NO;
    void (^finish)(id) = ^(id value) {
      if (submitted)
          return;
      submitted = YES;
      NSDictionary *draft = [value isKindOfClass:NSDictionary.class] ? value : nil;
      dispatch_async(self->owner, ^{
        if ([draft[@"text"] isKindOfClass:NSString.class] && [draft[@"chat"] isKindOfClass:NSNumber.class])
            ot_draft(self->core, [draft[@"chat"] longLongValue], [draft[@"text"] UTF8String]);
        dispatch_async(dispatch_get_main_queue(), ^{
          if (!self.restarting) {
              [NSApp replyToApplicationShouldTerminate:YES];
              return;
          }
          [self.window orderOut:nil];
          [self stopServices];
          NSWorkspaceOpenConfiguration *configuration = [NSWorkspaceOpenConfiguration configuration];
          configuration.createsNewApplicationInstance = YES;
          configuration.activates = YES;
          configuration.arguments = @[ @"--profile", @(self->location.base) ];
          [NSWorkspace.sharedWorkspace openApplicationAtURL:NSBundle.mainBundle.bundleURL
                                              configuration:configuration
                                          completionHandler:^(NSRunningApplication *app, NSError *error) {
                                            (void)app;
                                            dispatch_async(dispatch_get_main_queue(), ^{
                                              if (error)
                                                  NSLog(@"OverTone restart failed (%ld)", (long)error.code);
                                              [NSApp replyToApplicationShouldTerminate:YES];
                                            });
                                          }];
        });
      });
    };
    /* A stopped renderer must not hold the app and its engines open indefinitely.
     * The periodic draft remains durable if this final read cannot complete. */
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
      finish(nil);
    });
    [self.web callAsyncJavaScript:@"return window.overtoneDraft()"
                        arguments:@{}
                          inFrame:nil
                   inContentWorld:WKContentWorld.pageWorld
                completionHandler:^(id value, NSError *error) {
                  (void)error;
                  finish(value);
                }];
    return NSTerminateLater;
}
- (void)applicationWillTerminate:(NSNotification *)notification {
    (void)notification;
    if (self.observingAppearance) {
        [NSApp removeObserver:self forKeyPath:@"effectiveAppearance" context:&appearance_context];
        self.observingAppearance = NO;
    }
    [self stopServices];
}
- (void)stopServices {
    if (self.servicesStopped)
        return;
    self.servicesStopped = YES;
    [self.connectionManager close];
    [self stopObservation];
    [self.motionTrace cancel];
    if (self.opening) {
        /* No wait on a file operation blocked by the OS. Process exit releases the
         * locks; the engine supervisor owns cleanup if startup already spawned one. */
        [self.session invalidateAndCancel];
        [self.localSession invalidateAndCancel];
        return;
    }
    if (self.timer)
        dispatch_source_cancel(self.timer);
    if (self.fileWork)
        dispatch_group_wait(self.fileWork, DISPATCH_TIME_FOREVER);
    /* Join diagnostics before closing its owner; never join it on the owner queue. */
    ot_diagnostics_close(self.diagnostics);
    if (owner)
        dispatch_sync(owner, ^{
          [self.session invalidateAndCancel];
        [self.localSession invalidateAndCancel];
          ot_library_close(self->library);
          self->library = NULL;
          [self.libraryTasks removeAllObjects];
          ot_close(self->core);
          self->core = NULL;
          self.task = nil;
        });
    if (self.lock >= 0)
        close(self.lock);
    if (self.locationLock >= 0)
        close(self.locationLock);
}
@end

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        umask(0077);
        if (argc > 2 && !strcmp(argv[1], "--supervise"))
            return ot_engine_supervise((char *const *)(argv + 2));
        if (argc == 3 && strcmp(argv[1], "--inspect") == 0) {
            ot_location observed;
            return ot_location_open(&observed, argv[2]) ? ot_inspect(observed.active) : 1;
        }
        if (argc >= 4 && argc <= 6 && !strcmp(argv[1], "--control")) {
            NSMutableArray<NSString *> *parts = [NSMutableArray new];
            for (int i = 3; i < argc; i++) {
                NSString *part = [NSString stringWithUTF8String:argv[i]];
                if (!part) return 2;
                [parts addObject:part];
            }
            ot_location observed;
            return ot_location_open(&observed, argv[2]) ?
                ot_control(observed.active, [parts componentsJoinedByString:@" "].UTF8String) : 1;
        }
        NSString *profile = nil;
        BOOL showSetup = NO;
        for (int i = 1; i < argc; i++) {
            if (!strcmp(argv[i], "--setup") && !showSetup)
                showSetup = YES;
            else if (!strcmp(argv[i], "--profile") && !profile && i + 1 < argc) {
                profile = [NSString stringWithUTF8String:argv[++i]];
                if (!profile) return 2;
            } else {
                fprintf(stderr, "usage: overtone [--profile DIRECTORY] [--setup] | --inspect DIRECTORY | --control DIRECTORY COMMAND\n");
                return 2;
            }
        }
        // Launchers may inherit a protected working directory (for example a
        // checkout on Desktop). App resources and profiles are absolute paths;
        // neither AppKit nor incidental helpers need access to that directory.
        if (chdir("/")) {
            perror("overtone working directory");
            return 1;
        }
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        Desktop *delegate = [Desktop new];
        delegate.lock = -1;
        delegate.locationLock = -1;
        delegate.showSetup = showSetup;
        delegate.profile = profile ?: [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Application Support/OverTone"];
        if (![delegate.profile isAbsolutePath])
            return 2;
        NSApp.delegate = delegate;
        [delegate applyLanguage:[NSLocale.preferredLanguages.firstObject componentsSeparatedByString:@"-"].firstObject];
        [NSApp run];
    }
    return 0;
}
