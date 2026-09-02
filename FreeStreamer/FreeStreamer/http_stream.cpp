/*
 * This file is part of the FreeStreamer project,
 * (C)Copyright 2011-2018 Matias Muhonen <mmu@iki.fi> 穆马帝
 * See the file ''LICENSE'' for using the code.
 *
 * https://github.com/muhku/FreeStreamer
 */

#include "http_stream.h"
#include "audio_queue.h"
#include "id3_parser.h"
#include "stream_configuration.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>

//#define HS_DEBUG 1

#if !defined (HS_DEBUG)
#define HS_TRACE(...) do {} while (0)
#define HS_TRACE_CFSTRING(X) do {} while (0)
#else
#define HS_TRACE(...) printf(__VA_ARGS__)
#define HS_TRACE_CFSTRING(X) HS_TRACE("%s\n", CFStringGetCStringPtr(X, kCFStringEncodingMacRoman))
#endif

/*
 * Comment the following line to disable ID3 tag support:
 */
#define INCLUDE_ID3TAG_SUPPORT 1

namespace astreamer {

static bool appendCFStringToUTF8String(std::string& destination, CFStringRef source)
{
    if (!source) {
        return false;
    }

    CFIndex length = CFStringGetLength(source);
    CFIndex maxBytes = CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8) + 1;
    if (maxBytes <= 0) {
        return false;
    }

    std::vector<char> buffer(maxBytes);
    if (!CFStringGetCString(source, &buffer[0], maxBytes, kCFStringEncodingUTF8)) {
        return false;
    }

    destination.append(&buffer[0]);
    return true;
}

static std::string stringByTrimmingHTTPWhitespace(const std::string& value)
{
    std::string::size_type first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return std::string();
    }

    std::string::size_type last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

static std::string lowercaseString(const std::string& value)
{
    std::string lowered(value);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return lowered;
}

static bool stringHasPrefix(const std::string& value, const char *prefix)
{
    return value.compare(0, strlen(prefix), prefix) == 0;
}

static CFStringRef createHeaderValueString(const std::string& value)
{
    CFStringRef string = CFStringCreateWithBytes(kCFAllocatorDefault,
                                                 reinterpret_cast<const UInt8 *>(value.data()),
                                                 value.length(),
                                                 kCFStringEncodingUTF8,
                                                 false);
    if (string) {
        return string;
    }

    return CFStringCreateWithBytes(kCFAllocatorDefault,
                                   reinterpret_cast<const UInt8 *>(value.data()),
                                   value.length(),
                                   kCFStringEncodingISOLatin1,
                                   false);
}

/* HTTP_Stream: public */
HTTP_Stream::HTTP_Stream() :
    m_readStream(0),
    m_writeStream(0),
    m_scheduledInRunLoop(false),
    m_readPending(false),
    m_url(0),
    m_httpHeadersParsed(false),
    m_contentType(0),
    m_contentLength(0),
    m_bytesRead(0),
    
    m_icyStream(false),
    m_icyHeaderCR(false),
    m_icyHeadersRead(false),
    m_icyHeadersParsed(false),
    
    m_icyName(0),
    
    m_icyMetaDataInterval(0),
    m_dataByteReadCount(0),
    m_metaDataBytesRemaining(0),
    
    m_httpReadBuffer(0),
    m_icyReadBuffer(0),
    
    m_id3Parser(new ID3_Parser())
{
    m_id3Parser->m_delegate = this;
}

HTTP_Stream::~HTTP_Stream()
{
    close();
    
    for (std::vector<CFStringRef>::iterator h = m_icyHeaderLines.begin(); h != m_icyHeaderLines.end(); ++h) {
        CFRelease(*h);
    }
    
    m_icyHeaderLines.clear();
    
    if (m_contentType) {
        CFRelease(m_contentType);
        m_contentType = 0;
    }
    
    if (m_icyName) {
        CFRelease(m_icyName);
        m_icyName = 0;
    }
    
    if (m_httpReadBuffer) {
        delete [] m_httpReadBuffer;
        m_httpReadBuffer = 0;
    }
    if (m_icyReadBuffer) {
        delete [] m_icyReadBuffer;
        m_icyReadBuffer = 0;
    }
    if (m_url) {
        CFRelease(m_url);
        m_url = 0;
    }
    
    delete m_id3Parser;
    m_id3Parser = 0;
}
    
Input_Stream_Position HTTP_Stream::position()
{
    return m_position;
}
    
CFStringRef HTTP_Stream::contentType()
{
    return m_contentType;
}
    
size_t HTTP_Stream::contentLength()
{
    return m_contentLength;
}
    
bool HTTP_Stream::open()
{
    Input_Stream_Position position;
    position.start = 0;
    position.end = 0;
    
    m_contentLength = 0;
#ifdef INCLUDE_ID3TAG_SUPPORT
    m_id3Parser->reset();
#endif
    
    return open(position);
}

