package com.ulab.wowsdk;

import android.content.Context;
import android.media.MediaMetadataRetriever;

import com.ulab.wowsdk.utils.FileUtils;
import com.ulab.wowsdk.utils.VideoProcessingThrowable;

import java.io.File;
import java.lang.ref.WeakReference;
import java.util.ArrayList;
import java.util.List;

import rx.Observable;
import rx.Subscriber;
import rx.schedulers.Schedulers;

/**
 * Created by Ilja Kosynkin on 25.03.2016.
 * Copyright (c) 2015 inFullMobile. All rights reserved.
 */
public class VideoKit {
    private static final int TRIM_DURATION = 30;

    static {
        System.loadLibrary("avutil-54");
        System.loadLibrary("swresample-1");
        System.loadLibrary("avcodec-56");
        System.loadLibrary("avformat-56");
        System.loadLibrary("swscale-3");
        System.loadLibrary("avfilter-5");
        System.loadLibrary("avdevice-56");
        System.loadLibrary("videokit");
    }

    private final Context context;

    public VideoKit(Context context) {
        this.context = context.getApplicationContext();
    }

    /**
     * Call FFmpeg with specified arguments
     * @param args FFmpeg arguments
     * @return ret_code equal to 0 if success, for handled codes see file ffmpeg_ret_codes in docs
     */
    private int process(String[] args) {
        final String[] params = new String[args.length + 1];
        params[0] = "ffmpeg";
        System.arraycopy(args, 0, params, 1, args.length);

        return run(0, params);
    }

    // If loglevel greater then 0 there will VERY BIG AND ANNOYING LOG of ffmpeg working process
    // that however could be very useful in case return code didn't help a lot
    private native int run(int loglevel, String[] args);

    // CHECKSTYLE:OFF
    public String trimVideo(String path,
                                int startPosition,
                                int duration,
                                boolean deleteInput,
                                VideoProcessingListener listener) {
        return new CommandBuilder()
                    .shouldOverwriteOutput()
                    .addInputPath(path)
                    .trimForDuration(startPosition, duration)
                    .copyVideoCodec()
                    .copyAudioCodec()
                    .addExperimental()
                    .shouldDeleteInput(deleteInput)
                    .withListener(listener)
                    .processVideo();
    }
    // CHECKSTYLE:ON

    public String autoTrimVideo(String path, boolean deleteInput,
            VideoProcessingListener listener) {
        return trimVideo(path, 0, TRIM_DURATION, deleteInput, listener);
    }

     public String autoCropVideo(String path, boolean deleteInput,
             VideoProcessingListener listener) {
         return new CommandBuilder()
                        .addInputPath(path)
                        .autoCrop()
                        .addExperimental()
                        .shouldDeleteInput(deleteInput)
                        .withListener(listener)
                        .processVideo();
    }

    public String limitBitrate(String path, boolean deleteInput, VideoProcessingListener listener) {
        return new CommandBuilder()
                .addInputPath(path)
                .limitVideoBitrate()
                .addExperimental()
                .shouldDeleteInput(deleteInput)
                .withListener(listener)
                .processVideo();
    }

    public String processVideoBlocking(String path) {
        final String trimmed = autoTrimVideo(path, false);
        final String cropped = autoCropVideo(trimmed, true);
        return limitBitrate(cropped, true);
    }

    private String limitBitrate(String path, boolean deleteInput) {
        return limitBitrate(path, deleteInput, null);
    }

    private String autoCropVideo(String path, boolean deleteInput) {
        return autoCropVideo(path, deleteInput, null);
    }

    private String autoTrimVideo(String path, boolean deleteInput) {
        return autoTrimVideo(path, deleteInput, null);
    }

    public Observable<String> processVideoAsync(final String path) {
        return Observable.create(new Observable.OnSubscribe<String>() {
            @Override
            public void call(final Subscriber<? super String> subscriber) {
                final String trimmed = autoTrimVideo(path, false);
                final String cropped = autoCropVideo(trimmed, true);
                limitBitrate(cropped, true, new VideoProcessingListener() {
                    @Override
                    public void onSuccess(String path) {
                        subscriber.onNext(path);
                        subscriber.onCompleted();
                    }

                    @Override
                    public void onError(int errorCode) {
                        subscriber.onError(new VideoProcessingThrowable(errorCode));
                        subscriber.onCompleted();
                    }
                });
            }
        }).subscribeOn(Schedulers.computation());
    }

    public Observable<String> autoCropVideoAsync(final String path) {
        return Observable.create(new Observable.OnSubscribe<String>() {
            @Override
            public void call(final Subscriber<? super String> subscriber) {
                autoCropVideo(path, true, new VideoProcessingListener() {
                    @Override
                    public void onSuccess(String path) {
                        subscriber.onNext(path);
                        subscriber.onCompleted();
                    }

                    @Override
                    public void onError(int errorCode) {
                        subscriber.onError(new VideoProcessingThrowable(errorCode));
                        subscriber.onCompleted();
                    }
                });
            }
        }).subscribeOn(Schedulers.computation());
    }

