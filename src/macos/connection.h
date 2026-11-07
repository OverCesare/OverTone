#import <Foundation/Foundation.h>
#include "core.h"
/* Shared address policy for discovery and generation. No redirects or TLS bypass. */
NSURLSessionConfiguration *OTNetworkConfiguration(BOOL local);
/* Stable product error code; nil on success. Does not expose server response text. */
NSString *OTTransportError(NSError *error);
NSString *OTServerBase(NSString *address, BOOL secure);
BOOL OTServerLocal(NSURL *url);
BOOL OTLocalServerTrust(NSURLAuthenticationChallenge *challenge,
                       void (^completion)(NSURLSessionAuthChallengeDisposition, NSURLCredential *));
NSURL *OTServerChat(NSString *base, BOOL ollama, BOOL remote);
/* One cancellable discovery, credentials scoped to the exact canonical server.
 * This adapter never launches engines and never owns the app's durable state. */
@interface OTConnection : NSObject <NSURLSessionDataDelegate, NSURLSessionTaskDelegate>
@property(nonatomic,copy) BOOL (^allowsURL)(NSURL *url);
@property(nonatomic,copy) void (^update)(NSDictionary *state);
@property(nonatomic,copy) void (^apply)(ot_connection connection, ot_server_info info, NSString *key);
- (void)probe:(NSDictionary *)input;
- (void)select:(NSString *)model;
- (void)commit:(NSUInteger)revision remember:(BOOL)remember;
- (void)cancel;
- (void)revokeRemoteAccess;
- (void)close;
@end