bool HTTP_Stream::open(const Input_Stream_Position& position)
{
    bool success = false;
    CFStreamClientContext CTX = { 0, this, NULL, NULL, NULL };
    
    /* Already opened a read stream, return */
    if (m_readStream) {
        goto out;
    }
    
    /* Reset state */
    m_position = position;
    
    m_readPending = false;
    m_httpHeadersParsed = false;
    m_httpHeaderData.clear();
    
    if (m_contentType) {
        CFRelease(m_contentType);
        m_contentType = NULL;
    }
    
    m_icyStream = false;
    m_icyHeaderCR = false;
    m_icyHeadersRead = false;
    m_icyHeadersParsed = false;
    
    if (m_icyName) {
        CFRelease(m_icyName);
        m_icyName = 0;
    }
    
    for (std::vector<CFStringRef>::iterator h = m_icyHeaderLines.begin(); h != m_icyHeaderLines.end(); ++h) {
        CFRelease(*h);
    }
    
    m_icyHeaderLines.clear();
    m_icyMetaDataInterval = 0;
    m_dataByteReadCount = 0;
    m_metaDataBytesRemaining = 0;
    m_bytesRead = 0;
    
    if (!m_url) {
        goto out;
    }
	
    /* Failed to create a stream */
    if (!(m_readStream = createReadStream(m_url))) {
        goto out;
    }
    
    if (!CFReadStreamSetClient(m_readStream, kCFStreamEventHasBytesAvailable |
	                                         kCFStreamEventEndEncountered |
	                                         kCFStreamEventErrorOccurred, readCallBack, &CTX)) {
        CFRelease(m_readStream);
        m_readStream = 0;
        goto out;
    }
    
    setScheduledInRunLoop(true);
    
    if (!CFReadStreamOpen(m_readStream)) {
        /* Open failed: clean */
        CFReadStreamSetClient(m_readStream, 0, NULL, NULL);
        setScheduledInRunLoop(false);
        if (m_readStream) {
            CFRelease(m_readStream);
            m_readStream = 0;
        }
        goto out;
    }
    
    success = true;

out:
    return success;
}

void HTTP_Stream::close()
{
    /* The stream has been already closed */
    if (!m_readStream) {
        return;
    }
    
    CFReadStreamSetClient(m_readStream, 0, NULL, NULL);
    setScheduledInRunLoop(false);
    CFReadStreamClose(m_readStream);
    CFRelease(m_readStream);
    m_readStream = 0;

    if (m_writeStream) {
        CFWriteStreamClose(m_writeStream);
        CFRelease(m_writeStream);
        m_writeStream = 0;
    }
}
    
void HTTP_Stream::setScheduledInRunLoop(bool scheduledInRunLoop)
{
    /* The stream has not been opened, or it has been already closed */
    if (!m_readStream) {
        return;
    }
    
    /* The state doesn't change */
    if (m_scheduledInRunLoop == scheduledInRunLoop) {
        return;
    }
    
    if (m_scheduledInRunLoop) {
        CFReadStreamUnscheduleFromRunLoop(m_readStream, CFRunLoopGetCurrent(), kCFRunLoopCommonModes);
    } else {
        if (m_readPending) {
            m_readPending = false;
            
            readCallBack(m_readStream, kCFStreamEventHasBytesAvailable, this);
        }
        
        CFReadStreamScheduleWithRunLoop(m_readStream, CFRunLoopGetCurrent(), kCFRunLoopCommonModes);
    }
    
    m_scheduledInRunLoop = scheduledInRunLoop;
}
    
void HTTP_Stream::setUrl(CFURLRef url)
{
    if (m_url) {
        CFRelease(m_url);
    }
    if (url) {
        m_url = (CFURLRef)CFRetain(url);
    } else {
        m_url = NULL;
    }
}
    
bool HTTP_Stream::canHandleUrl(CFURLRef url)
{
    if (!url) {
        return false;
    }
    
    CFStringRef scheme = CFURLCopyScheme(url);
    
    if (scheme) {
        if (CFStringCompare(scheme, CFSTR("file"), 0) == kCFCompareEqualTo) {
            CFRelease(scheme);
            
            // The only scheme we claim not to handle are local files.
            return false;
        }
        
        CFRelease(scheme);
    }
    
    return true;
}
    
void HTTP_Stream::id3metaDataAvailable(std::map<CFStringRef,CFStringRef> metaData)
{
    if (m_delegate) {
        m_delegate->streamMetaDataAvailable(metaData);
    }
}
    
void HTTP_Stream::id3tagSizeAvailable(UInt32 tagSize)
{
    if (m_delegate) {
        m_delegate->streamMetaDataByteSizeAvailable(tagSize);
    }
}

/* private */
    
