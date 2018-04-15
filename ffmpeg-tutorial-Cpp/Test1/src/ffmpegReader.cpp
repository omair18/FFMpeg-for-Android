// Copyright (c) 2012 The Foundry Visionmongers Ltd.  All Rights Reserved.

#ifdef _WIN32
  #include <io.h>
#endif

#define INT64_C(c) (c ## LL)
#define UINT64_C(c) (c ## ULL)

extern "C" {
#include <errno.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
}

// Set non-zero to enable tracing of file information inspected while opening a file in FFmpegFile::FFmpegFile().
// Make sure this is disabled before checking-in this file.
#define TRACE_FILE_OPEN 0

// Set non-zero to enable tracing of FFmpegFile::decode() general processing (plus error reports).
// Make sure this is disabled before checking-in this file.
#define TRACE_DECODE_PROCESS 0

// Set non-zero to enable tracing of the first few bytes of each data block in the bitstream for each frame decoded. This
// assumes a 4-byte big-endian byte count at the start of each data block, followed by that many bytes of data. There may be
// multiple data blocks per frame. Each data block represents an atomic unit of data input to the decoder (e.g. an H.264 NALU).
// It works nicely for H.264 streams in .mov files but there's no guarantee for any other format. This can be useful if you
// need to see what kind of NALUs are being decoded.
// Make sure this is disabled before checking-in this file.
#define TRACE_DECODE_BITSTREAM 0

using namespace DD::Image;

namespace
{

#define CHECK(x) \
{\
  int error = x;\
  if (error<0) {\
    setInternalError(error);\
    return;\
  }\
}\

  void SetMetaDataItem(MetaData::Bundle& metadata, const std::string& DDIkey, AVDictionary* dict, const char* ffmpegKey)
  {
    if (!dict)
      return;

    AVDictionaryEntry* dictEntry = av_dict_get(dict, ffmpegKey, NULL, 0);
    if (!dictEntry)
      return;

    metadata.setData(DDIkey, dictEntry->value);
  }

  class FFmpegFile: public RefCountedObject
  {
    struct Stream  
    {
      int _idx;                      // stream index
      AVStream* _avstream;           // video stream
      AVCodecContext* _codecContext; // video codec context
      AVCodec* _videoCodec;
      AVFrame* _avFrame;             // decoding frame
      SwsContext* _convertCtx;

      int _fpsNum;
      int _fpsDen;

      int64_t _startPTS;     // PTS of the first frame in the stream
      int64_t _frames;       // video duration in frames

      bool _ptsSeen;                      // True if a read AVPacket has ever contained a valid PTS during this stream's decode,
                                          // indicating that this stream does contain PTSs.
      int64_t AVPacket::*_timestampField; // Pointer to member of AVPacket from which timestamps are to be retrieved. Enables
                                          // fallback to using DTSs for a stream if PTSs turn out not to be available.

      int _width;
      int _height;  
      double _aspect;

      int _decodeNextFrameIn; // The 0-based index of the next frame to be fed into decode. Negative before any
                              // frames have been decoded or when we've just seeked but not yet found a relevant frame. Equal to
                              // frames_ when all available frames have been fed into decode.

      int _decodeNextFrameOut; // The 0-based index of the next frame expected out of decode. Negative before
                               // any frames have been decoded or when we've just seeked but not yet found a relevant frame. Equal to
                               // frames_ when all available frames have been output from decode.

      int _accumDecodeLatency; // The number of frames that have been input without any frame being output so far in this stream
                               // since the last seek. This is part of a guard mechanism to detect when decode appears to have
                               // stalled and ensure that FFmpegFile::decode() does not loop indefinitely.

      Stream() 
        : _idx(0)
        , _avstream(NULL)
        , _codecContext(NULL)
        , _videoCodec(NULL)
        , _avFrame(NULL)
        , _convertCtx(NULL)
        , _fpsNum(1)
        , _fpsDen(1)
        , _startPTS(0)
        , _frames(0)
        , _ptsSeen(false)
        , _timestampField(&AVPacket::pts)
        , _width(0)
        , _height(0)
        , _aspect(1.0)
        , _decodeNextFrameIn(-1)
        , _decodeNextFrameOut(-1)
        , _accumDecodeLatency(0)
      {}

      ~Stream()
      {

       if (_avFrame)
         av_free(_avFrame);

        if (_codecContext)
          avcodec_close(_codecContext);       

        if (_convertCtx)
          sws_freeContext(_convertCtx);
      }

      static void destroy(Stream* s)
      {
        delete(s);
      }

      int64_t frameToPts(int frame) const
      {
        return _startPTS + (int64_t(frame) * _fpsDen *  _avstream->time_base.den) / 
                                    (int64_t(_fpsNum) * _avstream->time_base.num);
      }

      int ptsToFrame(int64_t pts) const 
      {
        return (int64_t(pts - _startPTS) * _avstream->time_base.num *  _fpsNum) / 
                                  (int64_t(_avstream->time_base.den) * _fpsDen);
      }

      SwsContext* getConvertCtx()
      {
        if (!_convertCtx)
          _convertCtx = sws_getContext(_width, _height, _codecContext->pix_fmt, _width, _height, PIX_FMT_RGB24, SWS_BICUBIC, NULL, NULL, NULL);

        return _convertCtx;
      }

      // Return the number of input frames needed by this stream's codec before it can produce output. We expect to have to
      // wait this many frames to receive output; any more and a decode stall is detected.
      int getCodecDelay() const
      {
        return ((_videoCodec->capabilities & CODEC_CAP_DELAY) ? _codecContext->delay : 0) + _codecContext->has_b_frames;
      }
    };

    // AV structure
    AVFormatContext* _context;
    AVInputFormat*   _format;

    // store all video streams available in the file
    std::vector<Stream*> _streams;

    // reader error state
    std::string _errorMsg;  // internal decoding error string
    bool _invalidState;     // true if the reader is in an invalid state

    AVPacket _avPacket;
    
    // internal lock for multithread access
    Lock _lock;

