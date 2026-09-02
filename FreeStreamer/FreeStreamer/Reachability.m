/*
 Copyright (c) 2011, Tony Million.
 All rights reserved.
 
 Redistribution and use in source and binary forms, with or without
 modification, are permitted provided that the following conditions are met:
 
 1. Redistributions of source code must retain the above copyright notice, this
 list of conditions and the following disclaimer.
 
 2. Redistributions in binary form must reproduce the above copyright notice,
 this list of conditions and the following disclaimer in the documentation
 and/or other materials provided with the distribution.
 
 THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 POSSIBILITY OF SUCH DAMAGE.
 */

#import "Reachability.h"

#import <Network/Network.h>
#import <arpa/inet.h>
#import <ifaddrs.h>
#import <net/if.h>
#import <netdb.h>
#import <netinet/in.h>
#import <sys/socket.h>

NSString *const kReachabilityChangedNotification = @"kReachabilityChangedNotification";

@interface Reachability ()

@property (nonatomic, assign) SCNetworkReachabilityRef reachabilityRef;
@property (nonatomic, strong) dispatch_queue_t reachabilitySerialQueue;
@property (nonatomic, strong) id reachabilityObject;
@property (nonatomic, strong) NSString *hostname;
@property (nonatomic, strong) NSData *hostAddressData;
@property (nonatomic, assign) BOOL localWiFiOnly;
@property (nonatomic, strong) nw_path_monitor_t pathMonitor;
@property (nonatomic, strong) nw_path_t currentPath;

- (instancetype)initWithHostname:(NSString *)hostname;
- (instancetype)initWithHostAddress:(const struct sockaddr *)hostAddress;
- (instancetype)initForLocalWiFi;
- (void)commonInit;
- (void)reachabilityChanged:(SCNetworkReachabilityFlags)flags;
- (BOOL)isReachableWithFlags:(SCNetworkReachabilityFlags)flags;

@end

static NSString *reachabilityFlags(SCNetworkReachabilityFlags flags)
{
    return [NSString stringWithFormat:@"%c%c %c%c%c%c%c%c%c",
#if TARGET_OS_IPHONE
            (flags & kSCNetworkReachabilityFlagsIsWWAN)               ? 'W' : '-',
#else
            'X',
#endif
            (flags & kSCNetworkReachabilityFlagsReachable)            ? 'R' : '-',
            (flags & kSCNetworkReachabilityFlagsConnectionRequired)   ? 'c' : '-',
            (flags & kSCNetworkReachabilityFlagsTransientConnection)  ? 't' : '-',
            (flags & kSCNetworkReachabilityFlagsInterventionRequired) ? 'i' : '-',
            '-',
            (flags & kSCNetworkReachabilityFlagsConnectionOnDemand)   ? 'D' : '-',
            (flags & kSCNetworkReachabilityFlagsIsLocalAddress)       ? 'l' : '-',
            (flags & kSCNetworkReachabilityFlagsIsDirect)             ? 'd' : '-'];
}

static BOOL FRGInterfaceIsCellular(const char *name)
{
#if TARGET_OS_IPHONE
    return strncmp(name, "pdp_ip", 6) == 0 || strncmp(name, "ipsec", 5) == 0;
#else
    return NO;
#endif
}

static BOOL FRGInterfaceIsWiFi(const char *name)
{
    return strncmp(name, "en", 2) == 0 || strncmp(name, "awdl", 4) == 0 || strncmp(name, "llw", 3) == 0;
}