CFReadStreamRef HTTP_Stream::createReadStream(CFURLRef url)
{
    CFReadStreamRef readStream = 0;
    CFWriteStreamRef writeStream = 0;
    CFStringRef host = CFURLCopyHostName(url);
    CFStringRef scheme = CFURLCopyScheme(url);
    SInt32 port = 0;
    bool usesTLS = false;

    if (!host || !scheme) {
        goto out;
    }

    port = CFURLGetPortNumber(url);
    usesTLS = CFStringCompare(scheme, CFSTR("https"), kCFCompareCaseInsensitive) == kCFCompareEqualTo;
    if (port <= 0) {
        port = usesTLS ? 443 : 80;
    }

    CFStreamCreatePairWithSocketToHost(kCFAllocatorDefault, host, port, &readStream, &writeStream);
    if (!readStream || !writeStream) {
        goto out;
    }

    CFReadStreamSetProperty(readStream, kCFStreamPropertyShouldCloseNativeSocket, kCFBooleanTrue);
    CFWriteStreamSetProperty(writeStream, kCFStreamPropertyShouldCloseNativeSocket, kCFBooleanTrue);

    if (usesTLS) {
        CFReadStreamSetProperty(readStream, kCFStreamPropertySocketSecurityLevel, kCFStreamSocketSecurityLevelNegotiatedSSL);
        CFWriteStreamSetProperty(writeStream, kCFStreamPropertySocketSecurityLevel, kCFStreamSocketSecurityLevelNegotiatedSSL);
    }

    if (!sendHTTPRequest(writeStream, url)) {
        goto out;
    }

    m_writeStream = writeStream;
    writeStream = 0;

out:
    if (host) {
        CFRelease(host);
    }
    if (scheme) {
        CFRelease(scheme);
    }
    if (writeStream) {
        CFWriteStreamClose(writeStream);
        CFRelease(writeStream);
    }
    if (!m_writeStream && readStream) {
        CFRelease(readStream);
        readStream = 0;
    }

    return readStream;
}

bool HTTP_Stream::sendHTTPRequest(CFWriteStreamRef writeStream, CFURLRef url)
{
    CFStringRef host = CFURLCopyHostName(url);
    CFStringRef path = CFURLCopyPath(url);
    CFStringRef query = CFURLCopyQueryString(url, NULL);
    CFStringRef scheme = CFURLCopyScheme(url);
    bool success = false;

    if (host && scheme) {
        std::string request;
        request += "GET ";
        if (path && CFStringGetLength(path) > 0) {
            appendCFStringToUTF8String(request, path);
        } else {
            request += "/";
        }
        if (query && CFStringGetLength(query) > 0) {
            request += "?";
            appendCFStringToUTF8String(request, query);
        }
        request += " HTTP/1.1\r\nHost: ";
        appendCFStringToUTF8String(request, host);

        SInt32 port = CFURLGetPortNumber(url);
        const bool usesTLS = CFStringCompare(scheme, CFSTR("https"), kCFCompareCaseInsensitive) == kCFCompareEqualTo;
        if ((port > 0) && ((usesTLS && port != 443) || (!usesTLS && port != 80))) {
            char portBuffer[16];
            snprintf(portBuffer, sizeof(portBuffer), ":%d", static_cast<int>(port));
            request += portBuffer;
        }
        request += "\r\n";

        Stream_Configuration *config = Stream_Configuration::configuration();
        if (config->userAgent) {
            request += "User-Agent: ";
            appendCFStringToUTF8String(request, config->userAgent);
            request += "\r\n";
        }

        request += "Icy-MetaData: 1\r\n";

        if (m_position.start > 0 && m_position.end > m_position.start) {
            char rangeHeader[64];
            snprintf(rangeHeader, sizeof(rangeHeader), "Range: bytes=%llu-%llu\r\n", m_position.start, m_position.end);
            request += rangeHeader;
        } else if (m_position.start > 0 && m_position.end < m_position.start) {
            char rangeHeader[64];
            snprintf(rangeHeader, sizeof(rangeHeader), "Range: bytes=%llu-\r\n", m_position.start);
            request += rangeHeader;
        }

        if (config->predefinedHttpHeaderValues) {
            const CFIndex numKeys = CFDictionaryGetCount(config->predefinedHttpHeaderValues);

            if (numKeys > 0) {
                CFTypeRef *keys = (CFTypeRef *) malloc(numKeys * sizeof(CFTypeRef));

                if (keys) {
                    CFDictionaryGetKeysAndValues(config->predefinedHttpHeaderValues, (const void **) keys, NULL);

                    for (CFIndex i=0; i < numKeys; i++) {
                        CFTypeRef key = keys[i];

                        if (CFGetTypeID(key) == CFStringGetTypeID()) {
                            const void *value = CFDictionaryGetValue(config->predefinedHttpHeaderValues, (const void *) key);

                            if (value) {
                                CFTypeRef valueRef = (CFTypeRef) value;

                                if (CFGetTypeID(valueRef) == CFStringGetTypeID()) {
                                    appendCFStringToUTF8String(request, (CFStringRef) key);
                                    request += ": ";
                                    appendCFStringToUTF8String(request, (CFStringRef) valueRef);
                                    request += "\r\n";
                                }
                            }
                        }
                    }

                    free(keys);
                }
            }
        }

        request += "Connection: close\r\n\r\n";

        if (CFWriteStreamOpen(writeStream)) {
            const UInt8 *bytes = reinterpret_cast<const UInt8 *>(request.data());
            CFIndex bytesRemaining = request.length();
            success = true;

            while (bytesRemaining > 0) {
                CFIndex bytesWritten = CFWriteStreamWrite(writeStream, bytes, bytesRemaining);
                if (bytesWritten <= 0) {
                    success = false;
                    break;
                }
                bytes += bytesWritten;
                bytesRemaining -= bytesWritten;
            }
        }
    }

    if (host) {
        CFRelease(host);
    }
    if (path) {
        CFRelease(path);
    }
    if (query) {
        CFRelease(query);
    }
    if (scheme) {
        CFRelease(scheme);
    }
    return success;
}
    