    // set reader error
    void setError(const char* msg, const char* prefix = 0)
    {
      if (prefix) {
        _errorMsg = prefix;
        _errorMsg += msg;
#if TRACE_DECODE_PROCESS
        std::cout << "!!ERROR: " << prefix << msg << std::endl;
#endif
      }
      else {
        _errorMsg = msg;
#if TRACE_DECODE_PROCESS
        std::cout << "!!ERROR: " << msg << std::endl;
#endif
      }
      _invalidState = true;
    }

    // set FFmpeg library error
    void setInternalError(const int error, const char* prefix = 0) 
    {
      char errorBuf[1024];
      av_strerror(error, errorBuf, sizeof(errorBuf));
      setError(errorBuf, prefix);
    }

    // get stream start time
    int64_t getStreamStartTime(Stream& stream)
    {
#if TRACE_FILE_OPEN
      std::cout << "      Determining stream start PTS:" << std::endl;
#endif

      // Read from stream. If the value read isn't valid, get it from the first frame in the stream that provides such a
      // value.
      int64_t startPTS = stream._avstream->start_time;
#if TRACE_FILE_OPEN
      if (startPTS != int64_t(AV_NOPTS_VALUE))
        std::cout << "        Obtained from AVStream::start_time=";
#endif

      if (startPTS ==  int64_t(AV_NOPTS_VALUE)) {
#if TRACE_FILE_OPEN
        std::cout << "        Not specified by AVStream::start_time, searching frames..." << std::endl;
#endif

        // Seek 1st key-frame in video stream.
        avcodec_flush_buffers(stream._codecContext);

        if (av_seek_frame(_context, stream._idx, 0, 0) >= 0) {
          av_init_packet(&_avPacket);

          // Read frames until we get one for the video stream that contains a valid PTS.
          do {
            if (av_read_frame(_context, &_avPacket) < 0)  {
              // Read error or EOF. Abort search for PTS.
#if TRACE_FILE_OPEN
              std::cout << "          Read error, aborted search" << std::endl;
#endif
              break;
            }
            if (_avPacket.stream_index == stream._idx) {
              // Packet read for video stream. Get its PTS. Loop will continue if the PTS is AV_NOPTS_VALUE.
              startPTS = _avPacket.pts;
            }

            av_free_packet(&_avPacket);
          } while (startPTS ==  int64_t(AV_NOPTS_VALUE));
        }
#if TRACE_FILE_OPEN
        else
          std::cout << "          Seek error, aborted search" << std::endl;
#endif

#if TRACE_FILE_OPEN
        if (startPTS != int64_t(AV_NOPTS_VALUE))
          std::cout << "        Found by searching frames=";
#endif
      }

      // If we still don't have a valid initial PTS, assume 0. (This really shouldn't happen for any real media file, as
      // it would make meaningful playback presentation timing and seeking impossible.)
      if (startPTS ==  int64_t(AV_NOPTS_VALUE)) {
#if TRACE_FILE_OPEN
        std::cout << "        Not found by searching frames, assuming ";
#endif
        startPTS = 0;
      }

#if TRACE_FILE_OPEN
      std::cout << startPTS << " ticks, " << double(startPTS) * double(stream._avstream->time_base.num) /
                                                                double(stream._avstream->time_base.den) << " s" << std::endl;
#endif

      return startPTS;
    }

    // Get the video stream duration in frames...
    int64_t getStreamFrames(Stream& stream)
    {
#if TRACE_FILE_OPEN
      std::cout << "      Determining stream frame count:" << std::endl;
#endif

      int64_t frames = 0;

      // Obtain from movie duration if specified. This is preferred since mov/mp4 formats allow the media in
      // tracks (=streams) to be remapped in time to the final movie presentation without needing to recode the
      // underlying tracks content; the movie duration thus correctly describes the final presentation.
      if (_context->duration != 0) {
        // Annoyingly, FFmpeg exposes the movie duration converted (with round-to-nearest semantics) to units of
        // AV_TIME_BASE (microseconds in practice) and does not expose the original rational number duration
        // from a mov/mp4 file's "mvhd" atom/box. Accuracy may be lost in this conversion; a duration that was
        // an exact number of frames as a rational may end up as a duration slightly over or under that number
        // of frames in units of AV_TIME_BASE.
        // Conversion to whole frames rounds up the resulting number of frames because a partial frame is still
        // a frame. However, in an attempt to compensate for AVFormatContext's inaccurate representation of
        // duration, with unknown rounding direction, the conversion to frames subtracts 1 unit (microsecond)
        // from that duration. The rationale for this is thus:
        // * If the stored duration exactly represents an exact number of frames, then that duration minus 1
        //   will result in that same number of frames once rounded up.
        // * If the stored duration is for an exact number of frames that was rounded down, then that duration
        //   minus 1 will result in that number of frames once rounded up.
        // * If the stored duration is for an exact number of frames that was rounded up, then that duration
        //   minus 1 will result in that number of frames once rounded up, while that duration unchanged would
        //   result in 1 more frame being counted after rounding up.
        // * If the original duration in the file was not for an exact number of frames, then the movie timebase
        //   would have to be >= 10^6 for there to be any chance of this calculation resulting in the wrong
        //   number of frames. This isn't a case that I've seen. Even if that were to be the case, the original
        //   duration would have to be <= 1 microsecond greater than an exact number of frames in order to
        //   result in the wrong number of frames, which is highly improbable.
        int64_t divisor = int64_t(AV_TIME_BASE) * stream._fpsDen;
        frames = ((_context->duration - 1) * stream._fpsNum + divisor - 1) / divisor;

        // The above calculation is not reliable, because it seems in some situations (such as rendering out a mov
        // with 5 frames at 24 fps from Nuke) the duration has been rounded up to the nearest millisecond, which
        // leads to an extra frame being reported.  To attempt to work around this, compare against the number of
        // frames in the stream, and if they differ by one, use that value instead.
        int64_t streamFrames = stream._avstream->nb_frames;
        if( streamFrames > 0 && std::abs(frames - streamFrames) <= 1 ) {
          frames = streamFrames;
        }
#if TRACE_FILE_OPEN
        std::cout << "        Obtained from AVFormatContext::duration & framerate=";
#endif
      }

      // If number of frames still unknown, obtain from stream's number of frames if specified. Will be 0 if
      // unknown.
      if (!frames) {
#if TRACE_FILE_OPEN
        std::cout << "        Not specified by AVFormatContext::duration, obtaining from AVStream::nb_frames..." << std::endl;
#endif
        frames = stream._avstream->nb_frames;
#if TRACE_FILE_OPEN
        if (frames)
          std::cout << "        Obtained from AVStream::nb_frames=";
#endif
      }

      // If number of frames still unknown, attempt to calculate from stream's duration, fps and timebase.
      if (!frames) {
#if TRACE_FILE_OPEN
        std::cout << "        Not specified by AVStream::nb_frames, calculating from duration & framerate..." << std::endl;
#endif
        frames = (int64_t(stream._avstream->duration) * stream._avstream->time_base.num  * stream._fpsNum) /
                                               (int64_t(stream._avstream->time_base.den) * stream._fpsDen);
#if TRACE_FILE_OPEN
        if (frames)
          std::cout << "        Calculated from duration & framerate=";
#endif
      }

      // If the number of frames is still unknown, attempt to measure it from the last frame PTS for the stream in the
      // file relative to first (which we know from earlier).
      if (!frames) {
#if TRACE_FILE_OPEN
        std::cout << "        Not specified by duration & framerate, searching frames for last PTS..." << std::endl;
#endif

        int64_t maxPts = stream._startPTS;

        // Seek last key-frame.
        avcodec_flush_buffers(stream._codecContext);
        av_seek_frame(_context, stream._idx, stream.frameToPts(1<<29), AVSEEK_FLAG_BACKWARD);

        // Read up to last frame, extending max PTS for every valid PTS value found for the video stream.
        av_init_packet(&_avPacket);

        while (av_read_frame(_context, &_avPacket) >= 0) {
          if (_avPacket.stream_index == stream._idx && _avPacket.pts != int64_t(AV_NOPTS_VALUE) && _avPacket.pts > maxPts)
            maxPts = _avPacket.pts;
          av_free_packet(&_avPacket);
        }
#if TRACE_FILE_OPEN
        std::cout << "          Start PTS=" << stream._startPTS << ", Max PTS found=" << maxPts << std::endl;
#endif

        // Compute frame range from min to max PTS. Need to add 1 as both min and max are at starts of frames, so stream
        // extends for 1 frame beyond this.
        frames = 1 + stream.ptsToFrame(maxPts);
#if TRACE_FILE_OPEN
        std::cout << "        Calculated from frame PTS range=";
#endif
      }

#if TRACE_FILE_OPEN
      std::cout << frames << std::endl;
#endif

      return frames;
    }