static SCNetworkReachabilityFlags FRGReachabilityFlagsFromInterfaces(BOOL localWiFiOnly)
{
    struct ifaddrs *interfaces = NULL;
    if (getifaddrs(&interfaces) != 0) {
        return 0;
    }
    
    BOOL foundReachableInterface = NO;
    BOOL foundWiFiInterface = NO;
    BOOL foundCellularInterface = NO;
    BOOL foundLocalInterface = NO;
    
    for (struct ifaddrs *interface = interfaces; interface != NULL; interface = interface->ifa_next) {
        if (interface->ifa_addr == NULL) {
            continue;
        }
        
        int family = interface->ifa_addr->sa_family;
        if (family != AF_INET && family != AF_INET6) {
            continue;
        }
        
        unsigned int flags = interface->ifa_flags;
        BOOL isUsable = (flags & IFF_UP) && (flags & IFF_RUNNING) && !(flags & IFF_LOOPBACK);
        if (!isUsable) {
            continue;
        }
        
        BOOL isWiFi = FRGInterfaceIsWiFi(interface->ifa_name);
        if (localWiFiOnly && !isWiFi) {
            continue;
        }
        
        foundReachableInterface = YES;
        foundWiFiInterface = foundWiFiInterface || isWiFi;
        foundCellularInterface = foundCellularInterface || FRGInterfaceIsCellular(interface->ifa_name);
        
        if (family == AF_INET) {
            const struct sockaddr_in *address = (const struct sockaddr_in *)interface->ifa_addr;
            foundLocalInterface = foundLocalInterface || ((ntohl(address->sin_addr.s_addr) & IN_CLASSB_NET) == IN_LINKLOCALNETNUM);
        }
        else if (family == AF_INET6) {
            const struct sockaddr_in6 *address = (const struct sockaddr_in6 *)interface->ifa_addr;
            foundLocalInterface = foundLocalInterface || IN6_IS_ADDR_LINKLOCAL(&address->sin6_addr);
        }
    }
    
    freeifaddrs(interfaces);
    
    SCNetworkReachabilityFlags flags = 0;
    if (foundReachableInterface) {
        flags |= kSCNetworkReachabilityFlagsReachable;
    }
    if (foundLocalInterface || localWiFiOnly) {
        flags |= kSCNetworkReachabilityFlagsIsLocalAddress;
        flags |= kSCNetworkReachabilityFlagsIsDirect;
    }
#if TARGET_OS_IPHONE
    if (foundCellularInterface && !foundWiFiInterface) {
        flags |= kSCNetworkReachabilityFlagsIsWWAN;
    }
#endif
    
    return flags;
}

static SCNetworkReachabilityFlags FRGReachabilityFlagsFromPath(nw_path_t path, BOOL localWiFiOnly)
{
    if (path == nil) {
        return FRGReachabilityFlagsFromInterfaces(localWiFiOnly);
    }
    
    SCNetworkReachabilityFlags flags = 0;
    nw_path_status_t status = nw_path_get_status(path);
    if (status == nw_path_status_satisfied || status == nw_path_status_satisfiable) {
        flags |= kSCNetworkReachabilityFlagsReachable;
    }
    if (status == nw_path_status_satisfiable) {
        flags |= kSCNetworkReachabilityFlagsConnectionRequired;
        flags |= kSCNetworkReachabilityFlagsConnectionOnDemand;
    }
    if (localWiFiOnly) {
        flags |= kSCNetworkReachabilityFlagsIsLocalAddress;
        flags |= kSCNetworkReachabilityFlagsIsDirect;
    }
#if TARGET_OS_IPHONE
    if (nw_path_uses_interface_type(path, nw_interface_type_cellular)) {
        flags |= kSCNetworkReachabilityFlagsIsWWAN;
    }
#endif
    
    return flags;
}

@implementation Reachability

#pragma mark - Class Constructor Methods

+ (instancetype)reachabilityWithHostName:(NSString *)hostname
{
    return [self reachabilityWithHostname:hostname];
}

+ (instancetype)reachabilityWithHostname:(NSString *)hostname
{
    return [[self alloc] initWithHostname:hostname];
}

+ (instancetype)reachabilityWithAddress:(void *)hostAddress
{
    if (hostAddress == NULL) {
        return nil;
    }
    
    return [[self alloc] initWithHostAddress:(const struct sockaddr *)hostAddress];
}