bool HTTP_Stream::parseHttpHeadersIfNeeded(const UInt8 *buf, const CFIndex bufSize, CFIndex *bodyOffset)
{
    if (bodyOffset) {
        *bodyOffset = 0;
    }

    if (m_httpHeadersParsed) {
        return true;
    }

    const size_t previousSize = m_httpHeaderData.size();
    m_httpHeaderData.insert(m_httpHeaderData.end(), buf, buf + bufSize);

    if (m_httpHeaderData.size() >= 10 &&
        m_httpHeaderData[0] == 0x49 && m_httpHeaderData[1] == 0x43 && m_httpHeaderData[2] == 0x59 &&
        m_httpHeaderData[3] == 0x20 && m_httpHeaderData[4] == 0x32 && m_httpHeaderData[5] == 0x30 &&
        m_httpHeaderData[6] == 0x30 && m_httpHeaderData[7] == 0x20 && m_httpHeaderData[8] == 0x4F &&
        m_httpHeaderData[9] == 0x4B) {
        m_httpHeadersParsed = true;
        m_icyStream = true;
        m_httpHeaderData.clear();

        HS_TRACE("Detected an IceCast stream\n");
        return true;
    }

    if (m_httpHeaderData.size() < 4) {
        return false;
    }

    size_t headerEnd = std::string::npos;
    size_t delimiterLength = 0;
    for (size_t i = 0; i + 3 < m_httpHeaderData.size(); ++i) {
        if (m_httpHeaderData[i] == '\r' && m_httpHeaderData[i + 1] == '\n' &&
            m_httpHeaderData[i + 2] == '\r' && m_httpHeaderData[i + 3] == '\n') {
            headerEnd = i;
            delimiterLength = 4;
            break;
        }
    }

    if (headerEnd == std::string::npos) {
        for (size_t i = 0; i + 1 < m_httpHeaderData.size(); ++i) {
            if (m_httpHeaderData[i] == '\n' && m_httpHeaderData[i + 1] == '\n') {
                headerEnd = i;
                delimiterLength = 2;
                break;
            }
        }
    }

    if (headerEnd == std::string::npos) {
        return false;
    }

    m_httpHeadersParsed = true;
    HS_TRACE("A regular HTTP stream\n");

    std::string headers(reinterpret_cast<const char *>(&m_httpHeaderData[0]), headerEnd);
    CFIndex statusCode = 0;

    size_t lineStart = 0;
    bool firstLine = true;
    while (lineStart <= headers.length()) {
        size_t lineEnd = headers.find('\n', lineStart);
        if (lineEnd == std::string::npos) {
            lineEnd = headers.length();
        }

        std::string line = stringByTrimmingHTTPWhitespace(headers.substr(lineStart, lineEnd - lineStart));
        if (!line.empty()) {
            if (firstLine) {
                if (stringHasPrefix(line, "HTTP/")) {
                    size_t statusStart = line.find(' ');
                    if (statusStart != std::string::npos) {
                        statusCode = static_cast<CFIndex>(strtol(line.c_str() + statusStart + 1, NULL, 10));
                    }
                }
                firstLine = false;
            } else {
                size_t separator = line.find(':');
                if (separator != std::string::npos) {
                    std::string fieldName = lowercaseString(stringByTrimmingHTTPWhitespace(line.substr(0, separator)));
                    std::string fieldValue = stringByTrimmingHTTPWhitespace(line.substr(separator + 1));

                    if (fieldName == "icy-metaint") {
                        m_icyStream = true;
                        m_icyHeadersParsed = true;
                        m_icyHeadersRead = true;
                        m_icyMetaDataInterval = strtoull(fieldValue.c_str(), NULL, 10);
                    } else if (fieldName == "icy-name") {
                        if (m_icyName) {
                            CFRelease(m_icyName);
                        }
                        m_icyName = createHeaderValueString(fieldValue);

                        if (m_delegate && m_icyName) {
                            std::map<CFStringRef,CFStringRef> metadataMap;
                            metadataMap[CFSTR("IcecastStationName")] = CFStringCreateCopy(kCFAllocatorDefault, m_icyName);
                            m_delegate->streamMetaDataAvailable(metadataMap);
                        }
                    } else if (fieldName == "content-type") {
                        if (m_contentType) {
                            CFRelease(m_contentType);
                        }
                        m_contentType = createHeaderValueString(fieldValue);
                    } else if (fieldName == "content-length") {
                        m_contentLength = strtoull(fieldValue.c_str(), NULL, 10);
                    }
                }
            }
        }

        if (lineEnd == headers.length()) {
            break;
        }
        lineStart = lineEnd + 1;
    }

    HS_TRACE("icy-metaint: %zu\n", m_icyMetaDataInterval);
    HS_TRACE("HTTP response code %zu", statusCode);

    if (m_delegate && (statusCode == 200 || statusCode == 206)) {
        m_delegate->streamIsReadyRead();
    } else if (m_delegate) {
        CFStringRef statusCodeString = CFStringCreateWithFormat(NULL,
                                                                NULL,
                                                                CFSTR("HTTP response code %d"),
                                                                (unsigned int)statusCode);
        m_delegate->streamErrorOccurred(statusCodeString);

        if (statusCodeString) {
            CFRelease(statusCodeString);
        }
    }

    size_t bodyStart = headerEnd + delimiterLength;
    if (bodyOffset && bodyStart > previousSize) {
        *bodyOffset = static_cast<CFIndex>(bodyStart - previousSize);
    }

    m_httpHeaderData.clear();
    return true;
}
    
