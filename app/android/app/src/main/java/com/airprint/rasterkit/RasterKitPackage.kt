package com.airprint.rasterkit

import com.airprint.specs.NativeRasterKitSpec
import com.facebook.react.BaseReactPackage
import com.facebook.react.bridge.NativeModule
import com.facebook.react.bridge.ReactApplicationContext
import com.facebook.react.module.model.ReactModuleInfo
import com.facebook.react.module.model.ReactModuleInfoProvider

class RasterKitPackage : BaseReactPackage() {
  override fun getModule(name: String, ctx: ReactApplicationContext): NativeModule? =
    if (name == NativeRasterKitSpec.NAME) RasterKitModule(ctx) else null

  override fun getReactModuleInfoProvider() = ReactModuleInfoProvider {
    mapOf(
      NativeRasterKitSpec.NAME to ReactModuleInfo(
        NativeRasterKitSpec.NAME,
        NativeRasterKitSpec.NAME,
        false,  // canOverrideExistingModule
        false,  // needsEagerInit
        false,  // isCxxModule
        true,   // isTurboModule
      ),
    )
  }
}