  public:

    typedef RefCountedPtr<FFmpegFile> Ptr;

    // constructor
    FFmpegFile(const char* filename)
      : _context(NULL)
      , _format(NULL)
      , _invalidState(false)      
    {
      // FIXME_GC: shouldn't the plugin be passed the filename without the prefix?
      int offset = 0;
      if (std::string(filename).find("ffmpeg:") != std::string::npos)
        offset = 7;

#if TRACE_FILE_OPEN
      std::cout << "ffmpegReader=" << this << "::c'tor(): filename=" << filename + offset << std::endl;
#endif
      
      CHECK( avformat_open_input(&_context, filename + offset, _format, NULL) );
      CHECK( avformat_find_stream_info(_context, NULL) );

#if TRACE_FILE_OPEN
      std::cout << "  " << _context->nb_streams << " streams:" << std::endl;
#endif

      // fill the array with all available video streams
      bool unsuported_codec = false;

      // find all streams that the library is able to decode
      for (unsigned i = 0; i < _context->nb_streams; ++i) {
#if TRACE_FILE_OPEN
        std::cout << "    FFmpeg stream index " << i << ": ";
#endif
        AVStream* avstream = _context->streams[i];

        // be sure to have a valid stream
        if (!avstream || !avstream->codec) {
#if TRACE_FILE_OPEN
          std::cout << "No valid stream or codec, skipping..." << std::endl;
#endif
          continue;
        }

        // considering only video streams, skipping audio
        if (avstream->codec->codec_type != AVMEDIA_TYPE_VIDEO) {
#if TRACE_FILE_OPEN
          std::cout << "Not a video stream, skipping..." << std::endl;
#endif
          continue;
        }

        // find the codec
        AVCodec* videoCodec = avcodec_find_decoder(avstream->codec->codec_id);
        if (videoCodec == NULL) {
#if TRACE_FILE_OPEN
          std::cout << "Decoder not found, skipping..." << std::endl;
#endif
          continue;
        }

        // skip codec from the black list
        if (Foundry::Nuke::isCodecBlacklistedForReading(videoCodec->name)) {
#if TRACE_FILE_OPEN
          std::cout << "Decoder \"" << videoCodec->name << "\" blacklisted, skipping..." << std::endl;
#endif
          unsuported_codec = true;
          continue;
        }

        // skip if the codec can't be open
        if (avcodec_open2(avstream->codec, videoCodec, NULL) < 0) {
#if TRACE_FILE_OPEN
          std::cout << "Decoder \"" << videoCodec->name << "\" failed to open, skipping..." << std::endl;
#endif
          continue;
        }

#if TRACE_FILE_OPEN
        std::cout << "Video decoder \"" << videoCodec->name << "\" opened ok, getting stream properties:" << std::endl;
#endif

        Stream* stream = new Stream();
        stream->_idx = i;
        stream->_avstream = avstream;
        stream->_codecContext = avstream->codec;
        stream->_videoCodec = videoCodec;
        stream->_avFrame = avcodec_alloc_frame();

#if TRACE_FILE_OPEN
        std::cout << "      Timebase=" << avstream->time_base.num << "/" << avstream->time_base.den << " s/tick" << std::endl;
        std::cout << "      Duration=" << avstream->duration << " ticks, " <<
                                          double(avstream->duration) * double(avstream->time_base.num) /
                                                                       double(avstream->time_base.den) << " s" << std::endl;
#endif

        // If FPS is specified, record it. 
        // Otherwise assume 1 fps (default value).
        if ( avstream->r_frame_rate.num != 0 &&  avstream->r_frame_rate.den != 0 ) {
          stream->_fpsNum = avstream->r_frame_rate.num;
          stream->_fpsDen = avstream->r_frame_rate.den;
#if TRACE_FILE_OPEN
          std::cout << "      Framerate=" << stream->_fpsNum << "/" << stream->_fpsDen << ", " <<
                       double(stream->_fpsNum) / double(stream->_fpsDen) << " fps" << std::endl;
#endif
        } 
#if TRACE_FILE_OPEN
        else
          std::cout << "      Framerate unspecified, assuming 1 fps" << std::endl;
#endif

        stream->_width  = avstream->codec->width;
        stream->_height = avstream->codec->height;
#if TRACE_FILE_OPEN
        std::cout << "      Image size=" << stream->_width << "x" << stream->_height << std::endl;
#endif

        // set aspect ratio
        if (stream->_avstream->sample_aspect_ratio.num) {
          stream->_aspect = av_q2d(stream->_avstream->sample_aspect_ratio);
#if TRACE_FILE_OPEN
          std::cout << "      Aspect ratio (from stream)=" << stream->_aspect << std::endl;
#endif
        }
        else if (stream->_codecContext->sample_aspect_ratio.num) {
          stream->_aspect = av_q2d(stream->_codecContext->sample_aspect_ratio);
#if TRACE_FILE_OPEN
          std::cout << "      Aspect ratio (from codec)=" << stream->_aspect << std::endl;
#endif
        }
#if TRACE_FILE_OPEN
        else
          std::cout << "      Aspect ratio unspecified, assuming " << stream->_aspect << std::endl;
#endif

        // set stream start time and numbers of frames
        stream->_startPTS = getStreamStartTime(*stream);
        stream->_frames   = getStreamFrames(*stream);
          
        // save the stream
        _streams.push_back(stream);
      }

      if (_streams.empty())
        setError( unsuported_codec ? "unsupported codec..." : "unable to find video stream" );
    }

