package com.example.mesh_app

import android.content.Context
import android.net.ConnectivityManager
import android.net.Network
import android.net.NetworkCapabilities
import android.net.NetworkRequest
import android.os.Handler
import android.os.Looper
import io.flutter.embedding.android.FlutterActivity
import io.flutter.embedding.engine.FlutterEngine
import io.flutter.plugin.common.MethodChannel

class MainActivity : FlutterActivity() {
    private val channelName = "farmely/wifi_bind"
    private var wifiCallback: ConnectivityManager.NetworkCallback? = null

    override fun configureFlutterEngine(flutterEngine: FlutterEngine) {
        super.configureFlutterEngine(flutterEngine)
        MethodChannel(flutterEngine.dartExecutor.binaryMessenger, channelName)
            .setMethodCallHandler { call, result ->
                when (call.method) {
                    "bindWifi" -> bindWifi(result)
                    "unbindWifi" -> {
                        unbindWifi()
                        result.success(true)
                    }
                    else -> result.notImplemented()
                }
            }
    }

    /**
     * Force this process's sockets onto the Wi-Fi network even when it has
     * no internet access. Without this, Android routes app traffic to
     * mobile data because the SoftAP network fails validation, so requests
     * to 192.168.4.1 never reach the device.
     */
    private fun bindWifi(result: MethodChannel.Result) {
        val cm = getSystemService(Context.CONNECTIVITY_SERVICE) as ConnectivityManager
        unbindWifi()
        val request = NetworkRequest.Builder()
            .addTransportType(NetworkCapabilities.TRANSPORT_WIFI)
            .removeCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
            .build()
        val handler = Handler(Looper.getMainLooper())
        var finished = false
        val callback = object : ConnectivityManager.NetworkCallback() {
            override fun onAvailable(network: Network) {
                handler.post {
                    if (finished) return@post
                    finished = true
                    cm.bindProcessToNetwork(network)
                    result.success(true)
                }
            }

            override fun onUnavailable() {
                handler.post {
                    if (finished) return@post
                    finished = true
                    result.success(false)
                }
            }
        }
        wifiCallback = callback
        cm.requestNetwork(request, callback)
        handler.postDelayed({
            if (!finished) {
                finished = true
                result.success(false)
            }
        }, 8000)
    }

    private fun unbindWifi() {
        val cm = getSystemService(Context.CONNECTIVITY_SERVICE) as ConnectivityManager
        cm.bindProcessToNetwork(null)
        wifiCallback?.let {
            try {
                cm.unregisterNetworkCallback(it)
            } catch (_: IllegalArgumentException) {
            }
        }
        wifiCallback = null
    }

    override fun onDestroy() {
        unbindWifi()
        super.onDestroy()
    }
}