void HTTP_Stream::parseICYStream(const UInt8 *buf, const CFIndex bufSize)
{
    HS_TRACE("Parsing an IceCast stream, received %li bytes\n", bufSize);
    
    CFIndex offset = 0;
    CFIndex bytesFound = 0;
    if (!m_icyHeadersRead) {
        HS_TRACE("ICY headers not read, reading\n");
        
        for (; offset < bufSize; offset++) {
            if (m_icyHeaderCR && buf[offset] == '\n') {
                if (bytesFound > 0) {
                    m_icyHeaderLines.push_back(createMetaDataStringWithMostReasonableEncoding(&buf[offset-bytesFound-1], bytesFound));
                    
                    bytesFound = 0;
                    
                    HS_TRACE_CFSTRING(m_icyHeaderLines[m_icyHeaderLines.size()-1]);
                    
                    continue;
                }
                
                HS_TRACE("End of ICY headers\n");
                
                m_icyHeadersRead = true;
                break;
            }
            
            if (buf[offset] == '\r') {
                m_icyHeaderCR = true;
                continue;
            } else {
                m_icyHeaderCR = false;
            }
            
            bytesFound++;
        }
    } else if (!m_icyHeadersParsed) {
        HS_TRACE("ICY headers not parsed, parsing\n");
        
        const CFStringRef icyContentTypeHeader = CFSTR("content-type:");
        const CFStringRef icyMetaDataHeader    =  CFSTR("icy-metaint:");
        const CFStringRef icyNameHeader        = CFSTR("icy-name:");

        const CFIndex icyContenTypeHeaderLength = CFStringGetLength(icyContentTypeHeader);
        const CFIndex icyMetaDataHeaderLength   = CFStringGetLength(icyMetaDataHeader);
        const CFIndex icyNameHeaderLength       = CFStringGetLength(icyNameHeader);
        
        for (std::vector<CFStringRef>::iterator h = m_icyHeaderLines.begin(); h != m_icyHeaderLines.end(); ++h) {
            CFStringRef line = *h;
            const CFIndex lineLength = CFStringGetLength(line);
            
            if (lineLength == 0) {
                continue;
            }
            
            HS_TRACE_CFSTRING(line);
            
            if (CFStringCompareWithOptions(line,
                                           icyContentTypeHeader,
                                           CFRangeMake(0, icyContenTypeHeaderLength),
                                           0) == kCFCompareEqualTo) {
                if (m_contentType) {
                    CFRelease(m_contentType);
                    m_contentType = 0;
                }
                m_contentType = CFStringCreateWithSubstring(kCFAllocatorDefault,
                                                            line,
                                                            CFRangeMake(icyContenTypeHeaderLength, lineLength - icyContenTypeHeaderLength));
                
            }
            
            if (CFStringCompareWithOptions(line,
                                           icyMetaDataHeader,
                                           CFRangeMake(0, icyMetaDataHeaderLength),
                                           0) == kCFCompareEqualTo) {
                CFStringRef metadataInterval = CFStringCreateWithSubstring(kCFAllocatorDefault,
                                                                           line,
                                                                           CFRangeMake(icyMetaDataHeaderLength, lineLength - icyMetaDataHeaderLength));
                
                if (metadataInterval) {
                    m_icyMetaDataInterval = CFStringGetIntValue(metadataInterval);
                    
                    CFRelease(metadataInterval);
                } else {
                    m_icyMetaDataInterval = 0;
                }
            }
            
            if (CFStringCompareWithOptions(line,
                                           icyNameHeader,
                                           CFRangeMake(0, icyNameHeaderLength),
                                           0) == kCFCompareEqualTo) {
                if (m_icyName) {
                    CFRelease(m_icyName);
                }
                
                m_icyName = CFStringCreateWithSubstring(kCFAllocatorDefault,
                                                        line,
                                                        CFRangeMake(icyNameHeaderLength, lineLength - icyNameHeaderLength));
            }
        }
        
        m_icyHeadersParsed = true;
        offset++;
        
        if (m_delegate) {
            m_delegate->streamIsReadyRead();
        }
    }
    
    Stream_Configuration *config = Stream_Configuration::configuration();
    
    if (!m_icyReadBuffer) {
        m_icyReadBuffer = new UInt8[config->httpConnectionBufferSize];
    }
    
    HS_TRACE("Reading ICY stream for playback\n");
    
    UInt32 i=0;
    
    for (; offset < bufSize; offset++) {
        // is this a metadata byte?
        if (m_metaDataBytesRemaining > 0) {
            m_metaDataBytesRemaining--;
            
            if (m_metaDataBytesRemaining == 0) {
                m_dataByteReadCount = 0;
                
                if (m_delegate && !m_icyMetaData.empty()) {
                    std::map<CFStringRef,CFStringRef> metadataMap;
                    
                    CFStringRef metaData = createMetaDataStringWithMostReasonableEncoding(&m_icyMetaData[0],
                                                                                          m_icyMetaData.size());
                    
                    if (!metaData) {
                        // Metadata encoding failed, cannot parse.
                        m_icyMetaData.clear();
                        continue;
                    }
                    
                    CFArrayRef tokens = CFStringCreateArrayBySeparatingStrings(kCFAllocatorDefault,
                                                                               metaData,
                                                                               CFSTR(";"));
                    
                    for (CFIndex i=0, max=CFArrayGetCount(tokens); i < max; i++) {
                        CFStringRef token = (CFStringRef) CFArrayGetValueAtIndex(tokens, i);
                        
                        CFRange foundRange;
                        
                        if (CFStringFindWithOptions(token,
                                                    CFSTR("='"),
                                                    CFRangeMake(0, CFStringGetLength(token)),
                                                    NULL,
                                                    &foundRange) == true) {
                            
                            CFRange keyRange = CFRangeMake(0, foundRange.location);
                            
                            CFStringRef metadaKey = CFStringCreateWithSubstring(kCFAllocatorDefault,
                                                                                token,
                                                                                keyRange);
                            
                            CFRange valueRange = CFRangeMake(foundRange.location + 2, CFStringGetLength(token) - keyRange.length - 3);
                            
                            CFStringRef metadaValue = CFStringCreateWithSubstring(kCFAllocatorDefault,
                                                                                  token,
                                                                                  valueRange);
                            
                            metadataMap[metadaKey] = metadaValue;
                        }
                    }
                    
                    CFRelease(tokens);
                    CFRelease(metaData);
                    
                    if (m_icyName) {
                        metadataMap[CFSTR("IcecastStationName")] = CFStringCreateCopy(kCFAllocatorDefault, m_icyName);
                    }
                    
                    m_delegate->streamMetaDataAvailable(metadataMap);
                }
                m_icyMetaData.clear();
                continue;
            }
            
            m_icyMetaData.push_back(buf[offset]);
            continue;
        }
        
        // is this the interval byte?
        if (m_icyMetaDataInterval > 0 && m_dataByteReadCount == m_icyMetaDataInterval) {
            m_metaDataBytesRemaining = buf[offset] * 16;
            
            if (m_metaDataBytesRemaining == 0) {
                m_dataByteReadCount = 0;
            }
            continue;
        }
        
        // a data byte
        m_dataByteReadCount++;
        m_icyReadBuffer[i++] = buf[offset];
    }
    
    if (m_delegate && i > 0) {
        m_delegate->streamHasBytesAvailable(m_icyReadBuffer, i);
    }
}
    