    // destructor
    ~FFmpegFile()
    {
      // force to close all resources needed for all streams
      std::for_each(_streams.begin(), _streams.end(), Stream::destroy);

      if (_context)
        avformat_close_input(&_context);
    }

    // get the internal error string
    const char* error() const
    {
      return _errorMsg.c_str();
    }

    // return true if the reader can't decode the frame
    bool invalid() const
    {
      return _invalidState;
    }

    // return the numbers of streams supported by the reader
    unsigned streams() const
    {
      return _streams.size();
    }

    // decode a single frame into the buffer thread safe
    bool decode(unsigned char* buffer, unsigned frame, unsigned streamIdx = 0)
    {
      Guard guard(_lock);

      if (streamIdx >= _streams.size())
        return false;

      // get the stream
      Stream* stream = _streams[streamIdx];

      // Translate from the 1-based frames expected by Nuke to 0-based frame offsets for use in the rest of this code.
      int desiredFrame = frame - 1;

      // Early-out if out-of-range frame requested.
      if (desiredFrame < 0 || desiredFrame >= stream->_frames)
        return false;

#if TRACE_DECODE_PROCESS
      std::cout << "ffmpegReader=" << this << "::decode(): desiredFrame=" << desiredFrame << ", videoStream=" << streamIdx << ", streamIdx=" << stream->_idx << std::endl;
#endif

      // Number of read retries remaining when decode stall is detected before we give up (in the case of post-seek stalls,
      // such retries are applied only after we've searched all the way back to the start of the file and failed to find a
      // successful start point for playback)..
      //
      // We have a rather annoying case with a small subset of media files in which decode latency (between input and output
      // frames) will exceed the maximum above which we detect decode stall at certain frames on the first pass through the
      // file but those same frames will decode succesfully on a second attempt. The root cause of this is not understood but
      // it appears to be some oddity of FFmpeg. While I don't really like it, retrying decode enables us to successfully
      // decode those files rather than having to fail the read.
      int retriesRemaining = 1;

      // Whether we have just performed a seek and are still awaiting the first decoded frame after that seek. This controls
      // how we respond when a decode stall is detected.
      //
      // One cause of such stalls is when a file contains incorrect information indicating that a frame is a key-frame when it
      // is not; a seek may land at such a frame but the decoder will not be able to start decoding until a real key-frame is
      // reached, which may be a long way in the future. Once a frame has been decoded, we will expect it to be the first frame
      // input to decode but it will actually be the next real key-frame found, leading to subsequent frames appearing as
      // earlier frame numbers and the movie ending earlier than it should. To handle such cases, when a stall is detected
      // immediately after a seek, we seek to the frame before the previous seek's landing frame, allowing us to search back
      // through the movie for a valid key frame from which decode commences correctly; if this search reaches the beginning of
      // the movie, we give up and fail the read, thus ensuring that this method will exit at some point.
      //
      // Stalls once seeking is complete and frames are being decoded are handled differently; these result in immediate read
      // failure.
      bool awaitingFirstDecodeAfterSeek = false;

      // If the frame we want is not the next one to be decoded, seek to the keyframe before/at our desired frame. Set the last
      // seeked frame to indicate that we need to synchronise frame indices once we've read the first frame of the video stream,
      // since we don't yet know which frame number the seek will land at. Also invalidate current indices, reset accumulated
      // decode latency and record that we're awaiting the first decoded frame after a seek.
      int lastSeekedFrame = -1; // 0-based index of the last frame to which we seeked when seek in progress / negative when no
                                // seek in progress,

      if (desiredFrame != stream->_decodeNextFrameOut) {
#if TRACE_DECODE_PROCESS
        std::cout << "  Next frame expected out=" << stream->_decodeNextFrameOut << ", Seeking to desired frame" << std::endl;
#endif

        lastSeekedFrame = desiredFrame;
        stream->_decodeNextFrameIn  = -1;
        stream->_decodeNextFrameOut = -1;
        stream->_accumDecodeLatency = 0;
        awaitingFirstDecodeAfterSeek = true;

        avcodec_flush_buffers(stream->_codecContext);
        int error = av_seek_frame(_context, stream->_idx, stream->frameToPts(desiredFrame), AVSEEK_FLAG_BACKWARD);
        if (error < 0) {
          // Seek error. Abort attempt to read and decode frames.
          setInternalError(error, "FFmpeg Reader failed to seek frame: ");
          return false;
        }
      }
#if TRACE_DECODE_PROCESS
      else
        std::cout << "  Next frame expected out=" << stream->_decodeNextFrameOut << ", No seek required" << std::endl;
#endif

      av_init_packet(&_avPacket);

      // Loop until the desired frame has been decoded. May also break from within loop on failure conditions where the
      // desired frame will never be decoded.
      bool hasPicture = false;
      do {
        bool decodeAttempted = false;
        int frameDecoded = 0;

        // If the next frame to decode is within range of frames (or negative implying invalid; we've just seeked), read
        // a new frame from the source file and feed it to the decoder if it's for the video stream.
        if (stream->_decodeNextFrameIn < stream->_frames) {
#if TRACE_DECODE_PROCESS
          std::cout << "  Next frame expected in=";
          if (stream->_decodeNextFrameIn >= 0)
            std::cout << stream->_decodeNextFrameIn;
          else
            std::cout << "unknown";
#endif

          int error = av_read_frame(_context, &_avPacket);
          if (error < 0) {
            // Read error. Abort attempt to read and decode frames.
#if TRACE_DECODE_PROCESS
            std::cout << ", Read failed" << std::endl;
#endif
            setInternalError(error, "FFmpeg Reader failed to read frame: ");
            break;
          }
#if TRACE_DECODE_PROCESS
          std::cout << ", Read OK, Packet data:" << std::endl;
          std::cout << "    PTS=" << _avPacket.pts <<
                       ", DTS=" << _avPacket.dts <<
                       ", Duration=" << _avPacket.duration <<
                       ", KeyFrame=" << ((_avPacket.flags & AV_PKT_FLAG_KEY) ? 1 : 0) <<
                       ", Corrupt=" << ((_avPacket.flags & AV_PKT_FLAG_CORRUPT) ? 1 : 0) <<
                       ", StreamIdx=" << _avPacket.stream_index <<
                       ", PktSize=" << _avPacket.size;
#endif

          // If the packet read belongs to the video stream, synchronise frame indices from it if required and feed it
          // into the decoder.
          if (_avPacket.stream_index == stream->_idx) {
#if TRACE_DECODE_PROCESS
            std::cout << ", Relevant stream" << std::endl;
#endif

            // If the packet read has a valid PTS, record that we have seen a PTS for this stream.
            if (_avPacket.pts != int64_t(AV_NOPTS_VALUE))
              stream->_ptsSeen = true;

            // If a seek is in progress, we need to synchronise frame indices if we can...
            if (lastSeekedFrame >= 0) {
#if TRACE_DECODE_PROCESS
              std::cout << "    In seek (" << lastSeekedFrame << ")";
#endif

              // Determine which frame the seek landed at, using whichever kind of timestamp is currently selected for this
              // stream. If there's no timestamp available at that frame, we can't synchronise frame indices to know which
              // frame we're first going to decode, so we need to seek back to an earlier frame in hope of obtaining a
              // timestamp. Likewise, if the landing frame is after the seek target frame (this can happen, presumably a bug
              // in FFmpeg seeking), we need to seek back to an earlier frame so that we can start decoding at or before the
              // desired frame.
              int landingFrame;
              if (_avPacket.*stream->_timestampField == int64_t(AV_NOPTS_VALUE) ||
                  (landingFrame = stream->ptsToFrame(_avPacket.*stream->_timestampField)) > lastSeekedFrame) {
#if TRACE_DECODE_PROCESS
                std::cout << ", landing frame not found";
                if (_avPacket.*stream->_timestampField == int64_t(AV_NOPTS_VALUE))
                  std::cout << " (no timestamp)";
                else
                  std::cout << " (landed after target at " << landingFrame << ")";
#endif

                // Wind back 1 frame from last seeked frame. If that takes us to before frame 0, we're never going to be
                // able to synchronise using the current timestamp source...
                if (--lastSeekedFrame < 0) {
#if TRACE_DECODE_PROCESS
                  std::cout << ", can't seek before start";
#endif

                  // If we're currently using PTSs to determine the landing frame and we've never seen a valid PTS for any
                  // frame from this stream, switch to using DTSs and retry the read from the initial desired frame.
                  if (stream->_timestampField == &AVPacket::pts && !stream->_ptsSeen) {
                    stream->_timestampField = &AVPacket::dts;
                    lastSeekedFrame = desiredFrame;
#if TRACE_DECODE_PROCESS
                    std::cout << ", PTSs absent, switching to use DTSs";
#endif
                  }
                  // Otherwise, failure to find a landing point isn't caused by an absence of PTSs from the file or isn't
                  // recovered by using DTSs instead. Something is wrong with the file. Abort attempt to read and decode frames.
                  else {
#if TRACE_DECODE_PROCESS
                    if (stream->_timestampField == &AVPacket::dts)
                      std::cout << ", search using DTSs failed";
                    else
                      std::cout << ", PTSs present";
                    std::cout << ",  giving up" << std::endl;
#endif
                    setError("FFmpeg Reader failed to find timing reference frame, possible file corruption");
                    break;
                  }
                }

                // Seek to the new frame. By leaving the seek in progress, we will seek backwards frame by frame until we
                // either successfully synchronise frame indices or give up having reached the beginning of the stream.
#if TRACE_DECODE_PROCESS
                std::cout << ", seeking to " << lastSeekedFrame << std::endl;
#endif
                avcodec_flush_buffers(stream->_codecContext);
                error = av_seek_frame(_context, stream->_idx, stream->frameToPts(lastSeekedFrame), AVSEEK_FLAG_BACKWARD);
                if (error < 0) {
                  // Seek error. Abort attempt to read and decode frames.
                  setInternalError(error, "FFmpeg Reader failed to seek frame: ");
                  break;
                }
              }
              // Otherwise, we have a valid landing frame, so set that as the next frame into and out of decode and set
              // no seek in progress.
              else {
#if TRACE_DECODE_PROCESS
                std::cout << ", landed at " << landingFrame << std::endl;
#endif
                stream->_decodeNextFrameOut = stream->_decodeNextFrameIn = landingFrame;
                lastSeekedFrame = -1;
              }
            }

            // If there's no seek in progress, feed this frame into the decoder.
            if (lastSeekedFrame < 0) {
#if TRACE_DECODE_BITSTREAM              
              std::cout << "  Decoding input frame " << stream->_decodeNextFrameIn << " bitstream:" << std::endl;
              uint8_t *data = _avPacket.data;
              uint32_t remain = _avPacket.size;
              while (remain > 0) {
                if (remain < 4) {
                  std::cout << "    Insufficient remaining bytes (" << remain << ") for block size at BlockOffset=" << (data - _avPacket.data) << std::endl;
                  remain = 0;
                }
                else {
                  uint32_t blockSize = (data[0] << 24) | (data[1] << 16) | (data[2] << 8) | data[3];
                  data += 4;
                  remain -= 4;
                  std::cout << "    BlockOffset=" << (data - _avPacket.data) << ", Size=" << blockSize;
                  if (remain < blockSize) {
                    std::cout << ", Insufficient remaining bytes (" << remain << ")" << std::endl;
                    remain = 0;
                  }
                  else
                  {
                    std::cout << ", Bytes:";
                    int count = (blockSize > 16 ? 16 : blockSize);
                    for (int offset = 0; offset < count; offset++)
                    {
                      static const char hexTable[] = "0123456789ABCDEF";
                      uint8_t byte = data[offset];
                      std::cout << ' ' << hexTable[byte >> 4] << hexTable[byte & 0xF];
                    }
                    std::cout << std::endl;
                    data += blockSize;
                    remain -= blockSize;
                  }
                }
              }
#elif TRACE_DECODE_PROCESS
              std::cout << "  Decoding input frame " << stream->_decodeNextFrameIn << std::endl;
#endif

              // Advance next frame to input.
              ++stream->_decodeNextFrameIn;

              // Decode the frame just read. frameDecoded indicates whether a decoded frame was output.
              decodeAttempted = true;
              error = avcodec_decode_video2(stream->_codecContext, stream->_avFrame, &frameDecoded, &_avPacket);
              if (error < 0) {
                // Decode error. Abort attempt to read and decode frames.
                setInternalError(error, "FFmpeg Reader failed to decode frame: ");
                break;
              }
            }
          }
#if TRACE_DECODE_PROCESS
          else
            std::cout << ", Irrelevant stream" << std::endl;
#endif
        }

        // If the next frame to decode is out of frame range, there's nothing more to read and the decoder will be fed
        // null input frames in order to obtain any remaining output.
        else {
#if TRACE_DECODE_PROCESS
         std::cout << "  No more frames to read, pumping remaining decoder output" << std::endl;
#endif

          // Obtain remaining frames from the decoder. pkt_ contains NULL packet data pointer and size at this point,
          // required to pump out remaining frames with no more input. frameDecoded indicates whether a decoded frame
          // was output.
          decodeAttempted = true;
          int error = avcodec_decode_video2(stream->_codecContext, stream->_avFrame, &frameDecoded, &_avPacket);
          if (error < 0) {
            // Decode error. Abort attempt to read and decode frames.
            setInternalError(error, "FFmpeg Reader failed to decode frame: ");
            break;
          }
        }

        // If a frame was decoded, ...
        if (frameDecoded) {
#if TRACE_DECODE_PROCESS
          std::cout << "    Frame decoded=" << stream->_decodeNextFrameOut;
#endif

          // Now that we have had a frame decoded, we know that seek landed at a valid place to start decode. Any decode
          // stalls detected after this point will result in immediate decode failure.
          awaitingFirstDecodeAfterSeek = false;

          // If the frame just output from decode is the desired one, get the decoded picture from it and set that we
          // have a picture.
          if (stream->_decodeNextFrameOut == desiredFrame) {
#if TRACE_DECODE_PROCESS
            std::cout << ", is desired frame" << std::endl;
#endif
            AVPicture output;
            avpicture_fill(&output, buffer, PIX_FMT_RGB24, stream->_width, stream->_height);

            sws_scale(stream->getConvertCtx(), stream->_avFrame->data, stream->_avFrame->linesize, 0,  stream->_height, output.data, output.linesize);

            hasPicture = true;
          }
#if TRACE_DECODE_PROCESS
          else
            std::cout << ", is not desired frame (" << desiredFrame << ")" << std::endl;
#endif

          // Advance next output frame expected from decode.
          ++stream->_decodeNextFrameOut;
        }
        // If no frame was decoded but decode was attempted, determine whether this constitutes a decode stall and handle if so.
        else if (decodeAttempted) {
          // Failure to get an output frame for an input frame increases the accumulated decode latency for this stream.
          ++stream->_accumDecodeLatency;

#if TRACE_DECODE_PROCESS
          std::cout << "    No frame decoded, accumulated decode latency=" << stream->_accumDecodeLatency << ", max allowed latency=" << stream->getCodecDelay() << std::endl;
#endif
          
          // If the accumulated decode latency exceeds the maximum delay permitted for this codec at this time (the delay can
          // change dynamically if the codec discovers B-frames mid-stream), we've detected a decode stall.
          if (stream->_accumDecodeLatency > stream->getCodecDelay()) {
            int seekTargetFrame; // Target frame for any seek we might perform to attempt decode stall recovery.
            
            // Handle a post-seek decode stall.
            if (awaitingFirstDecodeAfterSeek) {
              // If there's anywhere in the file to seek back to before the last seek's landing frame (which can be found in
              // stream->_decodeNextFrameOut, since we know we've not decoded any frames since landing), then set up a seek to
              // the frame before that landing point to try to find a valid decode start frame earlier in the file.
              if (stream->_decodeNextFrameOut > 0) {
                seekTargetFrame = stream->_decodeNextFrameOut - 1;
#if TRACE_DECODE_PROCESS
                std::cout << "    Post-seek stall detected, trying earlier decode start, seeking frame " << seekTargetFrame << std::endl;
#endif
              }
              // Otherwise, there's nowhere to seek back. If we have any retries remaining, use one to attempt the read again,
              // starting from the desired frame.
              else if (retriesRemaining > 0) {
                --retriesRemaining;
                seekTargetFrame = desiredFrame;
#if TRACE_DECODE_PROCESS
                std::cout << "    Post-seek stall detected, at start of file, retrying from desired frame " << seekTargetFrame << std::endl;
#endif
              }
              // Otherwise, all we can do is to fail the read so that this method exits safely.
              else {
#if TRACE_DECODE_PROCESS
                std::cout << "    Post-seek STALL DETECTED, at start of file, no more retries, failed read" << std::endl;
#endif
                setError("FFmpeg Reader failed to find decode reference frame, possible file corruption");
                break;
              }
            }
            // Handle a mid-decode stall. All we can do is to fail the read so that this method exits safely.
            else {
              // If we have any retries remaining, use one to attempt the read again, starting from the desired frame.
              if (retriesRemaining > 0) {
                --retriesRemaining;
                seekTargetFrame = desiredFrame;
#if TRACE_DECODE_PROCESS
                std::cout << "    Mid-decode stall detected, retrying from desired frame " << seekTargetFrame << std::endl;
#endif
              }
              // Otherwise, all we can do is to fail the read so that this method exits safely.
              else {
#if TRACE_DECODE_PROCESS
                std::cout << "    Mid-decode STALL DETECTED, no more retries, failed read" << std::endl;
#endif
                setError("FFmpeg Reader detected decoding stall, possible file corruption");
                break;
              }
            }

            // If we reach here, seek to the target frame chosen above in an attempt to recover from the decode stall.
            lastSeekedFrame = seekTargetFrame;
            stream->_decodeNextFrameIn  = -1;
            stream->_decodeNextFrameOut = -1;
            stream->_accumDecodeLatency = 0;
            awaitingFirstDecodeAfterSeek = true;

            avcodec_flush_buffers(stream->_codecContext);
            int error = av_seek_frame(_context, stream->_idx, stream->frameToPts(seekTargetFrame), AVSEEK_FLAG_BACKWARD);
            if (error < 0) {
              // Seek error. Abort attempt to read and decode frames.
              setInternalError(error, "FFmpeg Reader failed to seek frame: ");
              break;
            }
          }
        }

        av_free_packet(&_avPacket);
      } while (!hasPicture);

#if TRACE_DECODE_PROCESS
      std::cout << "<-validPicture=" << hasPicture << " for frame " << desiredFrame << std::endl;
#endif

      // If read failed, reset the next frame expected out so that we seek and restart decode on next read attempt. Also free
      // the AVPacket, since it won't have been freed at the end of the above loop (we reach here by a break from the main
      // loop when hasPicture is false).
      if (!hasPicture) {
        if (_avPacket.size > 0)
          av_free_packet(&_avPacket);
        stream->_decodeNextFrameOut = -1;
      }

      return hasPicture;
    }

