package com.earthengine.minimalglobe;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.security.SecureRandom;
import java.security.cert.X509Certificate;
import javax.net.ssl.HttpsURLConnection;
import javax.net.ssl.SSLContext;
import javax.net.ssl.TrustManager;
import javax.net.ssl.X509TrustManager;

/**
 * JNI 辅助类 — 为 C++ AndroidPlatformBridge 提供 HTTP 和图片解码。
 */
public class JniHttpHelper {
    /**
     * 同步 HTTP GET，返回 body 字节数组（失败返回空数组）。
     * 由 C++ 通过 JNI 调用。
     */
    static {
        try {
            TrustManager[] trustAll = new TrustManager[] {
                new X509TrustManager() {
                    public X509Certificate[] getAcceptedIssuers() { return new X509Certificate[0]; }
                    public void checkClientTrusted(X509Certificate[] c, String a) {}
                    public void checkServerTrusted(X509Certificate[] c, String a) {}
                }
            };
            SSLContext sc = SSLContext.getInstance("TLS");
            sc.init(null, trustAll, new SecureRandom());
            HttpsURLConnection.setDefaultSSLSocketFactory(sc.getSocketFactory());
            HttpsURLConnection.setDefaultHostnameVerifier((h, s) -> true);
        } catch (Exception e) {
            android.util.Log.e("JniHttpHelper", "SSL trust-all setup failed", e);
        }
    }

    private static final java.util.concurrent.Semaphore semaphore =
        new java.util.concurrent.Semaphore(4);

    public static byte[] httpGet(String urlString) {
        try { semaphore.acquire(); } catch (InterruptedException e) { return new byte[0]; }
        try {
            android.util.Log.i("JniHttpHelper", "httpGet called: " + urlString);
            URL url = new URL(urlString);
            HttpURLConnection conn = (HttpURLConnection) url.openConnection();
            conn.setRequestMethod("GET");
            conn.setConnectTimeout(5000);
            conn.setReadTimeout(10000);
            conn.connect();

            int code = conn.getResponseCode();
            if (code != 200) {
                conn.disconnect();
                return new byte[0];
            }

            InputStream is = conn.getInputStream();
            ByteArrayOutputStream baos = new ByteArrayOutputStream();
            byte[] buf = new byte[4096];
            int n;
            while ((n = is.read(buf)) > 0) {
                baos.write(buf, 0, n);
            }
            is.close();
            conn.disconnect();
            byte[] result = baos.toByteArray();
            android.util.Log.i("JniHttpHelper", "httpGet OK: " + urlString + " -> " + result.length + " bytes");
            return result;
        } catch (Exception e) {
            android.util.Log.e("JniHttpHelper", "HTTP GET failed: " + urlString, e);
            return new byte[0];
        } finally {
            semaphore.release();
        }
    }
}
