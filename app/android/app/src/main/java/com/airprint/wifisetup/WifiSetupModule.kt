package com.airprint.wifisetup

import android.net.ConnectivityManager
import android.net.Network
import android.net.NetworkCapabilities
import android.net.NetworkRequest
import android.net.wifi.WifiNetworkSpecifier
import android.os.PatternMatcher
import com.airprint.specs.NativeWifiSetupSpec
import com.facebook.react.bridge.Arguments
import com.facebook.react.bridge.Promise
import com.facebook.react.bridge.ReactApplicationContext
import com.facebook.react.module.annotations.ReactModule
import java.io.BufferedReader
import java.net.HttpURLConnection
import java.net.URL
import java.util.concurrent.atomic.AtomicBoolean

@ReactModule(name = NativeWifiSetupSpec.NAME)
class WifiSetupModule(reactContext: ReactApplicationContext) :
  NativeWifiSetupSpec(reactContext) {

  override fun getName(): String = NAME

  private val cm =
    reactContext.getSystemService(ConnectivityManager::class.java)

  private var callback: ConnectivityManager.NetworkCallback? = null

  @Volatile private var network: Network? = null
  @Volatile private var joinedSsid: String = ""

  override fun isJoined(): Boolean = network != null

  override fun joinSetupNetwork(ssidPrefix: String, timeoutMs: Double, promise: Promise) {
    leave()

    // 前缀匹配：系统弹出的选择框里只会列出以它开头的网络，用户点一下确认。
    // Android 10+ 这一步省不掉，也不该省——加入一个网络是用户的决定。
    val specifier = WifiNetworkSpecifier.Builder()
      .setSsidPattern(PatternMatcher(ssidPrefix, PatternMatcher.PATTERN_PREFIX))
      .build()

    val request = NetworkRequest.Builder()
      .addTransportType(NetworkCapabilities.TRANSPORT_WIFI)
      // 不要求 INTERNET：配网热点本来就没有互联网。要求了就永远等不到。
      .removeCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
      .setNetworkSpecifier(specifier)
      .build()

    val settled = AtomicBoolean(false)
    val cb = object : ConnectivityManager.NetworkCallback() {
      override fun onAvailable(net: Network) {
        network = net
        if (settled.compareAndSet(false, true)) promise.resolve(joinedSsid)
      }

      override fun onCapabilitiesChanged(net: Network, caps: NetworkCapabilities) {
        // Android 12+ 才能从这里读到 SSID，且不需要定位权限。
        val info = caps.transportInfo
        if (info is android.net.wifi.WifiInfo) {
          joinedSsid = info.ssid.trim('"')
        }
      }

      override fun onUnavailable() {
        if (settled.compareAndSet(false, true)) {
          promise.reject("join_cancelled", "没有加入配网热点（用户取消或超时）")
        }
        leave()
      }

      override fun onLost(net: Network) {
        // 设备验证通过后 3 秒就重启，热点随之消失——这条路径是正常结局，不是错误。
        if (network == net) network = null
      }
    }
    callback = cb
    cm.requestNetwork(request, cb, timeoutMs.toInt())
  }

  override fun portalRequest(
    method: String,
    url: String,
    body: String?,
    timeoutMs: Double,
    promise: Promise,
  ) {
    val net = network
    if (net == null) {
      promise.reject("not_joined", "还没加入配网热点")
      return
    }
    // 网络请求不能在主线程上做。
    Thread {
      var conn: HttpURLConnection? = null
      try {
        // 关键：用 network.openConnection 而不是普通的 URL.openConnection，
        // 这样只有这一个连接走配网热点，其余流量照常走蜂窝。
        conn = net.openConnection(URL(url)) as HttpURLConnection
        conn.requestMethod = method
        conn.connectTimeout = timeoutMs.toInt()
        conn.readTimeout = timeoutMs.toInt()
        if (body != null) {
          conn.doOutput = true
          conn.setRequestProperty("Content-Type", "application/json")
          conn.outputStream.use { it.write(body.toByteArray(Charsets.UTF_8)) }
        }
        val status = conn.responseCode
        val stream = if (status in 200..299) conn.inputStream else conn.errorStream
        val text = stream?.bufferedReader()?.use(BufferedReader::readText) ?: ""
        promise.resolve(
          Arguments.createMap().apply {
            putDouble("status", status.toDouble())
            putString("body", text)
          },
        )
      } catch (e: Throwable) {
        promise.reject("portal_request_failed", e.message, e)
      } finally {
        conn?.disconnect()
      }
    }.start()
  }

  override fun leave() {
    callback?.let {
      runCatching { cm.unregisterNetworkCallback(it) }
    }
    callback = null
    network = null
    joinedSsid = ""
  }

  override fun invalidate() {
    leave()
    super.invalidate()
  }
}