    // fill the metada information of a particular stream
    bool metadata(DD::Image::MetaData::Bundle& metadata, unsigned streamIdx = 0)
    {
      Guard guard(_lock);

      if (streamIdx >= _streams.size())
        return false;

      // get the stream
      Stream* stream = _streams[streamIdx];

      // 8 bits per channel (Fix for Bug 29986. data_ is unsigned char)
      metadata.setData(MetaData::DEPTH, MetaData::DEPTH_8);

      AVDictionary* avmetadata = _context->metadata;

      SetMetaDataItem(metadata, MetaData::CREATOR, avmetadata, "author");
      SetMetaDataItem(metadata, MetaData::COMMENT, avmetadata, "comment");
      SetMetaDataItem(metadata, MetaData::PROJECT, avmetadata, "album");
      SetMetaDataItem(metadata, MetaData::COPYRIGHT, avmetadata, "copyright");

      metadata.setData("ffmpeg/num_streams", _context->nb_streams);

      if (stream->_videoCodec->name!=NULL)
        metadata.setData("ffmpeg/codec/codecName", stream->_videoCodec->name);

      metadata.setData(MetaData::FRAME_RATE, double(stream->_fpsNum) / double(stream->_fpsDen));
      metadata.setData("ffmpeg/codec/timecodeFrameStart", (unsigned int)stream->_codecContext->timecode_frame_start);
      metadata.setData("ffmpeg/codec/startTime", (unsigned int)_context->start_time);

      return true;
    }