static bool metadataLooksMisdecoded(CFStringRef str)
{
    return CFStringFind(str, CFSTR("Ã"), 0).location != kCFNotFound ||
           CFStringFind(str, CFSTR("Â"), 0).location != kCFNotFound ||
           CFStringFind(str, CFSTR("â"), 0).location != kCFNotFound;
}

static CFStringRef createStringByRoundTrippingEncoding(CFStringRef str,
                                                       CFStringEncoding sourceEncoding,
                                                       CFStringEncoding targetEncoding)
{
    if (!str) {
        return NULL;
    }

    CFIndex length = CFStringGetLength(str);
    CFIndex maxBytes = CFStringGetMaximumSizeForEncoding(length, sourceEncoding);
    if (maxBytes <= 0) {
        return NULL;
    }

    std::vector<UInt8> bytes(maxBytes);
    CFIndex usedBytes = 0;
    CFIndex converted = CFStringGetBytes(str,
                                         CFRangeMake(0, length),
                                         sourceEncoding,
                                         0,
                                         false,
                                         &bytes[0],
                                         maxBytes,
                                         &usedBytes);
    if (converted == 0 || usedBytes == 0) {
        return NULL;
    }

    return CFStringCreateWithBytes(kCFAllocatorDefault,
                                   &bytes[0],
                                   usedBytes,
                                   targetEncoding,
                                   false);
}

