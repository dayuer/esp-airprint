# RasterKit（iOS）

只做类型转换和平台渲染，编码走 `shared/urf` 那份跨平台 C++。

## 为什么不复制 shared/urf 的源文件进来

复制出来的那份就再也跑不了 `shared/urf/run_tests.sh` 的 35 个单测了，
而那是这个项目里唯一能在**不插打印机、不烧纸**的前提下验证光栅正确性的手段。

所以 Xcode target 直接引用 `../shared/urf/src/*.cpp`，头文件搜索路径指向
`../shared/urf/include`。接线由 `ios/scripts/wire_rasterkit.rb` 做，可以重复跑。

## 改了原生文件之后

`.pbxproj` 是提交进仓库的，正常情况下不用再跑接线脚本。只有在**新增**
`shared/urf` 的源文件、或者 `.pbxproj` 被 RN 升级冲掉时才需要：

    ruby ios/scripts/wire_rasterkit.rb
    cd ios && pod install

`codegenConfig` 改了（`src/native/*.ts` 增删了方法）之后必须重跑 `pod install`——
`AppSpecs` 的协议是在那一步生成的，不重跑的话编译会报「未实现协议方法」。