    // get stream information
    bool info( int& width, 
               int& height, 
               double& aspect, 
               int& frames, 
               unsigned streamIdx = 0)
    {
      Guard guard(_lock);

      if (streamIdx >= _streams.size())
        return false;

      // get the stream
      Stream* stream = _streams[streamIdx];

      width  = stream->_width;
      height = stream->_height;
      aspect = stream->_aspect;
      frames = stream->_frames;

      return true;
    }
    
  };

  // Keeps track of all FFmpegFile mapped against file name.
  class FFmpegFileManager
  {
    typedef std::map<std::string, FFmpegFile::Ptr> ReaderMap;

#if !defined(FN_HIERO)
    ReaderMap _readers;

    // internal lock
    Lock _lock;
#endif

    // A lock manager function for FFmpeg, enabling it to use mutexes managed by this reader. Pass to av_lockmgr_register().
    static int FFmpegLockManager(void** mutex, enum AVLockOp op)
    {
      switch (op) {
      case AV_LOCK_CREATE: // Create a mutex.
        try {
          *mutex = static_cast< void* >(new DD::Image::Lock());
          return 0;
        }
        catch(...) {
          // Return error if mutex creation failed.
          return 1;
        }

      case AV_LOCK_OBTAIN: // Lock the specified mutex.
        try {
          static_cast< DD::Image::Lock* >(*mutex)->lock();
          return 0;
        }
        catch(...) {
          // Return error if mutex lock failed.
          return 1;
        }

      case AV_LOCK_RELEASE: // Unlock the specified mutex.
        // Mutex unlock can't fail.
        static_cast< DD::Image::Lock* >(*mutex)->unlock();
        return 0;

      case AV_LOCK_DESTROY: // Destroy the specified mutex.
        // Mutex destruction can't fail.
        delete static_cast< DD::Image::Lock* >(*mutex);
        *mutex = 0;
        return 0;

      default: // Unknown operation.
        mFnAssert(false);
        return 1;
      }
    }