+ (instancetype)reachabilityForInternetConnection
{
    struct sockaddr_in zeroAddress;
    memset(&zeroAddress, 0, sizeof(zeroAddress));
    zeroAddress.sin_len = sizeof(zeroAddress);
    zeroAddress.sin_family = AF_INET;
    
    return [self reachabilityWithAddress:&zeroAddress];
}

+ (instancetype)reachabilityForLocalWiFi
{
    return [[self alloc] initForLocalWiFi];
}

- (instancetype)initWithHostname:(NSString *)hostname
{
    self = [super init];
    if (self != nil) {
        [self commonInit];
        self.hostname = hostname;
    }
    
    return self;
}

- (instancetype)initWithHostAddress:(const struct sockaddr *)hostAddress
{
    self = [super init];
    if (self != nil) {
        [self commonInit];
        if (hostAddress != NULL) {
            self.hostAddressData = [NSData dataWithBytes:hostAddress length:hostAddress->sa_len];
        }
    }
    
    return self;
}

- (instancetype)initForLocalWiFi
{
    self = [super init];
    if (self != nil) {
        [self commonInit];
        self.localWiFiOnly = YES;
    }
    
    return self;
}

- (instancetype)initWithReachabilityRef:(SCNetworkReachabilityRef)ref
{
    self = [super init];
    if (self != nil) {
        [self commonInit];
        self.reachabilityRef = ref;
    }
    
    return self;
}

- (void)commonInit
{
    self.reachableOnWWAN = YES;
    self.reachabilitySerialQueue = dispatch_queue_create("com.tonymillion.reachability", DISPATCH_QUEUE_SERIAL);
}

- (void)dealloc
{
    [self stopNotifier];
    
    if (self.reachabilityRef) {
        CFRelease(self.reachabilityRef);
        self.reachabilityRef = NULL;
    }
    
    self.reachableBlock = nil;
    self.unreachableBlock = nil;
    self.reachabilitySerialQueue = nil;
}

#pragma mark - Notifier Methods

- (BOOL)startNotifier
{
    if (self.reachabilityObject == self && self.pathMonitor != nil) {
        return YES;
    }
    
    nw_path_monitor_t monitor = self.localWiFiOnly ? nw_path_monitor_create_with_type(nw_interface_type_wifi) : nw_path_monitor_create();
    if (monitor == nil) {
        self.reachabilityObject = nil;
        return NO;
    }
    
    __weak typeof(self) weakSelf = self;
    nw_path_monitor_set_update_handler(monitor, ^(nw_path_t path) {
        __strong typeof(weakSelf) strongSelf = weakSelf;
        if (strongSelf == nil) {
            return;
        }
        
        strongSelf.currentPath = path;
        [strongSelf reachabilityChanged:FRGReachabilityFlagsFromPath(path, strongSelf.localWiFiOnly)];
    });
    nw_path_monitor_set_queue(monitor, self.reachabilitySerialQueue);
    nw_path_monitor_start(monitor);
    
    self.pathMonitor = monitor;
    self.reachabilityObject = self;
    return YES;
}

- (void)stopNotifier
{
    if (self.pathMonitor != nil) {
        nw_path_monitor_cancel(self.pathMonitor);
        self.pathMonitor = nil;
    }
    
    self.reachabilityObject = nil;
}

#pragma mark - reachability tests

#define testcase (kSCNetworkReachabilityFlagsConnectionRequired | kSCNetworkReachabilityFlagsTransientConnection)

- (BOOL)isReachableWithFlags:(SCNetworkReachabilityFlags)flags
{
    BOOL connectionUP = YES;
    
    if (!(flags & kSCNetworkReachabilityFlagsReachable)) {
        connectionUP = NO;
    }
    
    if ((flags & testcase) == testcase) {
        connectionUP = NO;
    }
    
#if TARGET_OS_IPHONE
    if (flags & kSCNetworkReachabilityFlagsIsWWAN) {
        if (!self.reachableOnWWAN) {
            connectionUP = NO;
        }
    }
#endif
    
    return connectionUP;
}

