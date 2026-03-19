//
// The functions here are replacments for deprecated C++ code.
//
// For more info see:
// https://web.archive.org/web/20101203170217/http://developer.apple.com/library/mac/#documentation/Cocoa/Conceptual/ObjectiveC/Articles/ocCPlusPlus.html
//
//

class DeprecationSupport
{
private:
    
    
public:
    
    DeprecationSupport();
    
  //  void say_hello(CFURLRef url);
    static CFStringRef stringByAddingPercentEncodingWithAllowedCharacters(CFStringRef path);
    
};