  public:

    // singleton
    static FFmpegFileManager s_readerManager;

    // constructor
    FFmpegFileManager()
    {
      av_log_set_level(AV_LOG_WARNING);
      av_register_all();

      // Register a lock manager callback with FFmpeg, providing it the ability to use mutex locking around
      // otherwise non-thread-safe calls.
      av_lockmgr_register(FFmpegLockManager);
    }

    // get a specific reader 
    FFmpegFile::Ptr get(const char* filename)
    {
      // For performance reason and for different use cases Hero prefer to decode multiple time the same file.
      // That means allocating more resources by gurantee best performance when the application needs
      // to seek in consecutive way from different position in the same video sequence.
#if defined(FN_HIERO)
      FFmpegFile::Ptr retVal;
      retVal.allocate(filename);
      return retVal;
#else
      Guard guad(_lock);
      FFmpegFile::Ptr retVal;

      ReaderMap::iterator it = _readers.find(filename);
      if (it == _readers.end()) {
        retVal.allocate(filename);
        _readers.insert(std::make_pair(std::string(filename), retVal));
      } 
      else {
        retVal = it->second;
      }
      return retVal;
#endif
    }

    // release a specific reader
    void release(const char* filename)
    {
#if !defined(FN_HIERO)
      Guard guad(_lock);
      ReaderMap::iterator it = _readers.find(filename);

      if (it != _readers.end()) {
        if (it->second.refcount() == 1) {
          _readers.erase(it);
        }
      }
#endif
    }
  };

