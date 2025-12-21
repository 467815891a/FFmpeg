FFmpeg-WHEP
================
This is a fork of the [FFmpeg](https://ffmpeg.org) project that adds support for the  [WHEP](https://datatracker.ietf.org/doc/html/draft-ietf-wish-whep-02) (WebRTC-HTTP Egress Protocol) using [libdatachannel](https://github.com/paullouisageneau/libdatachannel).

# Supported Codecs
- Video: H264 / H265 / VP8 / VP9
- Audio: OPUS / PCMA / PCMU / G722

# Usage
```bash
ffplay -f whep -token <token> http://myip/api/whep_endpoint
```
or
```bash
ffplay  -token <token> whep://myip/api/whep_endpoint
```

# Building
## Genral Guide
1. Build **libdatachannel** by following the instructions in its [BUILDING.md](https://github.com/467815891a/libdatachannel/blob/master/BUILDING.md).

2. Install **libdatachannel** to the system directory using `make install`.

3. Clone this repository and build it following the [FFmpeg Compilation Guide](https://trac.ffmpeg.org/wiki/CompilationGuide).
Be sure to add `--enable-libdatachannel` when running the `./configure` script.

## MinGW-w64 (Static Build)
1. Open MinGW-w64 Shell and install compiler:
```bash
pacman -Syu && pacman -S git diffutils mingw-w64-x86_64-nasm perl mingw-w64-x86_64-toolchain mingw-w64-x86_64-SDL2 mingw-w64-x86_64-openssl
```

2. Clone repos and make new folder
```bash
git clone --recursive https://github.com/467815891a/libdatachannel.git
git clone https://github.com/467815891a/FFmpeg-WHEP.git
mkdir install
```
3. Build and install libdatachannel
```bash
cd libdatachannel
cmake -B build -G "MinGW Makefiles" -DCMAKE_TOOLCHAIN_FILE=./mingw64-toolchain.cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=./../installed -DOPENSSL_ROOT_DIR="C:/msys64/mingw64" -DCMAKE_C_FLAGS="-Wno-format" -DBUILD_SHARED_LIBS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DNO_TESTS=ON -DNO_EXAMPLES=ON -DCMAKE_CXX_FLAGS="-static-libstdc++ -static-libgcc -DDATA_CHANNEL_STATIC_DEFINE -DRTC_STATIC_DEFINE"  -DCMAKE_FIND_LIBRARY_SUFFIXES=".a"  -DOPENSSL_USE_STATIC_LIBS=TRUE -DCMAKE_CXX_STANDARD=11  -DCMAKE_SHARED_LINKER_FLAGS="-static -static-libstdc++ -static-libgcc -lgcc_eh"
cd build
mingw32-make -j4
mingw32-make install
```
 
4. Build FFmpeg (only build we need)
```bash
cd FFmpeg-WHEP
./configure --prefix=../installed/ --target-os=mingw32 --arch=x86_64 --enable-static --disable-shared --disable-w32threads --enable-pthreads --disable-muxers --disable-indevs --disable-encoders --disable-protocols  --disable-filters --disable-demuxers --disable-bsfs --disable-filters --disable-mediafoundation --disable-amf --enable-filter=aresample --disable-parsers --disable-decoders --pkg-config-flags="--static" --enable-gpl  --extra-ldflags="-static -L../installed/lib -L/mingw64/lib -static-libstdc++ -static-libgcc -O2 -s"  --extra-cflags="-I../installed/include -I/mingw64/include -O2" --enable-sdl2 --enable-lcms2  --enable-libdatachannel --enable-protocol=file,http,https,tcp,udp,tls,rtp,rtmp,hls,whep --enable-demuxer=wav,au,alaw,mpegts,flv,h264,hevc,matroska,mjpeg,mpjpeg,image_jpeg_pipe,rtsp,rtp,sdp,mp3,aac,whep --enable-demuxer=wav,au,alaw,mpegts,flv,h264,hevc,matroska,mjpeg,mpjpeg,image_jpeg_pipe,rtsp,rtp,sdp,mp3,aac --enable-decoder=h264,hevc,vp8,vp9,mjpeg,aac,mp3,vorbis,opus,pcm_alaw,pcm_mulaw,pcm_s16le  --enable-bsf=h264_mp4toannexb,hevc_mp4toannexb,mjpeg2jpeg,filter_units--enable-filter=scale,volume --host-cc="C:/msys64/mingw64/bin/gcc.exe" --extra-libs="-ldatachannel -ljuice -lssl -lcrypto -lsrtp2 -lusrsctp -lz -lcrypt32 -lws2_32 -liphlpapi -lbcrypt -lgcc_eh -lgcc -lstdc++fs -lstdc++"  --disable-ffprobe --enable-ffmpeg --enable-ffplay --disable-doc --disable-htmlpages --disable-manpages --disable-podpages --disable-runtime-cpudetect --disable-debug --disable-safe-bitstream-reader
make V=99 -j 8
make install
```