static CFStringRef repairedMetadataString(CFStringRef str)
{
    if (!str) {
        return NULL;
    }

    if (!metadataLooksMisdecoded(str)) {
        return (CFStringRef)CFRetain(str);
    }

    CFStringRef repaired = createStringByRoundTrippingEncoding(str,
                                                               kCFStringEncodingWindowsLatin1,
                                                               kCFStringEncodingUTF8);
    if (repaired) {
        return repaired;
    }

    repaired = createStringByRoundTrippingEncoding(str,
                                                   kCFStringEncodingISOLatin1,
                                                   kCFStringEncodingUTF8);
    if (repaired) {
        return repaired;
    }

    return (CFStringRef)CFRetain(str);
}

CFStringRef HTTP_Stream::createMetaDataStringWithMostReasonableEncoding(const UInt8 *bytes, const CFIndex numBytes)
{
    const CFStringEncoding encodings[] = {
        kCFStringEncodingUTF8,
        kCFStringEncodingWindowsLatin1,
        kCFStringEncodingISOLatin1,
        kCFStringEncodingNextStepLatin,
        kCFStringEncodingISOLatin2,
        kCFStringEncodingISOLatin3,
        kCFStringEncodingISOLatin4,
        kCFStringEncodingISOLatinCyrillic,
        kCFStringEncodingISOLatinArabic,
        kCFStringEncodingISOLatinGreek,
        kCFStringEncodingISOLatinHebrew,
        kCFStringEncodingISOLatin5,
        kCFStringEncodingISOLatin6,
        kCFStringEncodingISOLatinThai,
        kCFStringEncodingISOLatin7,
        kCFStringEncodingISOLatin8,
        kCFStringEncodingISOLatin9,
        kCFStringEncodingWindowsLatin2,
        kCFStringEncodingWindowsCyrillic,
        kCFStringEncodingWindowsGreek,
        kCFStringEncodingWindowsLatin5,
        kCFStringEncodingWindowsHebrew,
        kCFStringEncodingWindowsArabic,
        kCFStringEncodingKOI8_R,
        kCFStringEncodingBig5,
        kCFStringEncodingASCII
    };

    for (size_t i = 0; i < sizeof(encodings) / sizeof(encodings[0]); ++i) {
        CFStringRef metaData = CFStringCreateWithBytes(kCFAllocatorDefault,
                                                       bytes,
                                                       numBytes,
                                                       encodings[i],
                                                       false);
        if (metaData != NULL) {
            CFStringRef repaired = repairedMetadataString(metaData);
            CFRelease(metaData);
            return repaired;
        }
    }

    return NULL;
}