  FFmpegFileManager FFmpegFileManager::s_readerManager;
}

class ffmpegReader : public Reader, public DD::Image::IBufferFillHeap
{
public:
  explicit ffmpegReader(Read* iop);
  ~ffmpegReader();

  virtual bool videosequence() const { return true; }

  void engine(int y, int x, int r, ChannelMask channels, Row& row);
  void open();
  
private:
  MetaData::Bundle meta;
  static const Reader::Description d;

public:
  const MetaData::Bundle& fetchMetaData(const char* key) { return meta; }

private:
        
  // redefined from DD::Image::IBufferFill 
  virtual bool fillBuffer(void* data);

  virtual void memoryInfo(DD::Image::Memory::MemoryInfoArray& output, const void* restrict_to) const
  {
    if (restrict_to && iop->node() != (const Node*)restrict_to)
      return;

      Memory::MemoryInfo memInfo = Memory::MemoryInfo(iop, _data.size());

      std::stringstream buf;
      buf << w() << "x" << h();
      memInfo.addUserData("ImgSize", buf);

     output.push_back(memInfo);
  }
   
private:

  FFmpegFile::Ptr _reader;
  DD::Image::MemoryBuffer _data;    // decoding buffer
  size_t _memNeeded;                // memory needed for decoding a single frame
  
  int  _numFrames;                  // number of frames in the selected stream
  int  _currentFrame;               // current decoding frame
};

ffmpegReader::ffmpegReader(Read* iop)
  : Reader(iop)
  , IBufferFillHeap("ffmpegReader")
  , _data(this)
  , _memNeeded(0)
  , _numFrames(0)
  , _currentFrame(0)  
{
  // get the reader from the FFmpeg reader  
  _reader = FFmpegFileManager::s_readerManager.get(iop->filename());

  if (_reader->invalid()) {
    iop->internalError(_reader->error());
    return;
  }

  // set iop info size
  int width, height, frames;
  double aspect; 

  // get metadata from file
  if (_reader->info(width, height, aspect, frames)) {
    info_.channels(Mask_RGBA);
    set_info(width, height, 3, aspect);
    info_.first_frame(1);
    info_.last_frame(frames);
    
    _numFrames = frames;
    _memNeeded = width * height * 3;   
  }

  // get stream 0 metadata
  _reader->metadata(meta);
    
  // set buffer size
  _data.resize(_memNeeded);
}

ffmpegReader::~ffmpegReader()
{
  // unref the reader
  _reader.clear();

  // release the reader if is not used anymore
  FFmpegFileManager::s_readerManager.release(iop->filename());
}

bool ffmpegReader::fillBuffer(void* buffer)
{
  if (buffer == NULL) {
    iop->internalError("FFmpeg Reader has run out of memory", _memNeeded);
    return false;
  }
  
  if (!_reader->decode( static_cast<unsigned char*>(buffer), _currentFrame )) {  
    iop->internalError(_reader->error());
    return false;
  }
  
  return true;  
}

void ffmpegReader::engine(int y, int x, int rx, ChannelMask channels, Row& out)
{
  MemoryBufferGuard guard(_data);
  if (!guard.buffer()) {
    // for some reason the buffer is not available and the Iop error
    // is already called inside fillBuffer method.
    return;
  }

  foreach ( z, channels ) {
    float* TO = out.writable(z) + x;
    unsigned char* FROM = static_cast<unsigned char*>(guard.buffer());
    FROM += (height() - y - 1) * width() * 3;
    FROM += x * 3;
    from_byte(z, TO, FROM + z - 1, NULL, rx - x, 3);
  }
}

void ffmpegReader::open()
{
  int f = frame();

  if (f < info_.first_frame())
    f = info_.first_frame();
  if (f > info_.last_frame())
    f = info_.last_frame();
  if (f != _currentFrame)
    _data.invalidate();

  _currentFrame = f;       
}

static Reader* build(Read* iop, int fd, const unsigned char* b, int n)
{
  ::close(fd);
  return new ffmpegReader(iop);
}

static bool test(int fd, const unsigned char* block, int length)
{
  return true;
}

const Reader::Description ffmpegReader::d("ffmpeg\0", build, test);