    public interface VideoProcessingListener {
        void onSuccess(String path);
        void onError(int errorCode);
    }

    @SuppressWarnings({"SuspiciousNameCombination", "ResultOfMethodCallIgnored"})
    private class CommandBuilder {
        private static final String OVERWRITE_FLAG = "-y";
        private static final String INPUT_FILE_FLAG = "-i";
        private static final String TRIM_FLAG = "-ss";
        private static final String DURATION_FLAG = "-t";
        private static final String COPY_FLAG = "copy";
        private static final String VIDEO_CODEC_FLAG = "-vcodec";
        private static final String AUDIO_CODEC_FLAG = "-acodec";
        private static final String VIDEO_FILTER_FLAG = "-vf";
        private static final String VIDEO_BITRATE_FLAG = "-b:v";
        private static final String VIDEO_BITRATE = "713K";
        private static final String STRICT_FLAG = "-strict";
        private static final String EXPERIMENTAL_FLAG = "-2";

        private final  List<String> flags = new ArrayList<>();

        private final  MediaMetadataRetriever metadataRetriever = new MediaMetadataRetriever();

        private String inputPath;
        private boolean deleteInput;

        private WeakReference<VideoProcessingListener> processingListener;

        public CommandBuilder shouldOverwriteOutput() {
            flags.add(OVERWRITE_FLAG);
            return this;
        }

        public CommandBuilder addInputPath(String inputFilePath) {
            this.inputPath = inputFilePath;
            flags.add(INPUT_FILE_FLAG);
            flags.add(inputFilePath);
            return this;
        }

        public CommandBuilder trimForDuration(int startPosition, int duration) {
            flags.add(TRIM_FLAG);
            flags.add(String.valueOf(startPosition));
            flags.add(DURATION_FLAG);
            flags.add(String.valueOf(duration));
            return this;
        }

        public CommandBuilder copyAudioCodec() {
            flags.add(AUDIO_CODEC_FLAG);
            flags.add(COPY_FLAG);
            return this;
        }

        public CommandBuilder copyVideoCodec() {
            flags.add(VIDEO_CODEC_FLAG);
            flags.add(COPY_FLAG);
            return this;
        }

        // Idea of auto crop here is to take lesser dimension of video
        // and cut central part of video based on this dimension
        public CommandBuilder autoCrop() {
            metadataRetriever.setDataSource(inputPath);
            final int height = Integer.parseInt(metadataRetriever.extractMetadata(
                    MediaMetadataRetriever.METADATA_KEY_VIDEO_HEIGHT));
            final int width = Integer.parseInt(metadataRetriever.extractMetadata(
                    MediaMetadataRetriever.METADATA_KEY_VIDEO_WIDTH));
            int offset;
            if (height > width) {
                offset = Math.round((height - width) / 2);
                return addCrop(0, offset, width, width);
            } else {
                offset = Math.round((width - height) / 2);
                return addCrop(offset, 0, height, height);
            }
        }

        public CommandBuilder addCrop(int x, int y, int width, int height) {
            flags.add(VIDEO_FILTER_FLAG);
            flags.add("crop=" + width + ":" + height + ":" + x + ":" + y);
            return this;
        }

        public CommandBuilder limitVideoBitrate() {
            flags.add(VIDEO_BITRATE_FLAG);
            flags.add(VIDEO_BITRATE);
            return this;
        }

        public CommandBuilder addExperimental() {
            flags.add(STRICT_FLAG);
            flags.add(EXPERIMENTAL_FLAG);
            return this;
        }

        public CommandBuilder shouldDeleteInput(boolean shouldDelete) {
            deleteInput = shouldDelete;
            return this;
        }

        public CommandBuilder withListener(VideoProcessingListener listener) {
            processingListener = new WeakReference<>(listener);
            return this;
        }

        public String processVideo() {
            final String outputPath = FileUtils.createNewVideoFile(context).getAbsolutePath();
            flags.add(outputPath);

            final String ffmpegArguments[] = convertFlagsToArray();
            final int returnCode = process(ffmpegArguments);
            if (returnCode == 0) {
                deleteInputFileIfNecessary();
                fireOnSuccess(outputPath);
                return outputPath;
            } else {
                fireOnError(returnCode);
                return inputPath;
            }
        }

        private void fireOnError(int returnCode) {
            if (processingListenerExists()) {
                processingListener.get().onError(returnCode);
            }
        }

        private void fireOnSuccess(String outputPath) {
            if (processingListenerExists()) {
                processingListener.get().onSuccess(outputPath);
            }
        }

        private boolean processingListenerExists() {
            return processingListener != null && processingListener.get() != null;
        }

        private String[] convertFlagsToArray() {
            final String ffmpegArguments[] = new String[flags.size()];
            for (int i = 0; i < flags.size(); i++) {
                ffmpegArguments[i] = flags.get(i);
            }
            return ffmpegArguments;
        }

        private void deleteInputFileIfNecessary() {
            if (deleteInput) {
                new File(inputPath).delete();
            }
        }
    }
}

