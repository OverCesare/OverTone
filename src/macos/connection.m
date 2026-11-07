#import "connection.h"
#import <Security/Security.h>

NSURLSessionConfiguration *OTNetworkConfiguration(BOOL local) {
    NSURLSessionConfiguration *c = NSURLSessionConfiguration.ephemeralSessionConfiguration;
    c.URLCache = nil; c.HTTPCookieStorage = nil; c.URLCredentialStorage = nil;
    c.HTTPShouldSetCookies = NO;
    c.requestCachePolicy = NSURLRequestReloadIgnoringLocalCacheData;
    // An explicit empty dictionary bypasses system proxies/PAC for loopback.
    // Remote sessions retain the user's system proxy settings after consent.
    if (local) c.connectionProxyDictionary = @{};
    return c;
}
NSString *OTTransportError(NSError *error) {
    if (!error) return nil;
    if ([error.domain isEqual:NSURLErrorDomain]) {
        if (error.code == NSURLErrorAppTransportSecurityRequiresSecureConnection) return @"connection_secure_required";
        if (error.code == NSURLErrorTimedOut) return @"connection_timeout";
        if (error.code <= NSURLErrorSecureConnectionFailed && error.code >= NSURLErrorClientCertificateRequired)
            return @"connection_secure";
    }
    return @"connection_failed";
}
static NSString *const service = @"com.overtone.desktop.server";
static NSString *http_problem(NSInteger status) {
    if (status == 401 || status == 403) return @"connection_auth";
    if (status == 429 || status == 503) return @"connection_busy";
    return nil;
}
static NSDictionary *credential(NSString *server) {
    return @{(__bridge id)kSecClass:(__bridge id)kSecClassGenericPassword,
             (__bridge id)kSecAttrService:service, (__bridge id)kSecAttrAccount:server};
}
static BOOL quiet_keychain(void) {
    static dispatch_once_t once;
    static BOOL ready;
    dispatch_once(&once, ^{
        /* Ad-hoc signing cannot use the provisioned Data Protection keychain.
         * LAContext/UI-skip do not suppress legacy ACL dialogs on macOS.
         * This public, process-local policy fails closed without requesting UI. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        ready = SecKeychainSetUserInteractionAllowed(false) == errSecSuccess;
#pragma clang diagnostic pop
    });
    return ready;
}
static NSString *read_key(NSString *server, OSStatus *result) {
    if (!quiet_keychain()) { if (result) *result = errSecInteractionNotAllowed; return nil; }
    NSMutableDictionary *query = [credential(server) mutableCopy];
    query[(__bridge id)kSecReturnData] = @YES;
    query[(__bridge id)kSecMatchLimit] = (__bridge id)kSecMatchLimitOne;
    CFTypeRef value = NULL;
    OSStatus status = SecItemCopyMatching((__bridge CFDictionaryRef)query, &value);
    if (result) *result = status;
    NSData *data = CFBridgingRelease(value);
    return status == errSecSuccess && [data isKindOfClass:NSData.class] ?
        [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] : nil;
}
static BOOL save_key(NSString *server, NSString *key, BOOL remember) {
    /* Session-only use does not overwrite a credential remembered earlier. */
    if (!remember) return YES;
    if (!quiet_keychain()) return NO;
    NSMutableDictionary *query = [credential(server) mutableCopy];
    if (!key.length) {
        OSStatus status = SecItemDelete((__bridge CFDictionaryRef)query);
        return status == errSecSuccess || status == errSecItemNotFound;
    }
    OSStatus access = errSecSuccess;
    read_key(server,&access);
    /* A verified key explicitly re-entered after a signing change replaces only
     * this server's inaccessible item. Never broaden its ACL or read other items. */
    if (access == errSecAuthFailed || access == errSecInteractionNotAllowed) {
        OSStatus removed = SecItemDelete((__bridge CFDictionaryRef)query);
        if (removed != errSecSuccess && removed != errSecItemNotFound) return NO;
    }
    NSData *data = [key dataUsingEncoding:NSUTF8StringEncoding];
    NSDictionary *change = @{(__bridge id)kSecValueData:data};
    OSStatus status = SecItemUpdate((__bridge CFDictionaryRef)query, (__bridge CFDictionaryRef)change);
    if (status == errSecItemNotFound) {
        NSMutableDictionary *item = [query mutableCopy];
        item[(__bridge id)kSecValueData] = data;
        item[(__bridge id)kSecAttrLabel] = [@"OverTone · " stringByAppendingString:server];
        item[(__bridge id)kSecAttrSynchronizable] = @NO;
        status = SecItemAdd((__bridge CFDictionaryRef)item, NULL);
    }
    return status == errSecSuccess;
}
NSString *OTServerBase(NSString *raw, BOOL secure) {
    if (![raw isKindOfClass:NSString.class] || [raw lengthOfBytesUsingEncoding:NSUTF8StringEncoding] > 2048)
        return nil;
    raw = [raw stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    if (!raw.length || [raw rangeOfCharacterFromSet:NSCharacterSet.controlCharacterSet].location != NSNotFound) return nil;
    if (![raw containsString:@"://"]) raw = [@"https://" stringByAppendingString:raw];
    NSURLComponents *c = [NSURLComponents componentsWithString:raw];
    if (!c.host.length || c.user || c.password || c.query || c.fragment ||
        ![@[@"http",@"https"] containsObject:c.scheme.lowercaseString]) return nil;
    c.scheme = secure ? @"https" : @"http";
    c.host = c.host.lowercaseString;
    if (c.port && (c.port.longLongValue < 1 || c.port.longLongValue > 65535)) return nil;
    if (c.port.integerValue == (secure ? 443 : 80)) c.port = nil;
    NSString *path = c.percentEncodedPath;
    while ([path hasSuffix:@"/"]) path = [path substringToIndex:path.length-1];
    for (NSString *suffix in @[@"/v1/chat/completions",@"/api/chat",@"/v1",@"/api"])
        if ([path hasSuffix:suffix]) { path = [path substringToIndex:path.length-suffix.length]; break; }
    c.percentEncodedPath = path;
    NSString *result = c.URL.absoluteString;
    return [result lengthOfBytesUsingEncoding:NSUTF8StringEncoding] <= 2048 ? result : nil;
}
BOOL OTServerLocal(NSURL *url) {
    return [@[@"localhost",@"127.0.0.1",@"::1",@"[::1]"] containsObject:url.host.lowercaseString];
}
BOOL OTLocalServerTrust(NSURLAuthenticationChallenge *challenge,
                       void (^completion)(NSURLSessionAuthChallengeDisposition, NSURLCredential *)) {
    NSURLProtectionSpace *space = challenge.protectionSpace;
    if (![space.authenticationMethod isEqual:NSURLAuthenticationMethodServerTrust] ||
        ![@[@"localhost",@"127.0.0.1",@"::1",@"[::1]"] containsObject:space.host.lowercaseString]) return NO;
    SecTrustRef trust = space.serverTrust;
    if (!trust || SecTrustSetNetworkFetchAllowed(trust, false) != errSecSuccess) {
        completion(NSURLSessionAuthChallengeCancelAuthenticationChallenge,nil); return YES;
    }
    CFRetain(trust);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY,0), ^{
        BOOL valid = SecTrustEvaluateWithError(trust,NULL);
        completion(valid ? NSURLSessionAuthChallengeUseCredential : NSURLSessionAuthChallengeCancelAuthenticationChallenge,
                   valid ? [NSURLCredential credentialForTrust:trust] : nil);
        CFRelease(trust);
    });
    return YES;
}
NSURL *OTServerChat(NSString *base, BOOL ollama, BOOL remote) {
    NSString *server = OTServerBase(base, [base.lowercaseString hasPrefix:@"https:"]);
    NSURL *url = server ? [NSURL URLWithString:server] : nil;
    if (!url || (!remote && !OTServerLocal(url))) return nil;
    return [NSURL URLWithString:[server stringByAppendingString:ollama ? @"/api/chat" : @"/v1/chat/completions"]];
}

@implementation OTConnection {
    NSURLSession *_localSession, *_remoteSession;
    NSURLSessionDataTask *_task;
    NSMutableData *_body;
    void (^_completion)(NSInteger, NSString *);
    NSUInteger _revision;
    NSString *_server, *_protocol, *_key, *_model, *_phase, *_failure;
    NSArray *_models;
    NSDictionary *_modelRow;
    ot_server_info _info;
    BOOL _automatic, _credentialStored;
    BOOL _restore;
}
- (instancetype)init {
    self = [super init];
    if (self) {
        NSURLSessionConfiguration *c = OTNetworkConfiguration(YES);
        c.timeoutIntervalForRequest = 10; c.timeoutIntervalForResource = 20;
        _localSession = [NSURLSession sessionWithConfiguration:c delegate:self delegateQueue:NSOperationQueue.mainQueue];
        _models = @[];
    }
    return self;
}
- (void)emit:(NSString *)phase error:(NSString *)error {
    _phase = phase;
    if (_restore && [phase isEqual:@"ready"]) { [self activate]; return; }
    if (_restore && ![phase isEqual:@"error"]) return;
    if (!self.update) return;
    self.update(@{@"revision":@(_revision),@"phase":phase,@"error":error ?: @"",@"restore":@(_restore),
        @"server":_server ?: @"",@"protocol":_protocol ?: @"",@"models":_models ?: @[],
        @"model":_model ?: @"",@"context":@(_info.model.context),@"residentKnown":@(_info.resident_known),
        @"resident":@(_info.resident),@"thinking":@(_info.thinking.count > 0),
        @"credentialStored":@(_credentialStored),@"quantization":@(_info.model.quantization)});
}
- (void)cancel {
    _revision++; [_task cancel]; _task = nil; _completion = nil; _body = nil;
    _key = nil; _info = (ot_server_info){0};
    _models = @[]; _modelRow = nil;
}
- (void)close { [self cancel]; [_localSession invalidateAndCancel]; [_remoteSession invalidateAndCancel]; }
- (void)URLSession:(NSURLSession *)session didReceiveChallenge:(NSURLAuthenticationChallenge *)challenge
 completionHandler:(void (^)(NSURLSessionAuthChallengeDisposition, NSURLCredential *))completion {
    (void)session;
    if (!OTLocalServerTrust(challenge,completion)) completion(NSURLSessionAuthChallengePerformDefaultHandling,nil);
}
- (void)fetch:(NSString *)path data:(NSDictionary *)data completion:(void (^)(NSInteger, NSString *))completion {
    [_task cancel]; _body = [NSMutableData new]; _completion = [completion copy]; _failure = nil;
    NSURL *url = [NSURL URLWithString:[_server stringByAppendingString:path]];
    if (!self.allowsURL || !self.allowsURL(url)) {
        _failure = @"internet_disabled";
        _completion = nil; _body = nil; _task = nil;
        [self emit:@"error" error:_failure];
        return;
    }
    NSMutableURLRequest *r = [NSMutableURLRequest requestWithURL:url];
    r.HTTPMethod = data ? @"POST" : @"GET";
    if (data) { r.HTTPBody = [NSJSONSerialization dataWithJSONObject:data options:0 error:NULL];
        [r setValue:@"application/json" forHTTPHeaderField:@"Content-Type"]; }
    if (_key.length) [r setValue:[@"Bearer " stringByAppendingString:_key] forHTTPHeaderField:@"Authorization"];
    BOOL local = OTServerLocal(url);
    if (!local && !_remoteSession) {
        NSURLSessionConfiguration *c = OTNetworkConfiguration(NO);
        c.timeoutIntervalForRequest = 10; c.timeoutIntervalForResource = 20;
        _remoteSession = [NSURLSession sessionWithConfiguration:c delegate:self delegateQueue:NSOperationQueue.mainQueue];
    }
    _task = [(local ? _localSession : _remoteSession) dataTaskWithRequest:r]; [_task resume];
}
- (void)URLSession:(NSURLSession *)session dataTask:(NSURLSessionDataTask *)task didReceiveResponse:(NSURLResponse *)response completionHandler:(void (^)(NSURLSessionResponseDisposition))handler {
    (void)session;
    if (task != _task) { handler(NSURLSessionResponseCancel); return; }
    if (response.expectedContentLength > 1048576) {
        _failure = @"connection_large"; handler(NSURLSessionResponseCancel);
    } else handler(NSURLSessionResponseAllow);
}
- (void)URLSession:(NSURLSession *)session dataTask:(NSURLSessionDataTask *)task didReceiveData:(NSData *)data {
    (void)session;
    if (task != _task) return;
    if (data.length > 1048576 - _body.length) { _failure = @"connection_large"; [task cancel]; return; }
    [_body appendData:data];
}
- (void)URLSession:(NSURLSession *)session task:(NSURLSessionTask *)task willPerformHTTPRedirection:(NSHTTPURLResponse *)response newRequest:(NSURLRequest *)request completionHandler:(void (^)(NSURLRequest *))handler {
    (void)session; (void)task; (void)response; (void)request; handler(nil);
}
- (void)URLSession:(NSURLSession *)session task:(NSURLSessionTask *)task didCompleteWithError:(NSError *)error {
    (void)session;
    if (task != _task) return;
    NSInteger status = error ? 0 : [(NSHTTPURLResponse *)task.response statusCode];
    NSString *body = [[NSString alloc] initWithData:_body encoding:NSUTF8StringEncoding];
    if (status >= 300 && status < 400) _failure = @"connection_redirect";
    if (!body || strlen(body.UTF8String) != _body.length) { _failure = @"connection_protocol"; status = 0; }
    if (!_failure) _failure = OTTransportError(error);
    void (^completion)(NSInteger, NSString *) = _completion;
    _task = nil; _completion = nil; _body = nil;
    if (completion) completion(status, body ?: @"");
}
- (void)list {
    [self fetch:[_protocol isEqual:@"ollama"] ? @"/api/tags" : @"/v1/models" data:nil completion:^(NSInteger status, NSString *body) {
        NSString *problem = http_problem(status);
        if (problem) { [self emit:@"error" error:problem]; return; }
        char *json = status == 200 ? ot_server_models(body.UTF8String, [self->_protocol isEqual:@"ollama"]) : NULL;
        if (!json && !self->_failure && self->_automatic && [self->_protocol isEqual:@"ollama"] && status != 0) {
            self->_protocol = @"openai"; [self list]; return;
        }
        if (!json) { [self emit:@"error" error:self->_failure ?: (status ? @"connection_protocol" : @"connection_failed")]; return; }
        NSData *data = [NSData dataWithBytes:json length:strlen(json)]; free(json);
        self->_models = [NSJSONSerialization JSONObjectWithData:data options:0 error:NULL] ?: @[];
        NSString *selected = self->_model;
        if (selected.length && [self->_models filteredArrayUsingPredicate:[NSPredicate predicateWithFormat:@"id == %@",selected]].count)
            [self select:selected];
        else if (self->_restore) [self emit:@"error" error:@"connection_missing"];
        else { self->_model = @""; [self emit:@"models" error:self->_models.count ? nil : @"connection_empty"]; }
    }];
}
- (void)revokeRemoteAccess {
    [_remoteSession invalidateAndCancel]; _remoteSession = nil;
    if (_server.length && !OTServerLocal([NSURL URLWithString:_server])) {
        [self cancel]; [self emit:@"error" error:@"internet_disabled"];
    }
}
- (void)probe:(NSDictionary *)input {
    [self cancel];
    _restore = NO;
    if (![input isKindOfClass:NSDictionary.class] || ![input[@"https"] isKindOfClass:NSNumber.class] ||
        ![input[@"useKey"] isKindOfClass:NSNumber.class]) {
        [self emit:@"error" error:@"invalid_input"]; return;
    }
    _restore = [input[@"restore"] isKindOfClass:NSNumber.class] && [input[@"restore"] boolValue];
    _server = OTServerBase(input[@"endpoint"], [input[@"https"] boolValue]);
    _model = [input[@"model"] isKindOfClass:NSString.class] &&
        [input[@"model"] lengthOfBytesUsingEncoding:NSUTF8StringEncoding] <= 256 ? input[@"model"] : @"";
    NSString *p = input[@"protocol"], *key = input[@"key"];
    if (!_server || ![@[@"auto",@"ollama",@"openai"] containsObject:p]) {
        [self emit:@"error" error:@"invalid_input"]; return;
    }
    if (![key isKindOfClass:NSString.class] || [key lengthOfBytesUsingEncoding:NSUTF8StringEncoding] > 4096 ||
        [key rangeOfCharacterFromSet:NSCharacterSet.controlCharacterSet].location != NSNotFound) {
        [self emit:@"error" error:@"connection_auth"]; return;
    }
    _models = @[]; _automatic = [p isEqual:@"auto"]; _protocol = _automatic ? @"ollama" : p;
    _key = [input[@"useKey"] boolValue] ? key : @""; _credentialStored = NO;
    [self emit:@"probing" error:nil];
    if ([input[@"useKey"] boolValue] && !_key.length) {
        NSUInteger revision = _revision; NSString *server = _server;
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY,0), ^{
            NSString *stored = read_key(server,NULL);
            dispatch_async(dispatch_get_main_queue(), ^{
                if (revision != self->_revision) return;
                self->_key = stored ?: @""; self->_credentialStored = stored.length > 0; [self list];
            });
        });
    } else [self list];
}
- (void)select:(NSString *)model {
    if (![model isKindOfClass:NSString.class] || [model lengthOfBytesUsingEncoding:NSUTF8StringEncoding] > 256) return;
    if (!model.length) {
        _revision++; [_task cancel]; _task = nil; _completion = nil; _body = nil;
        _model = @""; _modelRow = nil; _info = (ot_server_info){0};
        [self emit:@"models" error:nil]; return;
    }
    NSArray *rows = [_models filteredArrayUsingPredicate:[NSPredicate predicateWithFormat:@"id == %@",model]];
    if (!rows.count) return;
    _revision++;
    _model = model; _modelRow = rows.firstObject; _info = (ot_server_info){0};
    BOOL ollama = [_protocol isEqual:@"ollama"];
    [self emit:@"probing" error:nil];
    NSURLComponents *props = [NSURLComponents new];
    props.path = @"/props";
    NSCharacterSet *unreserved = [NSCharacterSet characterSetWithCharactersInString:
        @"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~"];
    props.percentEncodedQuery = [@"model=" stringByAppendingString:[model stringByAddingPercentEncodingWithAllowedCharacters:unreserved]];
    [self fetch:ollama ? @"/api/show" : props.string data:ollama ? @{@"model":model} : nil completion:^(NSInteger status, NSString *body) {
        NSString *problem = http_problem(status);
        if (problem) { [self emit:@"error" error:problem]; return; }
        if (status != 200 && (ollama || ![@[@404,@405,@501] containsObject:@(status)])) {
            [self emit:@"error" error:self->_failure ?: @"connection_failed"]; return;
        }
        NSString *metadata = status == 200 ? body : @"{}";
        ot_server_result parsed = ot_server_metadata(metadata.UTF8String, ollama, self->_model.UTF8String, &self->_info);
        if (parsed != OT_SERVER_OK) {
            [self emit:@"error" error:parsed == OT_SERVER_NOT_CHAT ? @"model_not_chat" : @"connection_protocol"]; return;
        }
        if (!self->_info.model.context) self->_info.model.context = [self->_modelRow[@"context"] unsignedIntValue];
        if (ollama) {
            [self fetch:@"/api/ps" data:nil completion:^(NSInteger code, NSString *runtime) {
                if (code == 200) ot_server_residency(runtime.UTF8String, self->_model.UTF8String, &self->_info);
                [self emit:@"ready" error:nil];
            }];
        } else [self emit:@"ready" error:nil];
    }];
}
- (void)commit:(NSUInteger)revision remember:(BOOL)remember {
    if (revision != _revision || !_info.verified || ![@[@"ready",@"error"] containsObject:_phase]) return;
    NSUInteger current = _revision; NSString *server = _server, *key = _key;
    [self emit:@"saving" error:nil];
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY,0), ^{
        BOOL stored = save_key(server, key, remember);
        dispatch_async(dispatch_get_main_queue(), ^{
            if (current != self->_revision) return;
            if (!stored) { [self emit:@"error" error:@"connection_keychain"]; return; }
            [self activate];
        });
    });
}
- (void)activate {
    ot_connection c = {.ollama=[_protocol isEqual:@"ollama"]};
    snprintf(c.endpoint,sizeof c.endpoint,"%s",_server.UTF8String);
    snprintf(c.model,sizeof c.model,"%s",_model.UTF8String);
    c.remote = !OTServerLocal([NSURL URLWithString:_server]);
    _info.has_key = _key.length > 0;
    if (self.apply) self.apply(c,_info,_key ?: @"");
}
@end
