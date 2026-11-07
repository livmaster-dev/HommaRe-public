// cloud_saves_apple.mm - the synced folder's side on Apple (cloud_saves.h): every
// access coordinated with the folder's file provider (iCloud Drive's, or
// another app's in the Files app), and files not on this device yet asked
// for and waited on a little.
//
// iCloud Drive keeps a file that is not downloaded as ".<name>.icloud" (or,
// newer, as the file itself with no data): either is listed as pending, its
// download started, and the listing waits up to a few seconds for it.
#import <Foundation/Foundation.h>

#include "cloud_saves.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>

namespace cloud_saves {

namespace {

constexpr auto kDownloadWait = std::chrono::seconds(8);

NSURL *url_of(const std::string &path) {
    return [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
}

// ".save_1.sav.icloud" -> "save_1.sav"; "" when `name` is no placeholder.
NSString *placeholder_target(NSString *name) {
    if (name.length > 8 && [name hasPrefix:@"."] && [name hasSuffix:@".icloud"])
        return [name substringWithRange:NSMakeRange(1, name.length - 8)];
    return nil;
}

bool not_downloaded(NSURL *url) {
    NSNumber *ubiquitous = nil;
    [url getResourceValue:&ubiquitous forKey:NSURLIsUbiquitousItemKey error:nil];
    if (!ubiquitous.boolValue)
        return false;
    NSString *status = nil;
    [url getResourceValue:&status forKey:NSURLUbiquitousItemDownloadingStatusKey error:nil];
    return status && [status isEqualToString:NSURLUbiquitousItemDownloadingStatusNotDownloaded];
}

class AppleIo : public Io {
public:
    bool list(const std::string &root, Listing *out) override {
        for (auto until = std::chrono::steady_clock::now() + kDownloadWait;;) {
            Listing l;
            if (!list_once(root, &l))
                return false;
            const bool waiting =
                std::any_of(l.begin(), l.end(), [](const auto &kv) { return kv.second.pending; });
            if (!waiting || std::chrono::steady_clock::now() >= until) {
                *out = std::move(l);
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }

    bool read(const std::string &src, const std::string &dst) override {
        __block bool ok = false;
        NSError *error = nil;
        [[[NSFileCoordinator alloc] initWithFilePresenter:nil]
            coordinateReadingItemAtURL:url_of(src)
                               options:0 // downloads it first
                                 error:&error
                            byAccessor:^(NSURL *url) {
                              ok = detail::copy_keeping_time(url.path.fileSystemRepresentation, dst);
                            }];
        return ok && !error;
    }

    bool write(const std::string &src, const std::string &dst, Stamp *now) override {
        __block bool ok = false;
        NSError *error = nil;
        [[NSFileManager defaultManager] createDirectoryAtURL:url_of(dst).URLByDeletingLastPathComponent
                                 withIntermediateDirectories:YES
                                                  attributes:nil
                                                       error:nil];
        [[[NSFileCoordinator alloc] initWithFilePresenter:nil]
            coordinateWritingItemAtURL:url_of(dst)
                               options:NSFileCoordinatorWritingForReplacing
                                 error:&error
                            byAccessor:^(NSURL *url) {
                              const std::string to = url.path.fileSystemRepresentation;
                              ok = detail::copy_keeping_time(src, to) && detail::stat_path(to, now);
                            }];
        return ok && !error;
    }

    bool remove(const std::string &path) override {
        __block bool ok = false;
        NSError *error = nil;
        [[[NSFileCoordinator alloc] initWithFilePresenter:nil]
            coordinateWritingItemAtURL:url_of(path)
                               options:NSFileCoordinatorWritingForDeleting
                                 error:&error
                            byAccessor:^(NSURL *url) {
                              NSError *gone = nil;
                              ok = [[NSFileManager defaultManager] removeItemAtURL:url error:&gone] ||
                                   gone.code == NSFileNoSuchFileError;
                            }];
        return ok && !error;
    }

private:
    static bool list_once(const std::string &root, Listing *out) {
        @autoreleasepool {
            NSFileManager *fm = [NSFileManager defaultManager];
            NSURL *base = url_of(root);
            BOOL dir = NO;
            if (![fm fileExistsAtPath:base.path isDirectory:&dir])
                return true; // nothing synced yet
            if (!dir)
                return false;
            __block bool ok = true;
            NSError *error = nil;
            [[[NSFileCoordinator alloc] initWithFilePresenter:nil]
                coordinateReadingItemAtURL:base
                                   options:NSFileCoordinatorReadingImmediatelyAvailableMetadataOnly
                                     error:&error
                                byAccessor:^(NSURL *at) {
                                  NSDirectoryEnumerator *it =
                                      [fm enumeratorAtURL:at
                                          includingPropertiesForKeys:@[ NSURLIsDirectoryKey ]
                                                             options:NSDirectoryEnumerationProducesRelativePathURLs
                                                        errorHandler:^BOOL(NSURL *, NSError *) {
                                                          ok = false;
                                                          return NO;
                                                        }];
                                  for (NSURL *url in it) {
                                      NSString *name = url.lastPathComponent;
                                      NSNumber *isDir = nil;
                                      [url getResourceValue:&isDir forKey:NSURLIsDirectoryKey error:nil];
                                      if (isDir.boolValue) {
                                          if ([name hasPrefix:@"."])
                                              [it skipDescendants];
                                          continue;
                                      }
                                      NSString *relative = url.relativePath;
                                      NSString *folder = relative.stringByDeletingLastPathComponent;
                                      if (NSString *target = placeholder_target(name)) {
                                          NSString *real = [folder stringByAppendingPathComponent:target];
                                          Entry e;
                                          e.pending = true;
                                          out->emplace(real.UTF8String, e); // the file itself, if listed, wins
                                          [fm startDownloadingUbiquitousItemAtURL:[at URLByAppendingPathComponent:real]
                                                                            error:nil];
                                          continue;
                                      }
                                      if ([name hasPrefix:@"."])
                                          continue;
                                      Entry e;
                                      if (not_downloaded(url)) {
                                          e.pending = true;
                                          [fm startDownloadingUbiquitousItemAtURL:url error:nil];
                                      } else if (!detail::stat_path(url.path.fileSystemRepresentation, &e.stamp)) {
                                          continue;
                                      }
                                      (*out)[relative.UTF8String] = e;
                                  }
                                }];
            return ok && !error;
        }
    }
};

} // namespace

Io &platform_io() {
    static AppleIo io;
    return io;
}

} // namespace cloud_saves
