#import "RasterKit.h"

#import <string>

#import "urf/urf.h"

@implementation RasterKit

RCT_EXPORT_MODULE()

- (NSString *)encoderVersion
{
  // 带上 colorspace，因为那是唯一一个填错不会报错、只会出废纸的常量。
  std::string v = "urf/1 bpp=8 colorspace=" + std::to_string(urf::kColorspaceGray) +
                  " duplex=" + std::to_string(urf::kDuplexOneSided);
  return [NSString stringWithUTF8String:v.c_str()];
}

- (void)validateForUpload:(NSString *)path
            expectWidthPx:(double)expectWidthPx
           expectHeightPx:(double)expectHeightPx
                  resolve:(RCTPromiseResolveBlock)resolve
                   reject:(RCTPromiseRejectBlock)reject
{
  try {
    urf::ValidateResult r = urf::ValidateForUpload(
        std::string(path.UTF8String ?: ""), static_cast<uint32_t>(expectWidthPx),
        static_cast<uint32_t>(expectHeightPx));
    resolve(@{
      @"ok" : @(r.ok),
      @"pages" : @(r.actual_pages),
      @"widthPx" : @(r.width_px),
      @"heightPx" : @(r.height_px),
      @"error" : [NSString stringWithUTF8String:r.error.c_str()],
    });
  } catch (const std::exception &e) {
    reject(@"validate_failed", [NSString stringWithUTF8String:e.what()], nil);
  }
}

- (void)rasterize:(JS::NativeRasterKit::RasterizeOptions &)options
          resolve:(RCTPromiseResolveBlock)resolve
           reject:(RCTPromiseRejectBlock)reject
{
  // 渲染在下一步。先把 JS → TurboModule → ObjC++ → shared/urf 这条链打通，
  // 链没通的话渲染写完了也验不了。
  (void)options;
  (void)resolve;
  reject(@"not_implemented", @"渲染还没实现", nil);
}

- (std::shared_ptr<facebook::react::TurboModule>)getTurboModule:
    (const facebook::react::ObjCTurboModule::InitParams &)params
{
  return std::make_shared<facebook::react::NativeRasterKitSpecJSI>(params);
}

@end
