package com.airprint

import android.os.Bundle
import com.facebook.react.ReactActivity
import com.facebook.react.ReactActivityDelegate
import com.facebook.react.defaults.DefaultNewArchitectureEntryPoint.fabricEnabled
import com.facebook.react.defaults.DefaultReactActivityDelegate

class MainActivity : ReactActivity() {

  /**
   * Returns the name of the main component registered from JavaScript. This is used to schedule
   * rendering of the component.
   */
  override fun getMainComponentName(): String = "AirPrint"

  /**
   * Returns the instance of the [ReactActivityDelegate]. We use [DefaultReactActivityDelegate]
   * which allows you to enable New Architecture with a single boolean flags [fabricEnabled]
   */
  override fun createReactActivityDelegate(): ReactActivityDelegate =
      DefaultReactActivityDelegate(this, mainComponentName, fabricEnabled)

  /**
   * react-native-screens 要求丢掉 Android 的 Fragment 恢复状态。
   * 不传 null 的话，系统杀掉进程后再恢复时会拿旧的 Fragment 树去对新的
   * React 树，直接崩在启动上——而且只在「后台被杀再回来」时复现。
   */
  override fun onCreate(savedInstanceState: Bundle?) {
    super.onCreate(null)
  }
}
