//
//
//
#include "DeprecationSupport.h"

DeprecationSupport::DeprecationSupport()
{
    
}

/*
void Hello::say_hello(CFURLRef url)
{
    printf("HELOO\n");
    
    NSURLSessionConfiguration* config = [NSURLSessionConfiguration defaultSessionConfiguration];
    [config setTimeoutIntervalForRequest:5];
    [config setTimeoutIntervalForResource:5];
    NSURLSession* urlsession = [NSURLSession sessionWithConfiguration:config];
    NSURLSessionDataTask* task = [urlsession dataTaskWithURL:(__bridge NSURL*)url
                                           completionHandler:^(NSData * _Nullable data, NSURLResponse * _Nullable response, NSError * _Nullable error)
                                  {
        printf("HERE");
    }];
    
    [task resume];
    
}
 */

CFStringRef DeprecationSupport::stringByAddingPercentEncodingWithAllowedCharacters(CFStringRef path)
{
    NSString* nspath = (__bridge NSString *)path;
    
    NSString* newpath = [nspath stringByAddingPercentEncodingWithAllowedCharacters:NSCharacterSet.URLQueryAllowedCharacterSet];
    
    return (__bridge CFStringRef)newpath;
}
