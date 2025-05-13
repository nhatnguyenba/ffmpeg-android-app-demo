package com.nhatnguyenba.ffmpeg

import android.content.Intent
import android.net.Uri
import android.os.Bundle
import android.util.Log
import androidx.activity.ComponentActivity
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import com.google.android.exoplayer2.ExoPlayer
import com.google.android.exoplayer2.MediaItem
import com.google.android.exoplayer2.SimpleExoPlayer
import com.google.android.exoplayer2.ui.PlayerView
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File
import java.io.FileOutputStream

class MainActivity : ComponentActivity() {

    companion object {
        init {
            System.loadLibrary("ffmpeg-wrapper")
//            System.loadLibrary("avcodec")
//            System.loadLibrary("avfilter")
//            System.loadLibrary("avformat")
//            System.loadLibrary("avutil")
//            System.loadLibrary("swresample")
//            System.loadLibrary("swscale")
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        setContent {
            VideoProcessorApp(
            )
        }
    }
}

sealed class Operation {
    object Rotate : Operation()
    object AddLogo : Operation()
}

@Composable
fun VideoProcessorApp() {
    val context = LocalContext.current
    var originalVideoPath by remember { mutableStateOf<String?>(null) }
    var modifiedVideoPath by remember { mutableStateOf<String?>(null) }
    var processing by remember { mutableStateOf(false) }
    var currentOperation by remember { mutableStateOf<Operation?>(null) }

    // Khởi tạo và copy file từ assets
    LaunchedEffect(Unit) {
        val assets = context.assets
        val filesDir = context.filesDir

        // Copy original.mp4
        File(filesDir, "original.mp4").takeIf { !it.exists() }?.let { targetFile ->
            assets.openFd("original.mp4").use { afd ->
                Log.d("NHAT","original copy")
                FileOutputStream(targetFile).channel.transferFrom(
                    afd.createInputStream().channel,
                    0,
                    afd.declaredLength
                )
            }
        }

        // Copy changed.mp4 (nếu cần)
        File(filesDir, "changed.mp4").takeIf { !it.exists() }?.let { targetFile ->
            assets.openFd("changed.mp4").use { afd ->
                Log.d("NHAT","changed copy")
                FileOutputStream(targetFile).channel.transferFrom(
                    afd.createInputStream().channel,
                    0,
                    afd.declaredLength
                )
            }
        }

        originalVideoPath = File(filesDir, "original.mp4").absolutePath
        modifiedVideoPath = File(filesDir, "changed.mp4").absolutePath
        Log.d("VideoProcessorApp", "Original video path: $originalVideoPath")
        Log.d("VideoProcessorApp", "Modified video path: $modifiedVideoPath")
//        originalVideoPath = "../../../../../assets/original.mp4"
//        modifiedVideoPath = "../../../../../assets/changed.mp4"
        Log.d("VideoProcessorApp", "Original video path: $originalVideoPath")
        Log.d("VideoProcessorApp", "Modified video path: $modifiedVideoPath")
    }

    Column(
        modifier = Modifier
            .fillMaxSize()
            .padding(16.dp),
        horizontalAlignment = Alignment.CenterHorizontally
    ) {
        // Hiển thị video gốc
        originalVideoPath?.let {
            VideoPlayer(
                uri = Uri.fromFile(File(it)),
                modifier = Modifier
                    .fillMaxWidth()
                    .height(200.dp)
            )
        }

        Spacer(modifier = Modifier.height(16.dp))

        // Hiển thị video đã xử lý
        modifiedVideoPath?.let {
            VideoPlayer(
                uri = Uri.fromFile(File(it)),
                modifier = Modifier
                    .fillMaxWidth()
                    .height(200.dp)
            )
        }

        Spacer(modifier = Modifier.height(16.dp))

        // Row chứa 2 nút chức năng
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.SpaceEvenly
        ) {
            // Nút xoay video
            Button(
                onClick = {
                    currentOperation = Operation.Rotate
                    processing = true
                },
                enabled = !processing,
                modifier = Modifier.weight(1f)
            ) {
                Text("Xoay 90 độ")
            }

            Spacer(modifier = Modifier.width(8.dp))

            // Nút thêm logo
            Button(
                onClick = {
                    currentOperation = Operation.AddLogo
                    processing = true
                },
                enabled = !processing,
                modifier = Modifier.weight(1f)
            ) {
                Text("Thêm Logo")
            }
        }

        // Xử lý logic khi có operation
        LaunchedEffect(currentOperation) {
            CoroutineScope(Dispatchers.Default).launch{
                when (currentOperation) {
                    is Operation.Rotate -> {
                        originalVideoPath?.let { input ->
                            modifiedVideoPath?.let { output ->
                                // ko nên truyền path mà nên truyền ByteArray (nghĩa là truyền data chứ ko phải path)
                                val result = rotateVideo(input, output, 90)
                                if (result == 0) {
                                    // Cập nhật UI khi thành công
                                    modifiedVideoPath = output
                                }
                            }
                        }
                        processing = false
                        currentOperation = null
                    }

                    is Operation.AddLogo -> {
                        originalVideoPath?.let { input ->
                            modifiedVideoPath?.let { output ->
                                val watermarkPath = "" // Lấy đường dẫn logo từ assets
                                val result = applyWatermark(input, output, watermarkPath)
                                if (result == 0) {
                                    modifiedVideoPath = output
                                }
                            }
                        }
                        processing = false
                        currentOperation = null
                    }

                    null -> {}
                }
            }
        }

        // Hiển thị trạng thái loading
        if (processing) {
            CircularProgressIndicator(modifier = Modifier.padding(16.dp))
        }
    }
}

@Composable
fun VideoPlayer(uri: Uri, modifier: Modifier = Modifier) {
    val context = LocalContext.current
    val exoPlayer = remember {
        ExoPlayer.Builder(context).build().apply {
            setMediaItem(MediaItem.fromUri(uri))
            prepare()
        }
    }

    DisposableEffect(Unit) {
        onDispose {
            exoPlayer.release()
        }
    }

    AndroidView(
        factory = { ctx ->
            PlayerView(ctx).apply {
                player = exoPlayer
                useController = true
            }
        },
        modifier = modifier
    )
}

// Native functions
private external fun rotateVideo(
    inputPath: String,
    outputPath: String,
    degrees: Int
): Int

private external fun applyWatermark(
    inputPath: String,
    outputPath: String,
    watermarkPath: String
): Int