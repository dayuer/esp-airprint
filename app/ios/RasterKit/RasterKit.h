#import <Foundation/Foundation.h>
#import <AppSpecs/AppSpecs.h>

NS_ASSUME_NONNULL_BEGIN

/**
 * 光栅模块的 iOS 实现。
 *
 * 它只做类型转换和平台渲染，编码走 shared/urf 那份跨平台 C++——
 * 那份代码在开发机上有单测，抄一份到这里就再也测不了了。
 */
@interface RasterKit : NSObject <NativeRasterKitSpec>
@end

NS_ASSUME_NONNULL_END