void HTTP_Stream::readCallBack(CFReadStreamRef stream, CFStreamEventType eventType, void *clientCallBackInfo)
{
    HTTP_Stream *THIS = static_cast<HTTP_Stream*>(clientCallBackInfo);
    
    Stream_Configuration *config = Stream_Configuration::configuration();
    
    CFStringRef reportedNetworkError = NULL;
    
    switch (eventType) {
        case kCFStreamEventHasBytesAvailable: {
            if (!THIS->m_httpReadBuffer) {
                THIS->m_httpReadBuffer = new UInt8[config->httpConnectionBufferSize];
            }
            
            while (CFReadStreamHasBytesAvailable(stream)) {
                if (!THIS->m_scheduledInRunLoop) {
                    /*
                     * This is critical - though the stream has data available,
                     * do not try to feed the audio queue with data, if it has
                     * indicated that it doesn't want more data due to buffers
                     * full.
                     */
                    THIS->m_readPending = true;
                    break;
                }
                
                CFIndex bytesRead = CFReadStreamRead(stream, THIS->m_httpReadBuffer, config->httpConnectionBufferSize);
                
                if (CFReadStreamGetStatus(stream) == kCFStreamStatusError ||
                    bytesRead < 0) {
                    if (THIS->contentLength() > 0) {
                        /*
                         * Try to recover gracefully if we have a non-continuous stream
                         */
                        Input_Stream_Position currentPosition = THIS->position();
                        
                        Input_Stream_Position recoveryPosition;
                        recoveryPosition.start = currentPosition.start + THIS->m_bytesRead;
                        recoveryPosition.end = THIS->contentLength();
                        
                        HS_TRACE("Recovering HTTP stream, start %llu\n", recoveryPosition.start);
                        
                        THIS->open(recoveryPosition);
                        
                        break;
                    }
                    
                    CFErrorRef streamError = CFReadStreamCopyError(stream);
                    
                    if (streamError) {
                        CFStringRef errorDesc = CFErrorCopyDescription(streamError);
                        
                        if (errorDesc) {
                            reportedNetworkError = CFStringCreateCopy(kCFAllocatorDefault, errorDesc);
                            
                            CFRelease(errorDesc);
                        }
                        
                        CFRelease(streamError);
                    }
                    
                    if (THIS->m_delegate) {
                        THIS->m_delegate->streamErrorOccurred(reportedNetworkError);
                        
                        if (reportedNetworkError) {
                            CFRelease(reportedNetworkError);
                            reportedNetworkError = NULL;
                        }
                    }
                    break;
                }
                
                if (bytesRead > 0) {
                    CFIndex bodyOffset = 0;
                    if (!THIS->parseHttpHeadersIfNeeded(THIS->m_httpReadBuffer, bytesRead, &bodyOffset)) {
                        continue;
                    }

                    if (bodyOffset >= bytesRead) {
                        continue;
                    }

                    UInt8 *bodyBuffer = THIS->m_httpReadBuffer + bodyOffset;
                    CFIndex bodyBytesRead = bytesRead - bodyOffset;
                    THIS->m_bytesRead += bodyBytesRead;

                    HS_TRACE("Read %li bytes, total %llu\n", bodyBytesRead, THIS->m_bytesRead);

    #ifdef INCLUDE_ID3TAG_SUPPORT
                    if (!THIS->m_icyStream && THIS->m_id3Parser->wantData()) {
                        THIS->m_id3Parser->feedData(bodyBuffer, (UInt32)bodyBytesRead);
                    }
    #endif

                    if (THIS->m_icyStream) {
                        HS_TRACE("Parsing ICY stream\n");

                        THIS->parseICYStream(bodyBuffer, bodyBytesRead);
                    } else {
                        if (THIS->m_delegate) {
                            HS_TRACE("Not an ICY stream; calling the delegate back\n");

                            THIS->m_delegate->streamHasBytesAvailable(bodyBuffer, (UInt32)bodyBytesRead);
                        }
                    }
                }
            }
            
            if (reportedNetworkError) {
                CFRelease(reportedNetworkError);
                reportedNetworkError = NULL;
            }
            
            break;
        }
        case kCFStreamEventEndEncountered: {
            
            // This should concerns only non-continous streams
            if (THIS->m_bytesRead < THIS->contentLength()) {
                HS_TRACE("End of stream, but we have read only %llu bytes on a total of %li. Missing: %llu\n", THIS->m_bytesRead, THIS->contentLength(), (THIS->contentLength() - THIS->m_bytesRead));
                
                Input_Stream_Position currentPosition = THIS->position();
                
                Input_Stream_Position recoveryPosition;
                recoveryPosition.start = currentPosition.start + THIS->m_bytesRead;
                recoveryPosition.end = THIS->contentLength();
                
                HS_TRACE("Reopen for the end of the file from byte position: %llu\n", recoveryPosition.start);
                THIS->close();
                THIS->open(recoveryPosition);
                break;
            }
            
            if (THIS->m_delegate) {
                THIS->m_delegate->streamEndEncountered();
            }
            break;
        }
        case kCFStreamEventErrorOccurred: {
            if (THIS->m_delegate) {
                CFStringRef reportedNetworkError = NULL;
                CFErrorRef streamError = CFReadStreamCopyError(stream);
                
                if (streamError) {
                    CFStringRef errorDesc = CFErrorCopyDescription(streamError);
                    
                    if (errorDesc) {
                        reportedNetworkError = CFStringCreateCopy(kCFAllocatorDefault, errorDesc);
                        
                        CFRelease(errorDesc);
                    }
                    
                    CFRelease(streamError);
                }
                
                THIS->m_delegate->streamErrorOccurred(reportedNetworkError);
                if (reportedNetworkError) {
                    CFRelease(reportedNetworkError);
                }
            }
            break;
        }
    }
}

}  // namespace astreamer