- (BOOL)isReachable
{
    return [self isReachableWithFlags:[self reachabilityFlags]];
}

- (BOOL)isReachableViaWWAN
{
#if TARGET_OS_IPHONE
    SCNetworkReachabilityFlags flags = [self reachabilityFlags];
    return (flags & kSCNetworkReachabilityFlagsReachable) && (flags & kSCNetworkReachabilityFlagsIsWWAN);
#else
    return NO;
#endif
}

- (BOOL)isReachableViaWiFi
{
    SCNetworkReachabilityFlags flags = [self reachabilityFlags];
    if (!(flags & kSCNetworkReachabilityFlagsReachable)) {
        return NO;
    }
    
#if TARGET_OS_IPHONE
    if (flags & kSCNetworkReachabilityFlagsIsWWAN) {
        return NO;
    }
#endif
    
    return YES;
}

- (BOOL)isConnectionRequired
{
    return [self connectionRequired];
}

- (BOOL)connectionRequired
{
    return ([self reachabilityFlags] & kSCNetworkReachabilityFlagsConnectionRequired) != 0;
}

- (BOOL)isConnectionOnDemand
{
    SCNetworkReachabilityFlags flags = [self reachabilityFlags];
    return ((flags & kSCNetworkReachabilityFlagsConnectionRequired) &&
            (flags & kSCNetworkReachabilityFlagsConnectionOnDemand));
}

- (BOOL)isInterventionRequired
{
    SCNetworkReachabilityFlags flags = [self reachabilityFlags];
    return ((flags & kSCNetworkReachabilityFlagsConnectionRequired) &&
            (flags & kSCNetworkReachabilityFlagsInterventionRequired));
}

#pragma mark - reachability status stuff

- (NetworkStatus)currentReachabilityStatus
{
    if ([self isReachable]) {
        if ([self isReachableViaWiFi]) {
            return ReachableViaWiFi;
        }
        
#if TARGET_OS_IPHONE
        return ReachableViaWWAN;
#endif
    }
    
    return NotReachable;
}

- (SCNetworkReachabilityFlags)reachabilityFlags
{
    if (self.currentPath != nil) {
        return FRGReachabilityFlagsFromPath(self.currentPath, self.localWiFiOnly);
    }
    
    return FRGReachabilityFlagsFromInterfaces(self.localWiFiOnly);
}

- (NSString *)currentReachabilityString
{
    NetworkStatus temp = [self currentReachabilityStatus];
    
    if (temp == ReachableViaWWAN) {
        return NSLocalizedString(@"Cellular", @"");
    }
    if (temp == ReachableViaWiFi) {
        return NSLocalizedString(@"WiFi", @"");
    }
    
    return NSLocalizedString(@"No Connection", @"");
}

- (NSString *)currentReachabilityFlags
{
    return reachabilityFlags([self reachabilityFlags]);
}

#pragma mark - Callback function calls this method

- (void)reachabilityChanged:(SCNetworkReachabilityFlags)flags
{
    if ([self isReachableWithFlags:flags]) {
        if (self.reachableBlock) {
            self.reachableBlock(self);
        }
    }
    else {
        if (self.unreachableBlock) {
            self.unreachableBlock(self);
        }
    }
    
    dispatch_async(dispatch_get_main_queue(), ^{
        [[NSNotificationCenter defaultCenter] postNotificationName:kReachabilityChangedNotification
                                                            object:self];
    });
}

#pragma mark - Debug Description

- (NSString *)description
{
    NSString *description = [NSString stringWithFormat:@"<%@: %@ (%@)>",
                             NSStringFromClass([self class]), self, [self currentReachabilityFlags]];
    return description;
}

@end
